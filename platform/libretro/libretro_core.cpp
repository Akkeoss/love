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
}

RETRO_API void retro_deinit()
{
	love::libretro::shutdown();
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

	// One resume of LOVE's boot coroutine == exactly one frame.
	if (!love::libretro::run_frame())
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
	// configurable resolutions without guessing a worst-case max up front. It
	// reinitialises the GL context (context_reset fires again, LOVE reboots), so
	// it is only done when the size actually changes, not every frame.
	{
		static unsigned last_w = 0;
		static unsigned last_h = 0;

		const unsigned w = love::libretro::state.width;
		const unsigned h = love::libretro::state.height;

		if ((w != last_w || h != last_h) && w > 0 && h > 0)
		{
			struct retro_system_av_info av;
			std::memset(&av, 0, sizeof(av));
			av.geometry.base_width   = w;
			av.geometry.base_height  = h;
			av.geometry.max_width    = w;   // = base: framebuffer is exactly the game
			av.geometry.max_height   = h;
			av.geometry.aspect_ratio = (float) w / (float) h;
			av.timing.fps            = love::libretro::state.fps;
			av.timing.sample_rate    = love::libretro::SAMPLE_RATE;

			environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);

			log_cb(RETRO_LOG_INFO, "[LOVE] geometry: %ux%u (fbo resized)\n", w, h);

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
