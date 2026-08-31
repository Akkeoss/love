/**
 * libretro_boot.cpp -- driving LOVE's boot coroutine one frame at a time.
 *
 * This mirrors runlove() in src/love.cpp, with one change that is the whole
 * point of the port. runlove() does:
 *
 *     while (luax_resume(L, 0, &nres) == LUA_YIELD)
 *         lua_pop(L, nres);
 *
 * i.e. it drives the boot coroutine to completion in a loop it owns. A libretro
 * core cannot own a loop -- the frontend calls retro_run() once per frame and
 * expects to get control back. So we keep the coroutine alive across calls and
 * resume it exactly once per frame.
 *
 * LOVE needs no modification for this to work: love.boot already yields once
 * per frame, because love.run returns a one-frame closure that boot.lua calls
 * and then yields after. We are only taking over who turns the crank.
 */

#include "libretro_state.h"

#include "graphics/opengl/Graphics.h"
#include "libretro_options.h"

#include "common/config.h"
#include "common/version.h"
#include "common/runtime.h"
#include "modules/love/love.h"
#include "libraries/physfs/physfs.h"

// For SDL_Quit at teardown -- see shutdown().
#include <SDL.h>

extern "C" {
	#include <lua.h>
	#include <lualib.h>
	#include <lauxlib.h>
}

#include <cstring>
#include <cstdarg>
#include <cstdio>

