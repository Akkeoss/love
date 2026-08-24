/**
 * libretro_core.cpp -- LOVE as a libretro core.
 *
 * The pivot this whole port turns on: LOVE's boot function is already a
 * coroutine that yields once per frame (see the resume loop in src/love.cpp),
 * and love.run already returns a closure that runs exactly one frame. Neither
 * was designed for libretro, but together they are precisely its shape.
 *
 * So we do not restructure LOVE's main loop. We take it over:
 *
 *   retro_load_game()  -> build the Lua state, preload love, start the boot
 *                         coroutine (everything runlove() does up to the loop)
 *   retro_run()        -> resume that coroutine exactly once = one frame
 *   retro_unload_game()-> close the Lua state
 *
 * The frontend owns the window, the GL context, the clock and the audio buffer,
 * so LOVE's SDL/OpenAL backends are replaced by libretro ones (see
 * src/modules/<mod>/libretro/). Those backends read the state this file publishes.
 *
 * Rendering is hardware: we ask for a GL context through SET_HW_RENDER and the
 * frontend hands us an FBO to draw into. Nothing is ever passed to video_cb as
 * pixels -- we signal a finished frame with RETRO_HW_FRAME_BUFFER_VALID.
 */

#include <libretro.h>

#include "libretro_state.h"
#include "libretro_options.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <chrono>

#ifdef __linux__
#include <cstdlib>      // setenv
#include <sys/stat.h>   // mkdir
#endif

// ---------------------------------------------------------------------------
// Frontend callbacks
// ---------------------------------------------------------------------------

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;

static retro_log_printf_t         log_cb;

// The shared state the LOVE backends read from. Defined in libretro_state.cpp.
love::libretro::State love::libretro::state;

