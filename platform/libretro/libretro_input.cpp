/**
 * libretro_input.cpp -- turning frontend input into something LOVE understands.
 *
 * Three devices reach a game here: the keyboard, the pad, and a pointer.
 *
 * The core does NOT translate the pad into key presses, and that is a decision
 * rather than an omission. Most .love games read the keyboard while the player
 * holds a pad, so something has to bridge the two -- but on Recalbox that
 * something already exists: pad2key maps a pad to keys per game, from a .p2k.cfg
 * next to the rom, and reaches us as real key events through the frontend's
 * keyboard callback. Doing it here as well would mean two mappings for one pad,
 * one of them needing a rebuild to change. The frontend's is better placed: it is
 * per game, and the player owns it.
 *
 * What is left is what only the core can do: hand LOVE the pad as a pad
 * (love.joystick, see src/modules/joystick/libretro), and build a pointer for the
 * games written around a mouse.
 *
 * libretro reports input as *state*, not events: it says "A is held", never "A
 * was just pressed". The keypressed/keyreleased edges LOVE needs are recovered by
 * diffing this frame's state against the last one, which is what update() does.
 */

#include "libretro_state.h"
#include "libretro_options.h"

#include <cmath>
#include <cstring>

namespace love {
namespace libretro {

namespace {

// Set by the frontend's keyboard callback. Kept separate from the pad-derived
// state so that releasing a pad button cannot clear a key the player is
// physically holding on a real keyboard, and vice versa.
bool keyboard_keys[State::NUM_KEYS] = {};

// Defined below, next to the rest of the pointer code.
void update_pointer();

} // anonymous namespace

void keyboard_callback(bool down, unsigned keycode, uint32_t /*character*/,
                       uint16_t /*key_modifiers*/)
{
	if (keycode < (unsigned) State::NUM_KEYS)
		keyboard_keys[keycode] = down;
}

void update_input()
{
	if (!state.input_state_cb)
		return;

	// Last frame's state is what we diff against to find the edges.
	std::memcpy(state.keys_prev, state.keys_now, sizeof(state.keys_now));
	std::memset(state.keys_now, 0, sizeof(state.keys_now));

	// The keyboard, which is every key that reaches us -- a real one, or the one
	// pad2key synthesises from the player's pad.
	//
	// Read twice, from two independent sources, because neither is reliable
	// alone. SET_KEYBOARD_CALLBACK is the event-driven one, but RetroArch only
	// delivers it in "Game Focus" mode -- outside it, the keys go to the
	// frontend's own hotkeys and the core hears nothing. Polling
	// RETRO_DEVICE_KEYBOARD has no such condition. A key held down reads true
	// from whichever source is working, and true OR true is still true, so
	// merging them costs nothing when both work.
	for (int i = 0; i < State::NUM_KEYS; i++)
	{
		state.keys_now[i] = keyboard_keys[i]
		                 || state.input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, i) != 0;
	}

	// Select sends escape, and this one mapping is hard-wired rather than left to
	// pad2key -- because pad2key cannot deliver it.
	//
	// Escape is the key a .love game almost universally uses to open its pause
	// menu or to step back out of one (Sienna has no other way in at all). But on
	// Recalbox RetroArch binds escape to BOTH input_enable_hotkey and
	// input_exit_emulator, so a .p2k.cfg pressing escape satisfies the two at once
	// and closes the emulator before the core ever sees the key. The player gets
	// one frame of the pause menu and then finds themselves back in the frontend.
	//
	// The core sits on the far side of that interception, so it can hand the game
	// an escape RetroArch never gets a say in. Select is the button for it: it is
	// what a pause/back button is for, and no .love game reads a pad's Select
	// natively -- love.joystick still reports the button as pressed either way, so
	// nothing is taken away from a game that does.
	if (state.input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
		state.keys_now[RETROK_ESCAPE] = true;

	update_pointer();
}

// --- Pointer -------------------------------------------------------------

namespace {

void update_pointer()
{
	// Speed 0 means the player did not ask for a core cursor, and then the core
	// must not touch the pointer at all -- not even to clamp it. A game that
	// keeps its own pad cursor watches the system mouse for movement to decide
	// whether the player has reached for one; nudging it here, however slightly,
	// is what makes that game's cursor disappear. Leaving the state untouched
	// also leaves mouse_dx/dy at zero, so no mousemoved event goes out either.
	if (option_pointer_speed() <= 0.0)
	{
		state.mouse_dx = 0.0;
		state.mouse_dy = 0.0;

		// The buttons still track, so love.mouse.isDown and mousepressed keep
		// working for a frontend that really has a mouse. Only the position is
		// left alone.
		std::memcpy(state.mouse_prev, state.mouse_now, sizeof(state.mouse_now));
		std::memset(state.mouse_now, 0, sizeof(state.mouse_now));

		state.mouse_now[1] = state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
		                                          RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
		state.mouse_now[2] = state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
		                                          RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
		state.mouse_now[3] = state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
		                                          RETRO_DEVICE_ID_MOUSE_MIDDLE) != 0;
		return;
	}