namespace love {
namespace libretro {

namespace {

lua_State *L = nullptr;     // the main Lua state
lua_State *boot_co = nullptr; // the boot coroutine we resume each frame
bool running = false;

// The directory that physically holds the .love (or the unpacked game folder).
// Mounted alongside the game so a data file dropped next to the .love shows up in
// love.filesystem -- see mount_game_directory() for why.
std::string game_dir;
bool game_dir_mounted = false;

// Where love.boot's coroutine sits on L's stack. Kept so we can pop whatever a
// yield leaves behind, exactly as runlove() does.
int co_stackpos = 0;

// Same helper as the one in src/love.cpp, which is static there and so cannot
// be reused. It only pokes a C function into package.preload.
int love_preload(lua_State *L, lua_CFunction f, const char *name)
{
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "preload");
	lua_pushcfunction(L, f);
	lua_setfield(L, -2, name);
	lua_pop(L, 2);
	return 0;
}

// love._libretro_frame_times(update, draw, present) -- called once per frame by
// the injected love.run. The three values (in ms) land in state, where
// report_hitch reads them back to say WHERE a slow frame went.
int l_frame_times(lua_State *ls)
{
	state.frame_update_ms  = luaL_optnumber(ls, 1, 0.0);
	state.frame_draw_ms    = luaL_optnumber(ls, 2, 0.0);
	state.frame_present_ms = luaL_optnumber(ls, 3, 0.0);
	state.frame_draw_calls      = (int) luaL_optnumber(ls, 4, 0);
	state.frame_canvas_switches = (int) luaL_optnumber(ls, 5, 0);
	state.frame_shader_switches = (int) luaL_optnumber(ls, 6, 0);
	return 0;
}

void log(enum retro_log_level level, const char *fmt, ...)
{
	if (!state.log)
		return;
	va_list ap;
	va_start(ap, fmt);
	// retro_log_printf_t is printf-shaped but variadic; forward through a buffer
	// since there is no vlog in the libretro log interface.
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	state.log(level, "%s", buf);
}

// The directory part of a path, or "." when there is no separator. Handles both
// separators so it is correct whichever platform packaged the path.
std::string parent_directory(const std::string &path)
{
	size_t slash = path.find_last_of("/\\");
	if (slash == std::string::npos)
		return ".";
	if (slash == 0)
		return "/";           // path is at the filesystem root
	return path.substr(0, slash);
}

// Mount the .love's own directory at the root of LOVE's search path.
//
// Some .love games look for user-supplied data by scanning the filesystem root
// (love.filesystem.getDirectoryItems("")) rather than asking for a path -- e.g. a
// game that imports a ROM or a save the player drops in. PhysFS only ever sees the
// .love archive and the save directory, so such a file is invisible unless we make
// its directory part of the search path.
//
// The frontend hands a .love to a core the same way it hands a game to any other
// core: as a single file the player put in a roms folder. The natural place for
// that player to drop a companion file is right next to it -- so we mount the
// .love's parent directory. It is appended (appendToPath = 1), so the game's own
// files always win a name clash; the extra directory only adds names the game did
// not already provide. Read-only is fine and in fact safer: that folder may live on
// a read-only medium, and LOVE still writes to its save directory as before.
//
// PhysFS is not initialised until LOVE's boot coroutine has run, so this is done
// lazily on the first frame rather than in boot().
void mount_game_directory()
{
	if (game_dir_mounted || game_dir.empty() || !PHYSFS_isInit())
		return;

	// One shot regardless of outcome: if the mount fails there is nothing to
	// retry, and we must not spam PhysFS every frame.
	game_dir_mounted = true;

	if (PHYSFS_mount(game_dir.c_str(), nullptr, 1) == 0)
	{
		log(RETRO_LOG_WARN, "[LOVE] could not mount game directory %s: %s\n",
			game_dir.c_str(),
			PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
	}
	else
	{
		log(RETRO_LOG_INFO, "[LOVE] mounted game directory %s\n",
			game_dir.c_str());
	}
}

// Run the game's love.quit once, if it has one.
//
// A game that spawns love.thread workers stops them in love.quit -- it pushes a
// "quit" message onto their channel and joins them (love does not join threads for
// you; they are detached). Normally love.quit runs when the game itself quits, but
// this core suppresses that path on purpose: the player leaves through the frontend,
// not by the game closing itself. So at teardown we have to run love.quit ourselves,
// before lua_close, or a worker still sitting in love.timer.sleep wakes up after the
// state and SDL have been torn down and faults. Stock love has the same hazard on
// exit (see the comment in runlove()); it only gets away with it because the process
// exits immediately after. A core does not: the frontend keeps running.
//
// Protected and best-effort: a game with no love.quit, or one that errors in it, must
// not turn teardown into a crash of its own.
void run_quit_handler()
{
	if (!L)
		return;

	// Exactly once per Lua state. shutdown() can be reached twice in a normal
	// RetroArch exit -- context_destroy tears LOVE down, then retro_deinit calls
	// shutdown() again -- and a second love.quit would re-enter cleanup that has
	// already run: stopping threads that are gone, waiting on nil channels,
	// releasing what was released. The first call does the work; the second must
	// be a no-op, not a repeat.
	static lua_State *quit_ran_for = nullptr;
	if (quit_ran_for == L)
		return;
	quit_ran_for = L;

	lua_getglobal(L, "love");
	if (lua_istable(L, -1))
	{
		lua_getfield(L, -1, "quit");
		if (lua_isfunction(L, -1))
		{
			if (lua_pcall(L, 0, 0, 0) != 0)
			{
				log(RETRO_LOG_WARN, "[LOVE] love.quit error at teardown: %s\n",
					lua_tostring(L, -1));
				lua_pop(L, 1);
			}
		}
		else
		{
			lua_pop(L, 1);   // love.quit was not a function
		}
	}
	lua_pop(L, 1);           // love

	// Safety net: say "quit" on the named channels.
	//
	// love.quit is the normal path, and it is enough when the game stops its
	// workers there. But it only stops the ones it knows about, and it may do
	// nothing at all (measured on a real Recalbox: love.quit returned 0 with not
	// one thread stopping). The remaining threads are then killed by lua_close in
	// the middle of their loop -- and a half-dead thread runs unmapped code once
	// the frontend dlclose()s the core.
	//
	// The convention love.thread workers follow -- including the ones measured
	// here -- is to read a command off a named channel and stop on "quit". So we
	// send them exactly that before lua_close. A worker that does not expect this
	// convention simply ignores one more value in its queue; one that follows it
	// leaves its loop and terminates cleanly.
	//
	// ACCEPTED DEBT: the two names below are one game's (Gen1Recomp), not a
	// general list. This is the only place in the port that knows a game by name,
	// and another game with different channel names would not be covered -- it
	// would fall back on love.quit, which is enough when the game does its job.
	//
	// There is no simple way to do better: LOVE exposes no enumeration of named
	// channels (love.thread.getChannel creates them on demand, and there is no
	// getChannels), so "push quit on EVERY channel" is not writable in Lua from
	// here at all. A real fix would go through C++, on the ThreadModule side, or
	// through a generalised thread:wait() -- real work, for a benefit limited to
	// games that spawn workers AND forget to stop them.
	//
	// Then let a few empty frames run: a thread has to be scheduled to see the
	// message. Without that pause we would push the quit and close Lua right
	// after, which would change nothing.
	static const char *STOP_WORKERS =
		"if not (love and love.thread) then return end\n"
		"local ok, err = pcall(function()\n"
		"  for _, name in ipairs({'chipaudio_cmd', 'update_cmd'}) do\n"
		"    local ch = love.thread.getChannel(name)\n"
		"    if ch then ch:push({ cmd = 'quit' }) end\n"
		"  end\n"
		"end)\n"
		"if love.timer then\n"
		"  local t0 = love.timer.getTime()\n"
		"  while love.timer.getTime() - t0 < 0.20 do love.timer.sleep(0.005) end\n"
		"end\n";
	if (luaL_loadstring(L, STOP_WORKERS) == 0)
		lua_pcall(L, 0, 0, 0);
	lua_pop(L, lua_gettop(L) > 0 ? 1 : 0);
}

} // anonymous namespace

bool is_running()
{
	return running && boot_co != nullptr;
}

void peek_game_size(const std::string &game_path)
{
	if (game_path.empty())
		return;   // nogame screen: the defaults are the answer

	// PhysFS is the only thing here that knows how to look inside a .love, and a
	// .love is just a zip. It is not initialised yet (boot() does that later), so
	// bring it up, read the one file, and put it straight back down -- boot()
	// must find the same clean slate it always did.
	//
	// argv0 is "love", the same string boot() puts in arg[0] and hands to
	// love.filesystem.init. NOT nullptr: PhysFS then tries to work out the
	// executable's path by itself, which inside a frontend's process is at best
	// a different answer than the one LOVE will compute a moment later, and at
	// worst a failure that leaves a half-built state behind. A fixed, known
	// string keeps this pass identical to the real init that follows it.
	if (PHYSFS_isInit())
		return;   // something else owns PhysFS; do not disturb it
	if (!PHYSFS_init("love"))
		return;

	std::string conf;
	if (PHYSFS_mount(game_path.c_str(), nullptr, 1) != 0)
	{
		PHYSFS_File *f = PHYSFS_openRead("conf.lua");
		if (f)
		{
			PHYSFS_sint64 len = PHYSFS_fileLength(f);
			// A conf.lua is a few kilobytes; anything huge is not one, and reading
			// an arbitrary amount here would be careless.
			if (len > 0 && len < 1024 * 1024)
			{
				conf.resize((size_t) len);
				PHYSFS_sint64 got = PHYSFS_readBytes(f, &conf[0], (PHYSFS_uint64) len);
				if (got != len)
					conf.clear();
			}
			PHYSFS_close(f);
		}
	}
	PHYSFS_deinit();

	if (conf.empty())
		return;

	// A scratch Lua state with the standard library only. conf.lua is ordinary
	// Lua that runs before any love module exists -- love.conf's whole job is to
	// fill in a table -- so nothing here needs love, a window or a GL context.
	lua_State *cl = luaL_newstate();
	if (!cl)
		return;
	luaL_openlibs(cl);

	// A `love` table for the chunk to hang love.conf on, carrying the fields a
	// conf.lua may reasonably read while deciding a size (some branch on the OS,
	// or on the version they were written for).
	lua_newtable(cl);
	lua_pushstring(cl, love::VERSION);
	lua_setfield(cl, -2, "_version");
	lua_pushinteger(cl, love::VERSION_MAJOR);
	lua_setfield(cl, -2, "_version_major");
	lua_pushinteger(cl, love::VERSION_MINOR);
	lua_setfield(cl, -2, "_version_minor");
	lua_pushinteger(cl, love::VERSION_REV);
	lua_setfield(cl, -2, "_version_revision");

	// A conf.lua runs before love.filesystem is up in stock LOVE too, but it may
	// still CALL into it -- love.filesystem.setSymlinksEnabled is the documented
	// example, and the game this was written against opens with exactly that.
	// An unhandled call there aborts the chunk before it reaches t.window, which
	// silently defeats the whole point of reading it. So stub the handful of
	// no-result setters a conf.lua legitimately uses: they configure state this
	// throwaway pass has no interest in, and the real boot calls them again for
	// real a moment later.
	lua_newtable(cl);                       // love.filesystem
	lua_pushcfunction(cl, [](lua_State *) -> int { return 0; });
	lua_setfield(cl, -2, "setSymlinksEnabled");
	lua_setfield(cl, -2, "filesystem");

	lua_setglobal(cl, "love");

	// Everything below is best-effort: a conf.lua that errors, expects modules we
	// have not provided, or simply does not set a size leaves the defaults alone.
	// That is the same outcome as not doing this at all, which is why none of it
	// is treated as a failure worth reporting.
	if (luaL_loadbuffer(cl, conf.c_str(), conf.size(), "@conf.lua") == 0
		&& lua_pcall(cl, 0, 0, 0) == 0)
	{
		lua_getglobal(cl, "love");
		lua_getfield(cl, -1, "conf");
		if (lua_isfunction(cl, -1))
		{
			// love.conf(t) mutates the table it is handed, so build t, keep one
			// reference to read back afterwards, and pass the other to the call.
			// Stack order matters: pcall consumes the function and its argument, so
			// the copy that survives has to sit BELOW them.
			lua_newtable(cl);              // t (the one we keep)
			lua_newtable(cl);              // t.window
			lua_setfield(cl, -2, "window");
			lua_newtable(cl);              // t.modules -- commonly written to
			lua_setfield(cl, -2, "modules");
			lua_insert(cl, -2);            // ... t, conf
			lua_pushvalue(cl, -2);         // ... t, conf, t

			if (lua_pcall(cl, 1, 0, 0) == 0)
			{
				lua_getfield(cl, -1, "window");
				if (lua_istable(cl, -1))
				{
					lua_getfield(cl, -1, "width");
					lua_getfield(cl, -2, "height");
					int w = (int) lua_tointeger(cl, -2);
					int h = (int) lua_tointeger(cl, -1);
					lua_pop(cl, 2);

					// Sanity: a plausible window, not a stray value. The upper bound is
					// generous -- some games open very large windows -- but a size the
					// frontend cannot allocate would be worse than guessing wrong.
					if (w >= 64 && h >= 64 && w <= 8192 && h <= 8192)
					{
						// Apply the player's render scale here too, or the frontend
						// would be told one size and handed another the moment
						// love.window.setMode runs -- which is the very reallocation
						// this is here to avoid. Same rounding as Window::setWindow.
						double scale = state.render_scale;
						if (scale > 0.0 && scale < 1.0)
						{
							w = ((int) (w * scale)) & ~1;
							h = ((int) (h * scale)) & ~1;
							if (w < 2) w = 2;
							if (h < 2) h = 2;
						}

						state.width  = (unsigned) w;
						state.height = (unsigned) h;
						log(RETRO_LOG_INFO,
							"[LOVE] conf.lua declares %ux%u -- reporting it up front\n",
							state.width, state.height);
					}
				}
				lua_pop(cl, 1);   // window
			}
		}
	}

	lua_close(cl);
}

bool boot(const std::string &game_path)
{
	if (L)
		shutdown();

	// Remember where the game lives so its directory can be mounted once PhysFS is
	// up (see mount_game_directory). An empty game_path is the nogame screen, which
	// has no directory to mount.
	game_dir = game_path.empty() ? "" : parent_directory(game_path);
	game_dir_mounted = false;

	L = luaL_newstate();
	if (!L)
	{
		log(RETRO_LOG_ERROR, "[LOVE] could not create Lua state\n");
		return false;
	}
	luaL_openlibs(L);

	// LuaJIT setup has to happen before anything loads external library code,
	// same as in runlove().
	love_preload(L, luaopen_love_jitsetup, "love.jitsetup");
	lua_getglobal(L, "require");
	lua_pushstring(L, "love.jitsetup");
	lua_call(L, 1, 0);

	// LuaJIT, only if the player asked for it.
	//
	// love's jitsetup.lua ends with jit.off() on arm/arm64, and this used to
	// undo it unconditionally, reasoning that interpreted Lua on a board is
	// simply slower than compiled. That reasoning misreads why upstream turns it
	// off. Their comment is explicit: on ARM, LuaJIT can only allocate machine
	// code within a short branch reach, SDL can take that space, and compilation
	// then "both fail and take a long time". A compiler that retries and fails
	// is not slower on average -- it stalls, which is exactly the shape of the
	// problem players report on heavy games: frames costing hundreds of
	// milliseconds rather than a uniformly lower rate.
	//
	// The evidence that settled it: the official native ARM64 build of a heavy
	// LOVE game ships this same liblove 11.5 with jitsetup's jit.off() intact,
	// nothing re-enables it, and it runs smoothly on the same class of board
	// where this core stuttered with the JIT forced on.
	//
	// So the default is now what upstream and the native builds use -- off --
	// and turning it on is the player's call, because which way wins depends on
	// the board and on what the game asks of the compiler. jitsetup's pool
	// reservation runs either way.
	if (option_jit())
	{
		static const char *JIT_ON =
			"if type(jit) ~= 'table' or not jit.on then return 'nojit' end\n"
			"pcall(jit.on)\n"
			"return jit.status and jit.status() and 'on' or 'off'\n";
		if (luaL_loadstring(L, JIT_ON) == 0 && lua_pcall(L, 0, 1, 0) == 0)
			log(RETRO_LOG_INFO, "[LOVE] LuaJIT: %s\n", lua_tostring(L, -1));

		else
			log(RETRO_LOG_WARN, "[LOVE] could not re-enable LuaJIT: %s\n",
				lua_tostring(L, -1));
		lua_pop(L, 1);
	}
	else
	{
		log(RETRO_LOG_INFO, "[LOVE] LuaJIT: off (jitsetup default)\n");
	}

	// Give up on the JIT if it cannot place its machine code.
	//
	// On ARM64 LuaJIT must allocate mcode within +-128MB of its own VM
	// (LJ_TARGET_JUMPRANGE in lj_arch.h); mcode_alloc probes for such an
	// address and gives up with "failed to allocate mcode memory" when the
	// range is taken. A core is dlopen'd after the frontend, Mesa, SDL and
	// the audio stack are already placed, so that window is far more likely
	// to be full than it is for an executable loaded into an empty address
	// space -- which is why the native ARM build of the same game does not
	// hit this.
	//
	// The failure is not a quiet loss of compilation. The compiler retries:
	// it records a trace, fails to place it, aborts, and the next hot loop
	// starts the whole thing again. Measured on a Pi 5 that shows up as
	// update() taking 462ms, 1247ms, 3990ms with draw() at 8ms -- the GPU
	// idle while LuaJIT spins. jitsetup.lua's own comment says exactly this
	// ("both fail and take a long time"); it is simply far worse in a core.
	//
	// So watch the trace events, and if mcode allocation keeps failing,
	// switch the JIT off once and stop. Interpreted Lua is slower than
	// compiled Lua, and much faster than a compiler that never succeeds.
	// Errors are matched by MESSAGE, not by index: jit.vmdef.traceerr is
	// generated per LuaJIT version and the numbering is not a contract.
	//
	// The error code arrives in the FIFTH callback argument, not the sixth.
	// jit.attach's trace callback is (what, tr, func, pc, otr, oex), and for
	// an 'abort' LuaJIT reuses those last two slots: otr carries the error
	// code and oex its format argument -- which is why jit/dump.lua writes
	// fmterr(otr, oex) and indexes traceerr[otr]. Reading oex instead looks
	// right, compiles, and silently never matches: on a Pi 5 it yielded 51,
	// a bytecode number, while traceerr ends at 31 -- so the guard never
	// fired and the protection was dead code. Verified directly: a pcall in
	// a hot loop reports otr=7 ('NYI: bytecode %s'), oex=51.
	static const char *JIT_GUARD =
		"local ok, vmdef = pcall(require, 'jit.vmdef')\n"
		"if not ok or type(vmdef.traceerr) ~= 'table' then return 'no vmdef' end\n"
		"local watch = {}\n"
		"for i, msg in ipairs(vmdef.traceerr) do\n"
		"  if msg == 'failed to allocate mcode memory'\n"
		"     or msg == 'hit mcode limit (retrying)' then watch[i] = true end\n"
		"end\n"
		"if not next(watch) then return 'no mcode errors in this build' end\n"
		"local hits, tripped = 0, false\n"
		"jit.attach(function(what, tr, func, pc, otr, oex)\n"
		"  if tripped or what ~= 'abort' then return end\n"
		"  if watch[otr] then\n"
		"    hits = hits + 1\n"
		"    if hits >= 32 then\n"
		"      tripped = true\n"
		"      pcall(jit.off)\n"
		"      pcall(jit.flush)\n"
		"      if love and love._libretro_jit_gave_up then\n"
		"        love._libretro_jit_gave_up(hits)\n"
		"      end\n"
		"    end\n"
		"  end\n"
		"end, 'trace')\n"
		"return 'armed'\n";
	if (luaL_loadstring(L, JIT_GUARD) == 0 && lua_pcall(L, 0, 1, 0) == 0)
		log(RETRO_LOG_INFO, "[LOVE] LuaJIT mcode guard: %s\n",
			lua_tostring(L, -1));
	else
		log(RETRO_LOG_WARN, "[LOVE] LuaJIT mcode guard failed: %s\n",
			lua_tostring(L, -1));
	lua_pop(L, 1);

	love_preload(L, luaopen_love, "love");

	// Build the `arg` table LOVE expects. love.arg parses this exactly as it
	// would a command line, so passing the .love path here is all it takes to
	// make LOVE mount and run the game -- no special-casing needed.
	{
		lua_newtable(L);

		lua_pushstring(L, "love");   // argv[0]
		lua_rawseti(L, -2, -2);

		lua_pushstring(L, "embedded boot.lua");
		lua_rawseti(L, -2, -1);

		if (!game_path.empty())
		{
			lua_pushstring(L, game_path.c_str());
			lua_rawseti(L, -2, 1);
		}

		lua_setglobal(L, "arg");
	}

	// require "love"
	lua_getglobal(L, "require");
	lua_pushstring(L, "love");
	lua_call(L, 1, 1);

	// love._exe = true tells LOVE it is the standalone runtime rather than a
	// library embedded in someone else's program. A libretro core is closer to
	// the former: we own the whole lifecycle, and LOVE should behave as it does
	// in the real executable (its own filesystem identity, its own quit path).
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "_exe");