namespace {

void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
	(void) level;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

struct retro_hw_render_callback hw_render;

// Set once the frontend has given us a live GL context. Until then LOVE must
// not touch GL -- which is why the Lua state is not built in retro_load_game
// but on the first context_reset.
bool gl_context_ready = false;

// The game path the frontend gave us, kept for the deferred boot.
std::string game_path;
bool game_loaded = false;

// The geometry the frontend was last told about by retro_get_system_av_info.
// The resize check in retro_run starts from this rather than from zero, so a
// game whose size we already reported correctly (see peek_game_size) does not
// trigger a pointless -- and expensive -- SET_SYSTEM_AV_INFO on its first frame.
unsigned reported_w = 800;
unsigned reported_h = 600;

// Push the fps the player chose into the shared state, and derive the game's dt
// from it. Both are read elsewhere: state.fps by retro_get_system_av_info (what
// the frontend paces us at), state.dt by love._libretro_dt (what update() gets).
// Call after every options_update.
void apply_fps()
{
	double fps = love::libretro::option_fps();
	if (fps <= 0.0)
		fps = 60.0988;

	love::libretro::state.fps = fps;
	love::libretro::state.dt  = 1.0 / fps;

	// Same moment, same reason: push the player's render scale where the window
	// backend can read it. A change only takes hold when the game next calls
	// love.window.setMode -- in practice, when the content is restarted.
	love::libretro::state.render_scale = love::libretro::option_render_scale();
}

// --- Hitch reporting ------------------------------------------------------
//
// A core that holds a good average frame rate can still stutter, and the average
// is exactly where that hides: one frame in two hundred costing 300ms is
// invisible in a mean and unmissable to a player. So time every frame, and log
// the ones that overran the budget the core itself asked the frontend for.
//
// Kept in the shipping core rather than behind a debug build: it costs one clock
// read per frame, says nothing while frames are on time, and turns "it stutters
// sometimes" into a frame number and a breakdown -- from a player running a
// normal build, which is the only place some of these problems appear. Rate-
// limited so a genuinely heavy stretch does not flood the frontend's log.
void report_hitch(double ms)
{
	// Below this multiple of the frame budget, a frame is simply a frame.
	// 1.5x is late enough to have missed its slot on any frontend, and loose
	// enough that ordinary jitter stays quiet.
	constexpr double HITCH_FACTOR = 1.5;
	constexpr int    MAX_LOGGED   = 20;   // individual lines before summarising

	static int    logged     = 0;
	static int    suppressed = 0;
	static double worst_ms   = 0.0;

	// retro_init sets log_cb (to the frontend's logger or the stderr fallback), so
	// in a normal lifecycle it is never null here. Checked anyway because this is
	// the one place in the file that would dereference it blind, and a frontend
	// that ever calls retro_run out of order would crash on a diagnostic -- the
	// worst possible thing to crash on.
	if (!log_cb)
		return;

	const double budget_ms = 1000.0 / love::libretro::state.fps;
	if (ms <= budget_ms * HITCH_FACTOR)
		return;

	if (ms > worst_ms)
		worst_ms = ms;

	if (logged < MAX_LOGGED)
	{
		logged++;
		log_cb(RETRO_LOG_WARN,
		       "[LOVE] slow frame: %.1fms (budget %.1fms) -- update %.1f draw %.1f "
		       "present %.1f, reste %.1f\n",
		       ms, budget_ms,
		       love::libretro::state.frame_update_ms,
		       love::libretro::state.frame_draw_ms,
		       love::libretro::state.frame_present_ms,
		       ms - love::libretro::state.frame_update_ms
		          - love::libretro::state.frame_draw_ms
		          - love::libretro::state.frame_present_ms);
		if (logged == MAX_LOGGED)
			log_cb(RETRO_LOG_WARN,
			       "[LOVE] further slow frames will only be counted, not listed\n");
		return;
	}

	// Past the cap: keep counting, and report in batches so the information is
	// still there without the noise.
	if (++suppressed % 100 == 0)
		log_cb(RETRO_LOG_WARN, "[LOVE] %d more slow frames (worst %.1fms)\n",
		       suppressed, worst_ms);
}

// --- The frame step -------------------------------------------------------
//
// dt is the nominal 1/fps, fixed. The frontend owns the clock: one retro_run()
// is one frame, and one frame is 1/fps of game time by definition. Audio renders
// exactly one frame's worth per call for the same reason (see render_audio), and
// the two have to agree or the music drifts away from the action.
//
// Measuring the real interval instead -- so a board that cannot hold 60fps plays
// at the right speed rather than in slow motion -- was tried and removed: it
// duplicates pacing the frontend already does, it feeds back on itself (the
// measured interval contains the render time, which depends on the previous dt),
// and it desynchronises the simulation from the audio. A board that cannot keep
// up runs slower than realtime, which is honest and steady.
void reset_frame_step()
{
	love::libretro::state.dt = 1.0 / love::libretro::state.fps;
}

void context_reset()
{
	log_cb(RETRO_LOG_INFO, "[LOVE] context_reset: GL context is live\n");
	gl_context_ready = true;

	love::libretro::state.gl_get_proc_address = hw_render.get_proc_address;
	love::libretro::state.gl_get_framebuffer  = hw_render.get_current_framebuffer;

	// Booting LOVE is deferred to here: love.graphics initialises OpenGL during
	// boot, and there is no context to initialise against until this point.
	// A frontend may also reset the context mid-run (window resize, driver
	// change), in which case LOVE has to be brought back up from scratch.
	if (!love::libretro::boot(game_path))
		log_cb(RETRO_LOG_ERROR, "[LOVE] boot failed\n");

	// A (re)boot may have changed the target fps; keep the step in step with it.
	reset_frame_step();
}

void context_destroy()
{
	log_cb(RETRO_LOG_INFO, "[LOVE] context_destroy\n");

	// Tear LOVE down while the context is still current, or it releases GL
	// objects with nothing bound.
	love::libretro::shutdown();

	gl_context_ready = false;
	love::libretro::state.gl_get_proc_address = nullptr;
	love::libretro::state.gl_get_framebuffer  = nullptr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// libretro API
// ---------------------------------------------------------------------------

RETRO_API void retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;

	// LOVE without a game shows its "nogame" screen, which is a perfectly good
	// thing to boot into -- and it makes the core testable with no content.
	bool no_game = true;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

	// Ask for real keyboard events on top of the RetroPad. Not every frontend
	// offers them (and RetroArch only delivers them in "Game Focus" mode), so a
	// refusal here is not an error -- the pad mapping still gives every game a
	// usable keyboard.
	struct retro_keyboard_callback kbcb;
	kbcb.callback = love::libretro::keyboard_callback;
	cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kbcb);

