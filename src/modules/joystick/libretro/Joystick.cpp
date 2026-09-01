/**
 * Copyright (c) 2006-2023 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

#include "common/config.h"

#ifdef LOVE_ENABLE_LIBRETRO

#include "Joystick.h"
#include "libretro_state.h"

#include <cmath>

namespace love
{
namespace joystick
{
namespace libretro
{

namespace {

// LOVE's GamepadButton and the RetroPad are the same shape -- both were modelled
// on the same controller -- so this is a straight translation, not a mapping
// that has to guess at anything.
bool retro_button_for(love::joystick::Joystick::GamepadButton button, unsigned &out)
{
	using J = love::joystick::Joystick;

	switch (button)
	{
	case J::GAMEPAD_BUTTON_A:             out = RETRO_DEVICE_ID_JOYPAD_B;      return true;
	case J::GAMEPAD_BUTTON_B:             out = RETRO_DEVICE_ID_JOYPAD_A;      return true;
	case J::GAMEPAD_BUTTON_X:             out = RETRO_DEVICE_ID_JOYPAD_Y;      return true;
	case J::GAMEPAD_BUTTON_Y:             out = RETRO_DEVICE_ID_JOYPAD_X;      return true;
	case J::GAMEPAD_BUTTON_BACK:          out = RETRO_DEVICE_ID_JOYPAD_SELECT; return true;
	case J::GAMEPAD_BUTTON_START:         out = RETRO_DEVICE_ID_JOYPAD_START;  return true;
	case J::GAMEPAD_BUTTON_LEFTSTICK:     out = RETRO_DEVICE_ID_JOYPAD_L3;     return true;
	case J::GAMEPAD_BUTTON_RIGHTSTICK:    out = RETRO_DEVICE_ID_JOYPAD_R3;     return true;
	case J::GAMEPAD_BUTTON_LEFTSHOULDER:  out = RETRO_DEVICE_ID_JOYPAD_L;      return true;
	case J::GAMEPAD_BUTTON_RIGHTSHOULDER: out = RETRO_DEVICE_ID_JOYPAD_R;      return true;
	case J::GAMEPAD_BUTTON_DPAD_UP:       out = RETRO_DEVICE_ID_JOYPAD_UP;     return true;
	case J::GAMEPAD_BUTTON_DPAD_DOWN:     out = RETRO_DEVICE_ID_JOYPAD_DOWN;   return true;
	case J::GAMEPAD_BUTTON_DPAD_LEFT:     out = RETRO_DEVICE_ID_JOYPAD_LEFT;   return true;
	case J::GAMEPAD_BUTTON_DPAD_RIGHT:    out = RETRO_DEVICE_ID_JOYPAD_RIGHT;  return true;

	// The A/B and X/Y swap above is not a mistake. RetroPad's face buttons are
	// laid out in the SNES positions, where B is the bottom button; LOVE (like
	// SDL and Xbox) calls the bottom button A. Mapping by position rather than by
	// letter is what makes the pad behave the way the player expects.

	default:
		return false;   // GUIDE has no RetroPad equivalent
	}
}

// The order a plain joystick reports its buttons in.
//
// love.joystick's numbers are NOT the gamepad enum: LOVE's SDL backend answers
// isDown and joystickpressed with SDL_Joystick, which reports the pad's physical
// buttons in the order the device declares them. On the controllers Linux drives
// -- Xbox-style, which is what SDL's own layout follows -- that order is
// A B X Y LB RB back start, with no Guide in the middle: Guide is a gamepad
// concept, and most devices do not expose it as an ordinary button.
//
// The difference is one place, and it decides whether a game's config works.
// Mr. Rescue defaults pause to button 8 (config.lua), which is start on a real
// pad; counting Guide in pushes start to 9 and leaves 8 on nothing, so its
// "PRESS START" screen ignores Start entirely.
const love::joystick::Joystick::GamepadButton RAW_BUTTON_ORDER[] =
{
	love::joystick::Joystick::GAMEPAD_BUTTON_A,
	love::joystick::Joystick::GAMEPAD_BUTTON_B,
	love::joystick::Joystick::GAMEPAD_BUTTON_X,
	love::joystick::Joystick::GAMEPAD_BUTTON_Y,
	love::joystick::Joystick::GAMEPAD_BUTTON_LEFTSHOULDER,
	love::joystick::Joystick::GAMEPAD_BUTTON_RIGHTSHOULDER,
	love::joystick::Joystick::GAMEPAD_BUTTON_BACK,
	love::joystick::Joystick::GAMEPAD_BUTTON_START,
	love::joystick::Joystick::GAMEPAD_BUTTON_LEFTSTICK,
	love::joystick::Joystick::GAMEPAD_BUTTON_RIGHTSTICK,
	love::joystick::Joystick::GAMEPAD_BUTTON_DPAD_UP,
	love::joystick::Joystick::GAMEPAD_BUTTON_DPAD_DOWN,
	love::joystick::Joystick::GAMEPAD_BUTTON_DPAD_LEFT,
	love::joystick::Joystick::GAMEPAD_BUTTON_DPAD_RIGHT,
};

// L2 / R2 as plain joystick buttons, appended after the gamepad ones.
//
// SDL reports a controller's triggers as axes, and getAxis covers that -- but a
// pad's triggers are also ordinary buttons to a game that just polls isDown, and
// on a RetroPad they are digital anyway. Without these two, L2 and R2 are the
// only buttons on the pad no game can reach by any route.
const unsigned EXTRA_RAW_BUTTONS[] =
{
	RETRO_DEVICE_ID_JOYPAD_L2,
	RETRO_DEVICE_ID_JOYPAD_R2,
};

constexpr int NUM_EXTRA_RAW_BUTTONS =
	(int) (sizeof(EXTRA_RAW_BUTTONS) / sizeof(EXTRA_RAW_BUTTONS[0]));

constexpr int NUM_RAW_BUTTONS =
	(int) (sizeof(RAW_BUTTON_ORDER) / sizeof(RAW_BUTTON_ORDER[0]));

// A trigger's position, 0..1.
//
// Two sources, in order of preference. RETRO_DEVICE_INDEX_ANALOG_BUTTON gives the
// real travel on a pad that has it -- an Xbox or PS controller, which is what a
// game reading a trigger as an axis is written for. A RetroPad's L2/R2 are plain
// buttons, and a frontend that offers no analog reading returns 0 there, so the
// digital state is the fallback: fully pressed or not at all, which is the
// honest answer for a pad with nothing in between.
float trigger_axis(int port, unsigned button_id)
{
	const int16_t analog = love::libretro::state.input_state_cb(
		(unsigned) port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON,
		button_id);

	if (analog > 0)
		return (float) analog / 32767.0f;

	return love::libretro::state.input_state_cb(
		(unsigned) port, RETRO_DEVICE_JOYPAD, 0, button_id) ? 1.0f : 0.0f;
}

// libretro reports analog axes as -0x8000..0x7fff; LOVE wants -1..1.
float to_love_axis(int16_t v)
{
	return v < 0 ? (float) v / 32768.0f : (float) v / 32767.0f;
}

} // anonymous namespace

Joystick::Joystick(int id)
	: port(id)
	, name("RetroPad")
	, guid("retropad")
	, open_(false)
{
}

Joystick::Joystick(int id, int deviceindex)
	: port(id)
	, name("RetroPad")
	, guid("retropad")
	, open_(false)
{
	open(deviceindex);
}

bool Joystick::open(int deviceindex)
{
	if (deviceindex < 0)
		return false;

	port = deviceindex;
	open_ = true;
	return true;
}

void Joystick::close()
{
	open_ = false;
}

bool Joystick::isConnected() const
{
	// What the frontend told us through retro_set_controller_port_device. Port 0
	// defaults to connected, because a frontend is allowed to say nothing at all
	// and a core that then reports no pad would be unusable.
	if (!open_ || port < 0 || port >= love::libretro::State::NUM_PORTS)
		return false;

	return love::libretro::state.port_connected[port];
}

const char *Joystick::getName() const
{
	return name.c_str();
}

// A RetroPad has a fixed shape, so these are constants rather than something to
// query.
// Two sticks plus the two triggers. SDL reports a controller's triggers as axes
// 5 and 6, not as buttons, so a game polling getAxes() expects to find them
// there -- and with a count of 4 they were simply unreachable: L2 and R2 are two
// buttons the player has under their fingers that no game could read.
int Joystick::getAxisCount() const   { return open_ ? 6 : 0; }
int Joystick::getButtonCount() const
{
	return open_ ? NUM_RAW_BUTTONS + NUM_EXTRA_RAW_BUTTONS : 0;
}
int Joystick::getHatCount() const    { return open_ ? 1 : 0; }   // the D-pad

float Joystick::getAxis(int axisindex) const
{
	if (!open_ || !love::libretro::state.input_state_cb)
		return 0.0f;

	unsigned index, id;

	switch (axisindex)
	{
	case 0: index = RETRO_DEVICE_INDEX_ANALOG_LEFT;  id = RETRO_DEVICE_ID_ANALOG_X; break;
	case 1: index = RETRO_DEVICE_INDEX_ANALOG_LEFT;  id = RETRO_DEVICE_ID_ANALOG_Y; break;
	case 2: index = RETRO_DEVICE_INDEX_ANALOG_RIGHT; id = RETRO_DEVICE_ID_ANALOG_X; break;
	case 3: index = RETRO_DEVICE_INDEX_ANALOG_RIGHT; id = RETRO_DEVICE_ID_ANALOG_Y; break;

	// The triggers, in SDL's axis order.
	case 4: return trigger_axis(port, RETRO_DEVICE_ID_JOYPAD_L2);
	case 5: return trigger_axis(port, RETRO_DEVICE_ID_JOYPAD_R2);

	default: return 0.0f;
	}

	const float stick = to_love_axis(love::libretro::state.input_state_cb(
		(unsigned) port, RETRO_DEVICE_ANALOG, index, id));

	// The D-pad drives the left stick's axes as well.
	//
	// libretro keeps the two apart -- the D-pad is four buttons, the stick is two
	// axes -- but a real pad under SDL does not: a game reading axis 1 and 2 to
	// find "left" and "up" expects the D-pad to show up there, because on most
	// controllers it does. Mr. Rescue is the case that proves it: it reads
	// directions ONLY through joystick:getAxes() (main.lua), so with the axes left
	// purely analog its D-pad does nothing at all while the stick works.
	//
	// A pressed direction reads as full deflection, which is what a digital pad
	// is. The stick wins when both move, so holding the stick is never fought by
	// a resting D-pad.
	if (axisindex > 1 || std::fabs(stick) > 0.5f)
		return stick;

	const unsigned neg = (axisindex == 0) ? RETRO_DEVICE_ID_JOYPAD_LEFT
	                                      : RETRO_DEVICE_ID_JOYPAD_UP;
	const unsigned pos = (axisindex == 0) ? RETRO_DEVICE_ID_JOYPAD_RIGHT
	                                      : RETRO_DEVICE_ID_JOYPAD_DOWN;

	if (love::libretro::state.input_state_cb((unsigned) port, RETRO_DEVICE_JOYPAD, 0, neg))
		return -1.0f;
	if (love::libretro::state.input_state_cb((unsigned) port, RETRO_DEVICE_JOYPAD, 0, pos))
		return 1.0f;

	return stick;
}

std::vector<float> Joystick::getAxes() const
{
	std::vector<float> axes;
	const int count = getAxisCount();
	axes.reserve(count);

	for (int i = 0; i < count; i++)
		axes.push_back(getAxis(i));

	return axes;
}

Joystick::Hat Joystick::getHat(int hatindex) const
{
	// libretro reports the D-pad as four buttons, but SDL reports a pad's D-pad as
	// a hat -- and a game written against SDL reads it there. Presenting the same
	// four buttons as one hat costs nothing and is what such a game expects.
	if (hatindex != 0 || !open_ || !love::libretro::state.input_state_cb)
		return HAT_INVALID;

	const bool up    = love::libretro::state.input_state_cb(
		(unsigned) port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP)    != 0;
	const bool down  = love::libretro::state.input_state_cb(
		(unsigned) port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN)  != 0;
	const bool left  = love::libretro::state.input_state_cb(
		(unsigned) port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT)  != 0;
	const bool right = love::libretro::state.input_state_cb(
		(unsigned) port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT) != 0;

	if (up    && left)  return HAT_LEFTUP;
	if (up    && right) return HAT_RIGHTUP;
	if (down  && left)  return HAT_LEFTDOWN;
	if (down  && right) return HAT_RIGHTDOWN;
	if (up)    return HAT_UP;
	if (down)  return HAT_DOWN;
	if (left)  return HAT_LEFT;
	if (right) return HAT_RIGHT;

	return HAT_CENTERED;
}

int Joystick::rawButtonCount()
{
	return NUM_RAW_BUTTONS + NUM_EXTRA_RAW_BUTTONS;
}

int Joystick::rawButtonNumber(GamepadButton button)
{
	for (int i = 0; i < NUM_RAW_BUTTONS; i++)
	{
		if (RAW_BUTTON_ORDER[i] == button)
			return i + 1;   // love.joystick numbers from 1
	}

	return 0;   // guide, and anything else a plain joystick does not report
}

bool Joystick::isDown(const std::vector<int> &buttonlist) const
{
	if (!open_ || !love::libretro::state.input_state_cb)
		return false;

	for (int button : buttonlist)
	{
		// `button` arrives 0-based: love.joystick numbers from 1 in Lua and
		// wrap_Joystick.cpp has already subtracted one. It indexes the physical
		// button order, not the gamepad enum -- see RAW_BUTTON_ORDER.
		//
		// The number must NOT be used as a RetroPad id either: libretro's ids are
		// laid out in the SNES order (B=0, Y=1, select=2, start=3 ...), so passing
		// the index straight through silently asked for the wrong button.
		if (button < 0 || button >= NUM_RAW_BUTTONS + NUM_EXTRA_RAW_BUTTONS)
			continue;

		unsigned id;
		if (button >= NUM_RAW_BUTTONS)
			id = EXTRA_RAW_BUTTONS[button - NUM_RAW_BUTTONS];
		else if (!retro_button_for(RAW_BUTTON_ORDER[button], id))
			continue;

		if (love::libretro::state.input_state_cb((unsigned) port, RETRO_DEVICE_JOYPAD,
		                                         0, id))
			return true;
	}

	return false;
}

bool Joystick::openGamepad(int deviceindex)
{
	return open(deviceindex);
}

bool Joystick::isGamepad() const
{
	// Always. The frontend has already normalised whatever the player physically
	// owns into the RetroPad layout, which is exactly what a gamepad is.
	return open_;
}

float Joystick::getGamepadAxis(GamepadAxis axis) const
{
	if (!open_ || !love::libretro::state.input_state_cb)
		return 0.0f;

	switch (axis)
	{
	case GAMEPAD_AXIS_LEFTX:  return getAxis(0);
	case GAMEPAD_AXIS_LEFTY:  return getAxis(1);
	case GAMEPAD_AXIS_RIGHTX: return getAxis(2);
	case GAMEPAD_AXIS_RIGHTY: return getAxis(3);

	// The triggers are digital on a RetroPad (L2/R2 are buttons, not axes), so
	// report the two values they can actually take rather than inventing a range.
	case GAMEPAD_AXIS_TRIGGERLEFT:  return trigger_axis(port, RETRO_DEVICE_ID_JOYPAD_L2);
	case GAMEPAD_AXIS_TRIGGERRIGHT: return trigger_axis(port, RETRO_DEVICE_ID_JOYPAD_R2);

	default:
		return 0.0f;
	}
}

bool Joystick::isGamepadDown(const std::vector<GamepadButton> &blist) const
{
	if (!open_ || !love::libretro::state.input_state_cb)
		return false;

	for (GamepadButton button : blist)
	{
		unsigned id;
		if (!retro_button_for(button, id))
			continue;

		if (love::libretro::state.input_state_cb((unsigned) port, RETRO_DEVICE_JOYPAD, 0, id))
			return true;
	}

	return false;
}

Joystick::JoystickInput Joystick::getGamepadMapping(const GamepadInput &input) const
{
	// The mapping is the identity: a RetroPad button *is* the gamepad button.
	// There is no remapping layer to describe, because the frontend already did
	// that job before we saw the input.
	JoystickInput jinput;
	jinput.type = INPUT_TYPE_MAX_ENUM;

	if (input.type == INPUT_TYPE_BUTTON)
	{
		unsigned id;
		if (retro_button_for(input.button, id))
		{
			jinput.type = INPUT_TYPE_BUTTON;
			jinput.button = (int) id;
		}
	}
	else if (input.type == INPUT_TYPE_AXIS)
	{
		jinput.type = INPUT_TYPE_AXIS;
		switch (input.axis)
		{
		case GAMEPAD_AXIS_LEFTX:  jinput.axis = 0; break;
		case GAMEPAD_AXIS_LEFTY:  jinput.axis = 1; break;
		case GAMEPAD_AXIS_RIGHTX: jinput.axis = 2; break;
		case GAMEPAD_AXIS_RIGHTY: jinput.axis = 3; break;
		default: jinput.type = INPUT_TYPE_MAX_ENUM; break;
		}
	}

	return jinput;
}

std::string Joystick::getGamepadMappingString() const
{
	// SDL's mapping-string format describes how to turn an arbitrary device into
	// a gamepad. A RetroPad is already one, so there is nothing to describe.
	return "";
}

void *Joystick::getHandle() const
{
	return nullptr;
}

std::string Joystick::getGUID() const
{
	return guid;
}

int Joystick::getInstanceID() const
{
	return port;
}

int Joystick::getID() const
{
	return port;
}

void Joystick::getDeviceInfo(int &vendorID, int &productID, int &productVersion) const
{
	// A RetroPad is virtual: it has no vendor and no product.
	vendorID = 0;
	productID = 0;
	productVersion = 0;
}

bool Joystick::isVibrationSupported()
{
	// Whether the frontend gave us a rumble interface. Whether the player's pad
	// actually shakes is beyond what a core can know -- the frontend swallows the
	// request for a pad that cannot -- but claiming support with no interface at
	// all would make a game's rumble option do nothing with no way to tell.
	return love::libretro::state.set_rumble != nullptr;
}

bool Joystick::setVibration(float left, float right, float /*duration*/)
{
	vibration_left  = std::fmax(0.0f, std::fmin(1.0f, left));
	vibration_right = std::fmax(0.0f, std::fmin(1.0f, right));

	if (!open_ || love::libretro::state.set_rumble == nullptr)
		return false;

	// LOVE's two motors are libretro's two effects: the left one is the heavy
	// motor (STRONG), the right the light one (WEAK) -- the same split SDL makes.
	// Strength is 0..0xffff there and 0..1 here.
	const uint16_t strong = (uint16_t) (vibration_left  * 0xffff);
	const uint16_t weak   = (uint16_t) (vibration_right * 0xffff);

	// Both effects must be set: a game lowering one motor only would otherwise
	// leave the other running at its previous strength.
	const bool a = love::libretro::state.set_rumble((unsigned) port,
		RETRO_RUMBLE_STRONG, strong);
	const bool b = love::libretro::state.set_rumble((unsigned) port,
		RETRO_RUMBLE_WEAK, weak);

	return a || b;
}

bool Joystick::setVibration()
{
	return setVibration(0.0f, 0.0f);
}

void Joystick::getVibration(float &left, float &right)
{
	left  = vibration_left;
	right = vibration_right;
}

} // libretro
} // joystick
} // love

#endif // LOVE_ENABLE_LIBRETRO
