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

#define LOVE_LIBRETRO_VERSION "1.5"

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
	double   fps    = 60.0988;

	// Render scale (1.0 = native). Applied to the size a game requests via
	// love.window.setMode: the game lays out and renders at the reduced size and
	// the frontend upscales the finished frame. Every full-screen pass (canvases,
	// post-processing) shrinks with it, which is the whole point -- it is the one
	// lever against a GPU/fill-bound game on a weak board. Set from a core option.
	double render_scale = 1.0;

	// The GL context lives in the frontend. LOVE resolves its GL entry points
	// through get_proc_address, and must render into the FBO that
	// get_framebuffer returns -- it is NOT framebuffer 0, and it can change from
	// frame to frame, so it has to be re-queried every frame rather than cached.
	retro_hw_get_proc_address_t   gl_get_proc_address = nullptr;
	retro_hw_get_current_framebuffer_t gl_get_framebuffer = nullptr;

	// --- Input ---------------------------------------------------------
	// Valid only during retro_run(); the input backends read it from there.
	retro_input_state_t input_state_cb = nullptr;

	// Rumble, when the frontend offers it. Null means it does not, and
	// love.joystick's vibration calls then answer honestly that they did nothing.
	retro_set_rumble_state_t set_rumble = nullptr;

	// Which ports the frontend says have a device on them.
	//
	// libretro has no "is a pad plugged in" query, but it does tell us: the
	// frontend calls retro_set_controller_port_device for every port, and sends
	// RETRO_DEVICE_NONE for the empty ones. Without recording that, the only
	// honest answer left is "all four are connected", and a game with a
	// player-count menu then offers four players when one pad is plugged in.
	//
	// Port 0 starts true because a frontend is not obliged to call at all (the
	// spec says JOYPAD is assumed), and a core that reports no pad in that case
	// would be worse than one that reports a pad too many.
	static constexpr int NUM_PORTS = 4;
	bool port_connected[NUM_PORTS] = { true, false, false, false };

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

	// Pointer state, in game pixels.
	//
	// A .love game written for a desktop expects a pointer it can move to an
	// arbitrary spot -- Balatro is played entirely that way -- and a player on a
	// Recalbox box has a pad and nothing else. So the left analog stick drives a
	// cursor the core maintains here, and the frontend's own mouse (if there is
	// one) moves the same cursor. The game cannot tell which moved it.
	//
	// Absolute, unlike libretro's mouse, which only ever reports per-frame
	// deltas: the position has to be accumulated somewhere, and it belongs here
	// rather than in the mouse backend, so that the stick and the mouse agree on
	// one cursor instead of keeping two.
	double mouse_x = 0.0;
	double mouse_y = 0.0;

	// How far the cursor moved this frame, for love.mousemoved's dx/dy.
	double mouse_dx = 0.0;
	double mouse_dy = 0.0;

	// Mouse buttons, indexed by LOVE's numbering (1 = left, 2 = right,
	// 3 = middle); index 0 is unused so the numbers read as they do in Lua.
	// now/prev for the same reason as the keys: libretro reports state, and the
	// mousepressed/mousereleased edges have to be recovered by diffing.
	static constexpr int NUM_MOUSE_BUTTONS = 4;
	bool mouse_now[NUM_MOUSE_BUTTONS]  = {};
	bool mouse_prev[NUM_MOUSE_BUTTONS] = {};

	// --- Timing --------------------------------------------------------
	// The frontend paces us, so dt is a constant derived from fps rather than a
	// measured wall-clock delta. Feeding LOVE the real elapsed time would make
	// the game speed up whenever the frontend runs the core faster than
	// realtime (fast-forward, or a headless test).
	double dt = 1.0 / 60.0988;

	// --- Frame breakdown (diagnostic) ----------------------------------
	// Filled by the injected love.run each frame: how long the game spent in
	// update(), in draw(), and in present(). Reported alongside a slow frame so
	// a spike says WHERE it went instead of only how long it was. Costs three
	// clock reads a frame; the numbers are meaningless until the first frame has
	// run, which is why they start at zero.
	double frame_update_ms  = 0.0;
	double frame_draw_ms    = 0.0;
	double frame_present_ms = 0.0;

	// What the renderer actually did this frame, from love.graphics.getStats.
	// Reported next to a slow frame so a heavy draw says whether it was many
	// small submissions (draw calls, canvas and shader switches) or few large
	// ones -- which is the difference between a CPU-side submission cost and a
	// GPU-side fill cost, and they have opposite fixes.
	int frame_draw_calls      = 0;
	int frame_canvas_switches = 0;
	int frame_shader_switches = 0;

	// --- Paths ---------------------------------------------------------
	std::string system_dir;
	std::string save_dir;

	// --- Logging -------------------------------------------------------
	retro_log_printf_t log = nullptr;
};

extern State state;

// Read the game's intended window size out of its conf.lua and publish it in
// state.width/height, WITHOUT booting LOVE or touching GL.
//
// Called from retro_load_game, before the frontend asks for the geometry. The
// point is to get the first answer right: a size corrected later costs a
// SET_SYSTEM_AV_INFO, and RetroArch answers that by recreating the GL context,
// which reboots LOVE from scratch. Does nothing if the game has no conf.lua or
// does not set a size -- the defaults then stand, exactly as before.
//
// CURRENTLY NOT CALLED. It works, and it does remove the second boot, but it
// breaks the picture on a 15 kHz CRT: see the call site in retro_load_game for
// why the context rebuild that the second boot brings is what keeps KMS and the
// modeline in agreement. Kept here rather than deleted so the next attempt
// starts from working code and the note that goes with it.
void peek_game_size(const std::string &game_path);

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

// The same edges for the mouse buttons, in LOVE's numbering (1 = left).
bool mouse_pressed(int button);
bool mouse_released(int button);
bool mouse_down(int button);

// --- Audio (libretro_audio.cpp) ------------------------------------------

// Pull one frame's worth of audio out of LOVE and hand it to the frontend.
void render_audio(retro_audio_sample_batch_t audio_batch_cb);

} // namespace libretro
} // namespace love