	// Tell the frontend what the pad buttons do, so RetroArch's input remapper
	// shows meaningful names instead of "Button A".
	static const struct retro_input_descriptor desc[] =
	{
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left (arrow key)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up (arrow key)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down (arrow key)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right (arrow key)" },
		// These say "keyboard key" rather than a specific key, because the key each
		// button sends is now a core option the player sets (see options). Naming a
		// fixed key here would just be a lie once they change it.
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Keyboard key (see options)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Keyboard key (see options)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Keyboard key (see options)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Keyboard key (see options)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Keyboard key (see options)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Keyboard key (see options)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Keyboard key (see options)" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Keyboard key (see options)" },
		{ 0 },
	};
	cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *) desc);

	// One core option per action button, letting the player pick the key it
	// sends. Different .love games want different keys (Mr. Rescue wants s/d/a,
	// the defaults are z/x/...), and RetroArch's own control remapper cannot help
	// here: it maps buttons to buttons, and has no idea what key a game listens
	// for. Only the core knows that, so only the core can expose the choice.
	love::libretro::options_set(cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)  { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)    { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)        { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)      { input_state_cb = cb; }

RETRO_API void retro_init()
{
	struct retro_log_callback logging;
	if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
		log_cb = logging.log;
	else
		log_cb = fallback_log;

	love::libretro::state.log = log_cb;

	const char *dir = nullptr;
	if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
		love::libretro::state.system_dir = dir;
	if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
		love::libretro::state.save_dir = dir;

#ifdef __linux__
	// Give the GL driver's on-disk shader cache a writable home.
	//
	// Mesa only keeps its shader cache when it can find a writable cache
	// directory; a frontend running without a usable HOME silently loses it, and
	// then EVERY session recompiles EVERY shader from scratch -- a stall the
	// first time each shader is used. Pointing the cache at the save directory
	// makes those compiles a one-time cost per board.
	if (!love::libretro::state.save_dir.empty())
	{
		std::string cache = love::libretro::state.save_dir + "/shader_cache";
		mkdir(cache.c_str(), 0755);
		setenv("MESA_SHADER_CACHE_DIR", cache.c_str(), 0);
		setenv("MESA_GLSL_CACHE_DIR", cache.c_str(), 0);   // pre-20.x Mesa
		setenv("MESA_SHADER_CACHE_MAX_SIZE", "64M", 0);

		// Put the game's saves where the FRONTEND says saves go.
		//
		// LOVE writes to its own standard location -- on Linux,
		// $XDG_DATA_HOME/<identity>, i.e. ~/.local/share/<identity> when that is
		// unset (Filesystem::getAppdataDirectory). That is right for a desktop
		// LOVE and wrong here: a libretro frontend owns save placement, tells the
		// core where via GET_SAVE_DIRECTORY, and a player looking for their save
		// looks there. On a Recalbox that is /recalbox/share/saves/love, not a
		// hidden .local/share the share does not even expose the same way.
		//
		// Setting XDG_DATA_HOME redirects it without patching LOVE: it is the
		// documented knob for exactly this, and love.filesystem reads it at
		// init -- which happens later, inside the boot coroutine, so setting it
		// here is early enough. Games keep their own identity as the subfolder
		// name, so two games never collide.
		//
		// The PARENT of the save directory, not the save directory itself. LOVE
		// always appends its own "love/" component (LOVE_APPDATA_FOLDER) below
		// whatever appdata directory it is given, so pointing this straight at
		// /recalbox/share/saves/love would land the game in
		// .../saves/love/love/<identity>. Handing it the parent makes LOVE's own
		// "love" the frontend's "love" folder, and the game lands exactly where a
		// player expects: /recalbox/share/saves/love/<identity>.
		//
		// If the save directory is not named "love" (another frontend, another
		// layout), the parent trick would put saves in a sibling folder rather
		// than in the one we were given -- so it is only used when the last
		// component really is LOVE's own folder name. Otherwise fall back to the
		// directory as given, which is still inside the frontend's save area.
		//
		// overwrite=1, unlike the cache vars above: the frontend's answer is the
		// authority on where saves live, and an inherited XDG_DATA_HOME (the
		// desktop's own, say) would put them somewhere the player is not looking.
		// "love" is LOVE_APPDATA_FOLDER on Linux (modules/filesystem/Filesystem.h).
		// Spelled out rather than included: that header pulls in the filesystem
		// module, and this file deliberately knows nothing about LOVE's modules.
		const std::string &sd = love::libretro::state.save_dir;
		const size_t slash = sd.find_last_of('/');
		const bool ends_in_love =
			slash != std::string::npos
			&& sd.compare(slash + 1, std::string::npos, "love") == 0;
		const std::string xdg = ends_in_love ? sd.substr(0, slash) : sd;
		setenv("XDG_DATA_HOME", xdg.c_str(), 1);
	}
#endif
}

