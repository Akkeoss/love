/**
 * libretro_state.h -- the contract between the libretro core and LOVE's backends.
 *
 * The core (libretro_core.cpp) talks to the frontend; the backends
 * (src/modules/<mod>/libretro/) implement LOVE's interfaces. Neither should include
 * the other's headers, so everything they share goes through this one struct.
 *
 * Read it as: "what the frontend is currently telling us", published once per
 * frame by the core and consumed by the backends during that frame.
 */

#pragma once

#include <libretro.h>

#include <string>

namespace love {
namespace libretro {

#define LOVE_LIBRETRO_VERSION "0.1"

// The FBO is allocated once at this size and never grown, so it has to cover
// anything a game might ask for. 1080p is the ceiling on the boards we target.
constexpr unsigned MAX_WIDTH  = 1920;
constexpr unsigned MAX_HEIGHT = 1080;

// libretro fixes the sample rate for the lifetime of the core, so LOVE's audio
// backend has to resample to this rather than pick its own.
constexpr double SAMPLE_RATE = 44100.0;

struct State
{
	// --- Video ---------------------------------------------------------
	// Current frame size. LOVE may change it (love.window.setMode), in which
	// case the core tells the frontend via SET_GEOMETRY.
	unsigned width  = 800;
	unsigned height = 600;
	double   fps    = 60.0;

	// The GL context lives in the frontend. LOVE resolves its GL entry points
	// through get_proc_address, and must render into the FBO that
	// get_framebuffer returns -- it is NOT framebuffer 0, and it can change from
	// frame to frame, so it has to be re-queried every frame rather than cached.
	retro_hw_get_proc_address_t   gl_get_proc_address = nullptr;
	retro_hw_get_current_framebuffer_t gl_get_framebuffer = nullptr;

	// --- Input ---------------------------------------------------------
	// Valid only during retro_run(); the input backends read it from there.
	retro_input_state_t input_state_cb = nullptr;

	// Keyboard state, indexed by libretro's retro_key (RETROK_*).
	//
	// Two things write into this: the frontend's keyboard callback (a real key on
	// a real keyboard) and the RetroPad mapping (a gamepad button pretending to be
	// a key). They are deliberately merged into one array rather than kept apart,
	// because a game asking love.keyboard.isDown("left") should not care which
	// device the press came from -- and most .love games only ever read the
	// keyboard, so a pad that cannot produce key presses would be a pad that
	// cannot play them.
	//
	// `now` is this frame, `prev` is last frame: the difference is what produces
	// the keypressed/keyreleased events, since libretro reports state and not
	// edges.
	static constexpr int NUM_KEYS = 324;   // RETROK_LAST
	bool keys_now[NUM_KEYS]  = {};
	bool keys_prev[NUM_KEYS] = {};

	// --- Timing --------------------------------------------------------
	// The frontend paces us, so dt is a constant derived from fps rather than a
	// measured wall-clock delta. Feeding LOVE the real elapsed time would make
	// the game speed up whenever the frontend runs the core faster than
	// realtime (fast-forward, or a headless test).
	double dt = 1.0 / 60.0;

	// --- Paths ---------------------------------------------------------
	std::string system_dir;
	std::string save_dir;

	// --- Logging -------------------------------------------------------
	retro_log_printf_t log = nullptr;
};

extern State state;

// Bring LOVE up: build the Lua state, preload the modules, start the boot
// coroutine. Must be called with a live GL context. Returns false if LOVE
// failed to start.
bool boot(const std::string &game_path);

// Resume LOVE's boot coroutine once -- exactly one frame. Returns false when
// LOVE has quit (or errored), meaning there are no more frames to run.
bool run_frame();

// True between a successful boot() and the frame LOVE quits on.
bool is_running();

// Close the Lua state. Safe to call when not running.
void shutdown();

// --- Input (libretro_input.cpp) ------------------------------------------

// Sample the frontend's input once, at the top of the frame: fold the RetroPad
// into key state and roll this frame's state into the previous one. Everything
// below reads what this publishes.
void update_input();

// Handed to the frontend via SET_KEYBOARD_CALLBACK.
void keyboard_callback(bool down, unsigned keycode, uint32_t character,
                       uint16_t key_modifiers);

// Edges, recovered by diffing this frame against the last. libretro only reports
// state, so this is the only place keypressed/keyreleased can come from.
bool key_pressed(int key);
bool key_released(int key);
bool key_down(int key);

// --- Audio (libretro_audio.cpp) ------------------------------------------

// Pull one frame's worth of audio out of LOVE and hand it to the frontend.
void render_audio(retro_audio_sample_batch_t audio_batch_cb);

} // namespace libretro
} // namespace love
