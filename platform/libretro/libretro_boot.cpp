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

} // anonymous namespace

bool is_running()
{
	return running && boot_co != nullptr;
}

bool boot(const std::string &game_path)
{
	if (L)
		shutdown();

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
		"  local dt = love._libretro_dt or (1/60)\n"
		"  return function()\n"
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

	lua_close(L);
	L = nullptr;
	boot_co = nullptr;
	running = false;
	co_stackpos = 0;
}

} // namespace libretro
} // namespace love