	// love._libretro_dt(): the seconds-per-frame the game's update() should use.
	//
	// It is a function, not a constant, so it always returns the current value --
	// the player can change the target fps mid-game through a core option. And it
	// is derived from that fps rather than measured off a wall clock: the frontend
	// paces us, so feeding LOVE the real elapsed time would make a game speed up
	// whenever the core runs faster than realtime (fast-forward, a headless test).
	lua_pushcfunction(L, [](lua_State *ls) -> int {
		lua_pushnumber(ls, state.dt);
		return 1;
	});
	lua_setfield(L, -2, "_libretro_dt");

	// love._libretro_frame_times(update, draw, present): where this frame's
	// milliseconds went. Read back by report_hitch so a slow frame is reported
	// with its breakdown instead of just its total. See l_frame_times.
	lua_pushcfunction(L, l_frame_times);
	lua_setfield(L, -2, "_libretro_frame_times");

	// love._libretro_jit_gave_up(n): the mcode guard installed above calls this
	// when it has switched the JIT off. Reported once, loudly, because it turns
	// "the game stutters" into a named cause -- and because a player seeing it
	// should know the core is now running interpreted on purpose.
	lua_pushcfunction(L, [](lua_State *ls) -> int {
		const int hits = (int) luaL_optnumber(ls, 1, 0);
		log(RETRO_LOG_WARN,
		    "[LOVE] LuaJIT could not place its machine code (%d failures) -- "
		    "switching it off. This is the ARM 128MB jump-range limit; the core "
		    "now runs interpreted, which is slower per operation but does not "
		    "stall.\n", hits);
		return 0;
	});
	lua_setfield(L, -2, "_libretro_jit_gave_up");

