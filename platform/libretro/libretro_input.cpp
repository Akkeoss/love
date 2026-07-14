/**
 * libretro_input.cpp -- turning frontend input into something LOVE understands.
 *
 * The problem this file solves: nearly every .love game reads the keyboard
 * (love.keyboard.isDown("left"), love.keypressed), while a Recalbox player is
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

#include <cstring>

namespace love {
namespace libretro {

namespace {

// RetroPad -> key. This is the mapping a player gets with no configuration, so
// it aims at the conventions .love games actually use rather than at elegance:
//
//   D-pad      -> arrow keys      (universal)
//   A          -> z, and space    (the two usual "confirm/jump" keys)
//   B          -> x, and lshift   (the usual "cancel/run" keys)
//   X / Y      -> c / v           (secondary actions)
//   START      -> return          (menus)
//   SELECT     -> escape          (pause/back; note this does NOT quit the core)
//   L / R      -> q / e           (shoulder actions)
//
// A single button can map to several keys: pressing A holds down both z and
// space, because one game listens for one and another game for the other, and
// there is no way to know which from here. The cost is that a game reading both
// sees both -- rare, and far less bad than a game that does not respond at all.
struct PadKeyMap
{
	unsigned pad_button;
	int keys[3];       // 0-terminated
};

const PadKeyMap PAD_KEYS[] =
{
	{ RETRO_DEVICE_ID_JOYPAD_UP,     { RETROK_UP,     0 } },
	{ RETRO_DEVICE_ID_JOYPAD_DOWN,   { RETROK_DOWN,   0 } },
	{ RETRO_DEVICE_ID_JOYPAD_LEFT,   { RETROK_LEFT,   0 } },
	{ RETRO_DEVICE_ID_JOYPAD_RIGHT,  { RETROK_RIGHT,  0 } },

	{ RETRO_DEVICE_ID_JOYPAD_A,      { RETROK_z,      RETROK_SPACE,  0 } },
	{ RETRO_DEVICE_ID_JOYPAD_B,      { RETROK_x,      RETROK_LSHIFT, 0 } },
	{ RETRO_DEVICE_ID_JOYPAD_X,      { RETROK_c,      0 } },
	{ RETRO_DEVICE_ID_JOYPAD_Y,      { RETROK_v,      0 } },

	{ RETRO_DEVICE_ID_JOYPAD_START,  { RETROK_RETURN, 0 } },
	{ RETRO_DEVICE_ID_JOYPAD_SELECT, { RETROK_ESCAPE, 0 } },

	{ RETRO_DEVICE_ID_JOYPAD_L,      { RETROK_q,      0 } },
	{ RETRO_DEVICE_ID_JOYPAD_R,      { RETROK_e,      0 } },
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
	for (const PadKeyMap &m : PAD_KEYS)
	{
		if (!state.input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, m.pad_button))
			continue;

		for (int i = 0; i < 3 && m.keys[i] != 0; i++)
			state.keys_now[m.keys[i]] = true;
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