RETRO_API void retro_deinit()
{
	love::libretro::shutdown();

	// Take back everything the frontend is holding a pointer to.
	//
	// retro_deinit is the last call before dlclose, and dlclose unmaps this
	// library. Anything of ours the frontend still has -- a function pointer, a
	// string it did not copy -- becomes a dangling pointer at that moment, and
	// RetroArch does keep using some of it while it shuts its own subsystems
	// down. The symptom is a SIGSEGV *after* every "unloading core" line in the
	// log, with the kernel pointing at an address inside this .so:
	//
	//     love_libretro.s[27884]: segfault at 7fbd2c929000 ip 00007fbd2c929000
	//         error 15 in love_libretro.so[0,7fbd2c929000+40000]
	//
	// -- ip at offset 0 of our first (read-only, non-executable) segment: a jump
	// through a pointer that no longer means anything.
	//
	// So hand back empty versions of both things we registered:
	//
	//   * the keyboard callback (SET_KEYBOARD_CALLBACK, set in retro_set_
	//     environment) is a pointer straight into this library's code;
	//   * the input descriptors (SET_INPUT_DESCRIPTORS) are static strings that
	//     live in it.
	//
	// A null callback and a terminator-only descriptor array are both valid
	// values for those calls, so this is a supported way to say "forget what I
	// told you" rather than a trick.
	if (environ_cb)
	{
		struct retro_keyboard_callback nokb = {};
		environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &nokb);

		static const struct retro_input_descriptor nodesc[] = {{ 0, 0, 0, 0, NULL }};
		environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *) nodesc);

		// And the core options, which are the largest set of our strings the
		// frontend holds: every key, label, category and value in
		// libretro_options.cpp is a literal in this library. RetroArch reads them
		// back when it writes retroarch-core-options.cfg -- which its own log
		// shows it doing right after "Unloading core symbols", i.e. after the
		// unmap. Handing it a null option set drops those pointers first.
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, NULL);
	}
}