	// The love.run the frontend needs, published as love._libretro_run.
	//
	// boot.lua installs this over whatever love.run the game defined, right
	// before calling it. That override is the point: a game is free to ship its
	// own love.run, and plenty do -- almost every .love from the 0.10 era wraps
	// the main loop in `while true do ... end`. That works in a normal LOVE and
	// deadlocks a core, because retro_run() would be entered once and never come
	// back. The game keeps running; the frontend freezes.
	//
	// This version differs from the stock one in exactly two ways, both forced by
	// libretro: it never sleeps (the frontend paces us), and dt comes from the
	// frontend's frame rate rather than a wall clock -- otherwise the game speeds
	// up whenever the core is run faster than realtime, as it is during
	// fast-forward or a headless test.
	static const char *LIBRETRO_RUN =
		"love._libretro_run = function()\n"
		"  if love.load then love.load(love.arg.parseGameArguments(arg), arg) end\n"
		"  if love.timer then love.timer.step() end\n"
		"  return function()\n"
		"    -- dt is asked for every frame, not captured once: the player can\n"
		"    -- change the target fps mid-game, and this reflects it immediately.\n"
		"    local dt = love._libretro_dt and love._libretro_dt() or (1/60)\n"
		"    if love.event then\n"
		"      love.event.pump()\n"
		"      for name, a,b,c,d,e,f in love.event.poll() do\n"
		"        if name == 'quit' then\n"
		"          -- The game is NOT allowed to close the core. The player leaves\n"
		"          -- through the frontend (a hotkey combo on most setups), so a\n"
		"          -- game quitting itself -- often just because it received\n"
		"          -- escape -- would drop them out of the emulator by surprise.\n"
		"          --\n"
		"          -- And love.quit is NOT called here, deliberately. It is a\n"
		"          -- teardown handler: a game that spawns love.thread workers\n"
		"          -- stops them in it (this game pushes 'quit' to its chip audio\n"
		"          -- and update workers and then worker:wait()s on both). Running\n"
		"          -- it while the game carries on leaves that game alive with its\n"
		"          -- threads shut down and its channels nil -- and then run_frame\n"
		"          -- keeps calling into it. The real teardown runs love.quit for\n"
		"          -- real, exactly once; see run_quit_handler().\n"
		"        else\n"
		"          love.handlers[name](a,b,c,d,e,f)\n"
		"        end\n"
		"      end\n"
		"    end\n"
		"    if love.timer then love.timer.step() end\n"
		"    local _clk = love.timer and love.timer.getTime\n"
		"    local _t0 = _clk and _clk() or 0\n"
		"    if love.update then love.update(dt) end\n"
		"    local _t1 = _clk and _clk() or 0\n"
		"    if love.graphics and love.graphics.isActive() then\n"
		"      love.graphics.origin()\n"
		"      love.graphics.clear(love.graphics.getBackgroundColor())\n"
		"      if love.draw then love.draw() end\n"
		"      local _t2 = _clk and _clk() or 0\n"
		// present() resets the per-frame counters (see Graphics::present), so
		// read them while they still describe the frame just drawn.
		"      local st = love.graphics.getStats and love.graphics.getStats()\n"
		"      love.graphics.present()\n"
		"      local _t3 = _clk and _clk() or 0\n"
		"      if love._libretro_frame_times then\n"

