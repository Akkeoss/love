/**
 * libretro_input.cpp -- turning frontend input into something LOVE understands.
 *
 * The problem this file solves: nearly every .love game reads the keyboard
 * (love.keyboard.isDown("left"), love.keypressed), while the player is
 * holding a gamepad. A core that only exposed love.joystick would technically be
 * faithful to libretro and practically useless -- most games would be unplayable.
 *
 * So the RetroPad is translated into key presses, and a real keyboard (which
 * libretro also exposes) is merged into the same state. A game cannot tell the
 * two apart, which is exactly the point. love.joystick still sees the pad
 * natively, for the games that prefer it.
 *
 * libretro reports input as *state*, not events: it says "A is held", never "A
 * was just pressed". The keypressed/keyreleased edges LOVE needs are recovered by
 * diffing this frame's state against the last one, which is what update() does.
 */

#include "libretro_state.h"
#include "libretro_options.h"

#include <cstring>

namespace love {
namespace libretro {

namespace {

// The D-pad is fixed: it maps to the arrow keys, which every game agrees on.
// Making it configurable would only add noise. The action buttons are not here
// -- they go through core options (see libretro_options.cpp), because different
// games want different keys and only the player knows which.
struct PadKeyMap
{
	unsigned pad_button;
	int key;
};

const PadKeyMap DPAD_KEYS[] =
{
	{ RETRO_DEVICE_ID_JOYPAD_UP,    RETROK_UP },
	{ RETRO_DEVICE_ID_JOYPAD_DOWN,  RETROK_DOWN },
	{ RETRO_DEVICE_ID_JOYPAD_LEFT,  RETROK_LEFT },
	{ RETRO_DEVICE_ID_JOYPAD_RIGHT, RETROK_RIGHT },
};

// The action buttons whose key comes from a core option.
const unsigned ACTION_BUTTONS[] =
{
	RETRO_DEVICE_ID_JOYPAD_A,
	RETRO_DEVICE_ID_JOYPAD_B,
	RETRO_DEVICE_ID_JOYPAD_X,
	RETRO_DEVICE_ID_JOYPAD_Y,
	RETRO_DEVICE_ID_JOYPAD_L,
	RETRO_DEVICE_ID_JOYPAD_R,
	RETRO_DEVICE_ID_JOYPAD_START,
	RETRO_DEVICE_ID_JOYPAD_SELECT,
};

// Set by the frontend's keyboard callback. Kept separate from the pad-derived
// state so that releasing a pad button cannot clear a key the player is
// physically holding on a real keyboard, and vice versa.
bool keyboard_keys[State::NUM_KEYS] = {};

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

	// Real keyboard first.
	for (int i = 0; i < State::NUM_KEYS; i++)
		state.keys_now[i] = keyboard_keys[i];

	// Then fold the pad in on top. Port 0 only for now: LOVE games are
	// overwhelmingly single-player on the keyboard, and a second player pressing
	// the same keys would be indistinguishable anyway.

	// D-pad: fixed arrow keys.
	for (const PadKeyMap &m : DPAD_KEYS)
	{
		if (state.input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, m.pad_button))
			state.keys_now[m.key] = true;
	}

	// Action buttons: whatever key the player mapped them to. A button mapped to
	// "none" resolves to key 0, which is skipped.
	for (unsigned button : ACTION_BUTTONS)
	{
		if (!state.input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, button))
			continue;

		int key = option_key_for_button(button);
		if (key > 0 && key < State::NUM_KEYS)
			state.keys_now[key] = true;
	}
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