	// Start the cursor in the middle of the screen rather than in a corner: a
	// pointer the player has to drag out of (0,0) before it is any use is a
	// pointer that looks broken.
	//
	// Re-centred whenever the resolution changes, not just once, because the
	// core boots at a provisional size and only learns the game's real one a
	// moment later. Centring on the provisional size put the cursor at 320,240
	// in a 320x240 game -- the bottom-right corner, where the clamp pinned it
	// and it could not be moved down or right at all.
	static unsigned centred_w = 0;
	static unsigned centred_h = 0;
	if (state.width > 0 && state.height > 0
	    && (state.width != centred_w || state.height != centred_h))
	{
		state.mouse_x = state.width  * 0.5;
		state.mouse_y = state.height * 0.5;
		centred_w = state.width;
		centred_h = state.height;
	}

	const double prev_x = state.mouse_x;
	const double prev_y = state.mouse_y;

	// The left analog stick, as a velocity. A stick is a displacement, not a
	// position: pushing it halfway should move the cursor steadily, not jump it
	// to the middle of the screen. So its value is read as pixels-per-frame and
	// integrated, which is how every console that fakes a pointer with a stick
	// does it.
	const int16_t ax = state.input_state_cb(0, RETRO_DEVICE_ANALOG,
	                                        RETRO_DEVICE_INDEX_ANALOG_LEFT,
	                                        RETRO_DEVICE_ID_ANALOG_X);
	const int16_t ay = state.input_state_cb(0, RETRO_DEVICE_ANALOG,
	                                        RETRO_DEVICE_INDEX_ANALOG_LEFT,
	                                        RETRO_DEVICE_ID_ANALOG_Y);

	// A stick at rest never reads exactly zero, and without a dead zone the
	// cursor drifts across the screen on its own. 15% is the usual figure.
	constexpr double DEAD_ZONE = 0.15;
	constexpr double AXIS_MAX  = 32767.0;

	double nx = ax / AXIS_MAX;
	double ny = ay / AXIS_MAX;

	const double magnitude = std::sqrt(nx * nx + ny * ny);
	if (magnitude > DEAD_ZONE)
	{
		// Rescale so the cursor starts from a standstill at the edge of the dead
		// zone rather than jumping to 15% speed the moment it is crossed, and
		// square the result: a gentle push then gives fine control and a full
		// push still crosses the screen quickly. A linear stick is either too
		// slow to cross the screen or too coarse to point at anything.
		const double scaled = (magnitude - DEAD_ZONE) / (1.0 - DEAD_ZONE);
		const double speed  = scaled * scaled * option_pointer_speed();

		nx /= magnitude;
		ny /= magnitude;

		state.mouse_x += nx * speed;
		state.mouse_y += ny * speed;
	}

	// A real mouse, if the frontend has one, moves the same cursor. libretro
	// reports it as a per-frame delta, so it simply adds in.
	//
	// On Recalbox this is also how pad2key's cursor arrives: its "mouse.moves"
	// and "mouse.move.*" actions drive a uinput mouse, which reaches us here as
	// ordinary deltas. Adding rather than assigning is what lets the two coexist
	// -- the stick and p2k push the same cursor instead of each snapping it to a
	// position of its own, which looked like the pointer vanishing the moment the
	// stick was touched.
	state.mouse_x += state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
	                                      RETRO_DEVICE_ID_MOUSE_X);
	state.mouse_y += state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
	                                      RETRO_DEVICE_ID_MOUSE_Y);

	// Clamp to the game's own resolution: the cursor is in game pixels, which is
	// what LOVE reports and what the game draws with.
	if (state.mouse_x < 0.0) state.mouse_x = 0.0;
	if (state.mouse_y < 0.0) state.mouse_y = 0.0;
	if (state.mouse_x > state.width)  state.mouse_x = state.width;
	if (state.mouse_y > state.height) state.mouse_y = state.height;

	state.mouse_dx = state.mouse_x - prev_x;
	state.mouse_dy = state.mouse_y - prev_y;

	// Buttons. The frontend's mouse buttons work when there is a mouse; the pad
	// button the player chose is folded in on top, so a click is reachable from
	// the pad alone.
	std::memcpy(state.mouse_prev, state.mouse_now, sizeof(state.mouse_now));
	std::memset(state.mouse_now, 0, sizeof(state.mouse_now));

	state.mouse_now[1] = state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
	                                          RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
	state.mouse_now[2] = state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
	                                          RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
	state.mouse_now[3] = state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
	                                          RETRO_DEVICE_ID_MOUSE_MIDDLE) != 0;

	const unsigned click_button = option_pointer_click_button();
	if (click_button != NO_PAD_BUTTON
	    && state.input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, click_button))
		state.mouse_now[1] = true;
}

} // anonymous namespace

bool mouse_pressed(int button)
{
	if (button < 0 || button >= State::NUM_MOUSE_BUTTONS)
		return false;
	return state.mouse_now[button] && !state.mouse_prev[button];
}

bool mouse_released(int button)
{
	if (button < 0 || button >= State::NUM_MOUSE_BUTTONS)
		return false;
	return !state.mouse_now[button] && state.mouse_prev[button];
}

bool mouse_down(int button)
{
	if (button < 0 || button >= State::NUM_MOUSE_BUTTONS)
		return false;
	return state.mouse_now[button];
}

bool key_pressed(int key)
{
	if (key < 0 || key >= State::NUM_KEYS)
		return false;
	return state.keys_now[key] && !state.keys_prev[key];
}

bool key_released(int key)
{
	if (key < 0 || key >= State::NUM_KEYS)
		return false;
	return !state.keys_now[key] && state.keys_prev[key];
}

bool key_down(int key)
{
	if (key < 0 || key >= State::NUM_KEYS)
		return false;
	return state.keys_now[key];
}

} // namespace libretro
} // namespace love