		"        love._libretro_frame_times((_t1-_t0)*1000, (_t2-_t1)*1000,\n"
		"                                   (_t3-_t2)*1000,\n"
		"                                   st and st.drawcalls or 0,\n"
		"                                   st and st.canvasswitches or 0,\n"
		"                                   st and st.shaderswitches or 0,\n"
		"                                   st and st.texturememory or 0)\n"
		"      end\n"
		"    end\n"
		"    -- No love.timer.sleep here: the frontend decides when the next frame\n"
		"    -- happens, and sleeping would just steal time from it.\n"
		"  end\n"
		"end\n";

	// Pop the love table first: the chunk looks love up as a global.
	lua_pop(L, 1);

	if (luaL_loadstring(L, LIBRETRO_RUN) != 0 || lua_pcall(L, 0, 0, 0) != 0)
	{
		log(RETRO_LOG_ERROR, "[LOVE] could not install libretro love.run: %s\n",
			lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	// Nothing to restore: the chunk above reached love through the global, and
	// what follows does the same. The stack is already balanced here.

	// require "love.boot"
	lua_getglobal(L, "require");
	lua_pushstring(L, "love.boot");
	lua_call(L, 1, 1);

	// Turn it into a coroutine -- but unlike runlove(), do not drive it here.
	boot_co = lua_newthread(L);
	lua_pushvalue(L, -2);          // the boot function
	lua_xmove(L, boot_co, 1);      // move it onto the coroutine's own stack
	co_stackpos = lua_gettop(L);

	running = true;
	log(RETRO_LOG_INFO, "[LOVE] %s booted (game: %s)\n",
		love_version(), game_path.empty() ? "none" : game_path.c_str());

	return true;
}

bool run_frame()
{
	if (!is_running())
		return false;

	// PhysFS comes up during the boot coroutine, so the earliest we can add to the
	// search path is here, on the frames after boot() -- not in boot() itself.
	mount_game_directory();

	// The frontend had the GL context since our last frame and drew with it --
	// its own program, buffers, textures and enables. LOVE caches what it last
	// bound and skips a bind it thinks is redundant, so every one of those
	// caches is now describing state that is no longer there. Tell it to stop
	// trusting them before the game draws anything.
	// The module registers under the base graphics::Graphics type, so ask for
	// that and step down to the GL implementation we know is behind it.
	if (auto *gfx = love::Module::getInstance<love::graphics::Graphics>(
			love::Module::M_GRAPHICS))
		((love::graphics::opengl::Graphics *) gfx)->libretroBeginFrame();

	int nres = 0;
	int status = luax_resume(boot_co, 0, &nres);

	if (status == LUA_YIELD)
	{
		// A frame completed and LOVE yielded back to us. Drop whatever it left
		// on the stack so it does not grow without bound across frames.
		lua_pop(boot_co, nres);

		return true;
	}

	if (status == LUA_OK)
	{
		// love.boot returned: the game quit cleanly.
		log(RETRO_LOG_INFO, "[LOVE] quit\n");
		running = false;
		return false;
	}

	// Anything else is an error inside LOVE. Report it -- a silent black screen
	// is the worst possible outcome here.
	const char *err = lua_tostring(boot_co, -1);
	log(RETRO_LOG_ERROR, "[LOVE] runtime error: %s\n", err ? err : "(unknown)");
	running = false;
	return false;
}

void shutdown()
{
	if (!L)
		return;

	// Let the game stop its own threads before we free the state out from under
	// them. See run_quit_handler().
	run_quit_handler();

	lua_close(L);
	L = nullptr;
	boot_co = nullptr;
	running = false;
	co_stackpos = 0;

	// Shut SDL down. The executable never has to: src/love.cpp only calls
	// SDL_Quit on Android and otherwise lets exit() take the whole process
	// apart. A core has no exit() -- the frontend goes on running, and then
	// dlclose()s us.
	//
	// That is the difference that turns a harmless leak into a crash. SDL is
	// initialised behind our backs (love.timer and love.thread both use it) and
	// keeps threads of its own. dlclose unmaps this library's code while those
	// threads are still alive, and the next instruction one of them executes is
	// in memory that no longer exists: SIGSEGV, after every "unloading..." line
	// in the frontend's log, which reads as "the core crashed on exit" with
	// nothing in the log to explain it. Measured: 9 live threads at dlclose.
	//
	// SDL_Quit is reference-counted per subsystem and safe when nothing was
	// initialised, so this costs nothing on the path where SDL was never used.
	SDL_Quit();
}

} // namespace libretro
} // namespace love