RETRO_API unsigned retro_api_version()
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
	std::memset(info, 0, sizeof(*info));
	info->library_name     = "LOVE";
	info->library_version  = LOVE_LIBRETRO_VERSION;
	info->valid_extensions = "love|zip";

	// LOVE mounts the game through PhysFS from its path, so we need the path on
	// disk rather than a buffer of bytes.
	info->need_fullpath    = true;
	info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	std::memset(info, 0, sizeof(*info));

	// This is the first, provisional geometry: LOVE has not booted yet, so the
	// real size is not known and state.width/height are still the defaults. max is
	// reported equal to base so the framebuffer starts exactly the right size for
	// the CRT case (see the resize block in retro_run). The instant LOVE boots and
	// reports its actual resolution, SET_SYSTEM_AV_INFO reallocates to it.
	info->geometry.base_width   = love::libretro::state.width;
	info->geometry.base_height  = love::libretro::state.height;
	info->geometry.max_width    = love::libretro::state.width;
	info->geometry.max_height   = love::libretro::state.height;
	info->geometry.aspect_ratio = (float) love::libretro::state.width
	                            / (float) love::libretro::state.height;

	reported_w = love::libretro::state.width;
	reported_h = love::libretro::state.height;

	info->timing.fps            = love::libretro::state.fps;
	info->timing.sample_rate    = love::libretro::SAMPLE_RATE;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
	// A .love is a zip; LOVE also runs an unpacked directory. Either way we hand
	// the path to PhysFS and let it work out which.
	game_path = (game && game->path) ? game->path : "";

	if (game_path.empty())
		log_cb(RETRO_LOG_INFO, "[LOVE] no content -- booting the nogame screen\n");
	else
		log_cb(RETRO_LOG_INFO, "[LOVE] game: %s\n", game_path.c_str());

	// Ask the frontend for a context, best first, and take what it gives.
	//
	// One binary has to serve every board this might run on, and they do not agree
	// on what GL they have: a PC offers desktop GL 3.3 and no GLES worth using; a
	// Pi offers GLES 3 and no desktop GL at all. Rather than shipping a core per
	// board, negotiate -- the frontend rejects what it cannot provide, so trying
	// in order of preference lands on the best available.
	//
	// LOVE needs no build flag for this. glad loads whichever entry points exist
	// and LOVE branches on GLAD_ES_VERSION_* at runtime, so the same code drives
	// both. That is the whole reason a single binary is possible.
	struct ContextChoice
	{
		retro_hw_context_type type;
		unsigned major, minor;
		const char *name;
	};

	// Order matters: the first choice the frontend accepts is the one we get.
	// Desktop GL first, GLES for the ARM boards, GL 2.1 as a last resort.
	//
	// LOVE itself does not care which it gets: glad loads whichever entry points
	// the context exposes and LOVE branches on GLAD_ES_VERSION_* at runtime.
	static const ContextChoice CHOICES[] =
	{
		{ RETRO_HW_CONTEXT_OPENGL_CORE, 3, 3, "OpenGL 3.3 core" },   // desktop
		{ RETRO_HW_CONTEXT_OPENGLES3,   3, 0, "OpenGL ES 3.0"   },   // ARM boards
		{ RETRO_HW_CONTEXT_OPENGL,      2, 1, "OpenGL 2.1"      },   // last resort
	};

	bool got_context = false;

	for (const ContextChoice &choice : CHOICES)
	{
		std::memset(&hw_render, 0, sizeof(hw_render));
		hw_render.context_type       = choice.type;
		hw_render.version_major      = choice.major;
		hw_render.version_minor      = choice.minor;
		hw_render.context_reset      = context_reset;
		hw_render.context_destroy    = context_destroy;
		hw_render.depth              = true;
		hw_render.stencil            = true;   // love.graphics.stencil needs it
		hw_render.bottom_left_origin = true;   // GL's convention, and LOVE's

		if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
		{
			log_cb(RETRO_LOG_INFO, "[LOVE] context: %s\n", choice.name);
			got_context = true;
			break;
		}
	}

	if (!got_context)
	{
		log_cb(RETRO_LOG_ERROR,
			"[LOVE] frontend offers no usable OpenGL context -- cannot run\n");
		return false;
	}

	// Read the initial options now that the frontend has taken the definitions.
	// Without this first read they stay at their compiled-in defaults until the
	// player happens to open the options menu. Reading fps here in particular
	// matters, because retro_get_system_av_info -- called right after this -- will
	// report state.fps to the frontend.
	love::libretro::options_update(environ_cb);
	apply_fps();

	// Find out the game's real resolution BEFORE the frontend asks for it.
	//
	// This is the difference between the game booting once and booting twice.
	// retro_get_system_av_info runs immediately after this and its answer is what
	// the frontend allocates -- but LOVE has not booted (it cannot: no GL context
	// until context_reset), so that answer used to be the 800x600 default. The
	// first frame revealing the true size then forced a SET_SYSTEM_AV_INFO, which
	// makes RetroArch rebuild the GL context and reboot LOVE from scratch. A
	// player's log showed it plainly: "booted" and "loaded save" twice.
	//
	// The size is in conf.lua, plain Lua that runs long before any GL work
	// (love.conf precedes love.window.setMode in boot.lua). A wrong guess costs
	// exactly what the old behaviour cost; a right one saves an entire boot.
	// Deliberately NOT called -- see the note on peek_game_size itself.
	//
	// Reading conf.lua up front removes the second boot, and it works. But on a
	// 15 kHz CRT it breaks the picture: announcing the game's size before the
	// frontend has a video mode means the geometry change that follows is a
	// SET_GEOMETRY rather than a SET_SYSTEM_AV_INFO, and only the latter makes
	// RetroArch rebuild the GL context -- which is what leaves KMS and the
	// modeline agreeing with each other. Without it the launcher renders shifted
	// down with a black band at the top, verified on real hardware both ways.
	//
	// The modeline itself is identical in both cases (1024x488i, Y scale 0.635),
	// which is why reading the log alone points at the wrong culprit. What
	// differs is the context rebuild. So the double boot stays: ~0.4s of
	// duplicated work at launch against a picture that is simply wrong.
	// love::libretro::peek_game_size(game_path);

	// LOVE itself is booted in context_reset, not here: there is no GL context
	// yet, and love.graphics cannot come up without one.
	game_loaded = true;
	return true;
}

