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

#include "common/config.h"
#include "common/version.h"
#include "common/runtime.h"
#include "modules/love/love.h"
#include "libraries/physfs/physfs.h"

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
}

} // anonymous namespace

bool is_running()
{
	return running && boot_co != nullptr;
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
		"          -- love.quit is still called, so a game can run its own cleanup;\n"
		"          -- its return value is simply ignored, and the loop continues.\n"
		"          if love.quit then love.quit() end\n"
		"        else\n"
		"          love.handlers[name](a,b,c,d,e,f)\n"
		"        end\n"
		"      end\n"
		"    end\n"
		"    if love.timer then love.timer.step() end\n"
		"    if love.update then love.update(dt) end\n"
		"    if love.graphics and love.graphics.isActive() then\n"
		"      love.graphics.origin()\n"
		"      love.graphics.clear(love.graphics.getBackgroundColor())\n"
		"      if love.draw then love.draw() end\n"
		"      love.graphics.present()\n"
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

	// Put the love table back, since the code below expects it there.
	lua_getglobal(L, "love");

	lua_pop(L, 1);   // pop the love table

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
}

} // namespace libretro
} // namespace love