RETRO_API void retro_unload_game()
{
	love::libretro::shutdown();
	game_loaded = false;
	game_path.clear();
}

RETRO_API void retro_run()
{
	input_poll_cb();

	if (!gl_context_ready || !love::libretro::is_running())
	{
		// Nothing to show yet. Repeating the previous frame is the honest
		// signal here; a black frame would look like a rendering bug.
		video_cb(nullptr, love::libretro::state.width,
		         love::libretro::state.height, 0);
		return;
	}

	// Re-read the button mapping if the player changed it mid-game. The frontend
	// raises this flag rather than making us poll every option every frame.
	bool options_changed = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_changed)
	    && options_changed)
	{
		love::libretro::options_update(environ_cb);
		apply_fps();
	}

	// Publish this frame's input so the libretro input backends can read it
	// during the frame LOVE is about to run.
	love::libretro::state.input_state_cb = input_state_cb;

	// Sample it exactly once, here, before LOVE runs. Doing it per-query instead
	// would let the state change mid-frame -- a game that checks isDown("left")
	// twice in one update could get two different answers.
	love::libretro::update_input();

	// One resume of LOVE's boot coroutine == exactly one frame, timed so a hitch
	// leaves a trace in the frontend's log. See report_hitch().
	const auto frame_start = std::chrono::steady_clock::now();
	const bool frame_ok = love::libretro::run_frame();
	report_hitch(std::chrono::duration<double>(
		std::chrono::steady_clock::now() - frame_start).count() * 1000.0);

	if (!frame_ok)
	{
		// We get here only when LOVE really cannot continue -- a Lua error, or the
		// boot script ending -- not when a game merely calls love.event.quit
		// (our love.run swallows that; the player leaves through the frontend).
		// A dead Lua state cannot render, so the only sane thing left is to close
		// the core rather than spin on it.
		log_cb(RETRO_LOG_INFO, "[LOVE] cannot continue -- shutting down\n");
		environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
		video_cb(nullptr, love::libretro::state.width,
		         love::libretro::state.height, 0);
		return;
	}

	// Did the game resize itself this frame?
	//
	// love.window.setMode() is a normal thing for a game to call, and the window
	// backend records the new size -- but the frontend has no way of knowing
	// unless it is told. Without this, a game that switches to 1024x768 renders
	// at 1024x768 into a framebuffer the frontend still believes is 800x600, and
	// the player gets a cropped or stretched picture with nothing to explain it.
	//
	// SET_SYSTEM_AV_INFO, not SET_GEOMETRY. The difference is the whole fix for
	// the CRT-SwitchRes offset:
	//
	// SET_GEOMETRY only re-crops within the existing framebuffer, whose size is
	// the *max* we first reported. We used to report a 1920x1080 max, so the
	// framebuffer stayed 1920x1080 while a game drew into an 800x600 corner of it.
	// On HDMI the frontend crops to base and centres, so it looked fine. On a CRT,
	// crtswitchres builds its modeline from that oversized framebuffer and the
	// image ends up pushed down the screen.
	//
	// SET_SYSTEM_AV_INFO lets us set max = base, which makes the frontend
	// reallocate the framebuffer to exactly the game's size -- no empty margin for
	// either display to misframe. libretro documents this as the way to support
	// configurable resolutions without guessing a worst-case max up front.
	//
	// But it is EXPENSIVE, in a way that is easy to miss locally: RetroArch tears
	// the GL context down and builds it again, which fires context_destroy /
	// context_reset -- and our context_reset reboots LOVE from scratch. The game
	// loads twice, visibly (a Recalbox log shows "booted" and "loaded save" twice,
	// ~0.4s of work for this game). The headless tester does not recreate the
	// context on SET_SYSTEM_AV_INFO, which is exactly why this never showed up in
	// testing and only appeared in a log from real hardware.
	//
	// It was once paid only when the game OUTGREW the framebuffer, on the theory
	// that a size which fits can be re-cropped for free with SET_GEOMETRY. That
	// theory is wrong on a CRT, and Mr. Rescue is the counter-example: it renders
	// 768x600 inside the 800x600 default, fits comfortably, took the cheap path --
	// and came out with a black band at the top on 15 kHz, because crtswitchres
	// builds its modeline from the ALLOCATED buffer, not from base. 32 unused
	// pixels of width were enough. HDMI hid it by cropping to base and centring.
	//
	// So the condition is exact match, not fit: whenever the game's size differs
	// from what is allocated, reallocate. That restores the rule the July CRT fix
	// established -- the framebuffer is always exactly the game's size, never a
	// buffer with margin -- which is the property the whole fix rests on.
	//
	// SET_GEOMETRY remains for the case where nothing needs reallocating, which
	// after this is only a change of aspect ratio at an unchanged size.
	{
		// Seeded, not zeroed. These start at whatever retro_get_system_av_info
		// reported. Starting them at 0 made the first frame look like a change
		// from nothing, firing the expensive path even for a game that runs at
		// exactly the size already announced.
		//
		// With peek_game_size disabled (see retro_load_game) that seed is the
		// 800x600 default, so a game declaring another size does pay one
		// SET_SYSTEM_AV_INFO at launch -- deliberately, because the context
		// rebuild it triggers is what keeps the picture right on a 15 kHz CRT.
		static unsigned last_w = reported_w;
		static unsigned last_h = reported_h;
		static unsigned fbo_w  = reported_w;   // what the frontend has allocated
		static unsigned fbo_h  = reported_h;

		const unsigned w = love::libretro::state.width;
		const unsigned h = love::libretro::state.height;

		if ((w != last_w || h != last_h) && w > 0 && h > 0)
		{
			// Resize the framebuffer whenever it does not already match the game
			// exactly -- not merely when the game outgrows it.
			//
			// The cheap path (SET_GEOMETRY, re-crop inside the buffer we have)
			// was written to avoid the context rebuild for a game that FITS. It
			// does avoid it, and on HDMI the result is indistinguishable: the
			// frontend crops to base and centres. On a 15 kHz CRT it is not.
			// crtswitchres derives its modeline from the ALLOCATED framebuffer,
			// so a game rendering 768x600 inside an 800x600 buffer gets a
			// modeline for 800x600 and a picture pushed off-centre -- Mr. Rescue,
			// 32px of unused width, a black band at the top on real hardware.
			//
			// This is the same defect the July fix addressed for an oversized
			// max (1920x1080), reappearing at a size small enough that "it fits"
			// looked like a reason not to pay. The rule that actually holds is
			// the one that fix established: the framebuffer is sized to the game,
			// always. So the exact-match test replaces the outgrows test.
			//
			// The cost is one context rebuild per resolution change rather than
			// zero. A game changes resolution rarely (usually once, at boot), and
			// a rebuild is ~0.4s of reboot against a picture that is simply wrong
			// on an entire class of display.
			const bool needs_fbo_resize = (w != fbo_w || h != fbo_h);

			struct retro_game_geometry geom;
			std::memset(&geom, 0, sizeof(geom));
			geom.base_width   = w;
			geom.base_height  = h;
			geom.max_width    = needs_fbo_resize ? w : fbo_w;
			geom.max_height   = needs_fbo_resize ? h : fbo_h;
			geom.aspect_ratio = (float) w / (float) h;

			if (needs_fbo_resize)
			{
				struct retro_system_av_info av;
				std::memset(&av, 0, sizeof(av));
				av.geometry           = geom;
				av.timing.fps         = love::libretro::state.fps;
				av.timing.sample_rate = love::libretro::SAMPLE_RATE;

				environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);

				fbo_w = w;
				fbo_h = h;
				log_cb(RETRO_LOG_INFO, "[LOVE] geometry: %ux%u (fbo resized)\n", w, h);
			}
			else
			{
				environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
				log_cb(RETRO_LOG_INFO, "[LOVE] geometry: %ux%u (within %ux%u)\n",
				       w, h, fbo_w, fbo_h);
			}

			last_w = w;
			last_h = h;
		}
	}

	// Pull this frame's audio out of LOVE and hand it to the frontend.
	love::libretro::render_audio(audio_batch_cb);

	// The frame is in the FBO the frontend gave us; there are no pixels to hand
	// over, only the fact that it is ready.
	video_cb(RETRO_HW_FRAME_BUFFER_VALID,
	         love::libretro::state.width,
	         love::libretro::state.height, 0);
}

RETRO_API void retro_reset()
{
	log_cb(RETRO_LOG_INFO, "[LOVE] reset\n");
	love::libretro::shutdown();
	if (gl_context_ready)
		love::libretro::boot(game_path);
}

// ---------------------------------------------------------------------------
// Unimplemented API surface
//
// Save states are the notable gap: snapshotting a running Lua VM plus its GPU
// resources is a research project, not an oversight. Rewind and netplay are out
// of reach for the same reason.
// ---------------------------------------------------------------------------

RETRO_API size_t retro_serialize_size() { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { (void) data; (void) size; return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { (void) data; (void) size; return false; }

RETRO_API void retro_cheat_reset() {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{ (void) index; (void) enabled; (void) code; }

RETRO_API bool retro_load_game_special(unsigned type,
	const struct retro_game_info *info, size_t num)
{ (void) type; (void) info; (void) num; return false; }

RETRO_API void *retro_get_memory_data(unsigned id) { (void) id; return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void) id; return 0; }

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{ (void) port; (void) device; }

RETRO_API unsigned retro_get_region() { return RETRO_REGION_NTSC; }
