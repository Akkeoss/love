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
	// libretro offers no way to ask whether a pad is actually plugged in, and
	// reporting "disconnected" would make a game that waits for a controller wait
	// forever. Reporting connected is the answer that lets the game run; if there
	// is no pad, its buttons simply read as unpressed.
	return open_;
}

const char *Joystick::getName() const
{
	return name.c_str();
}

// A RetroPad has a fixed shape, so these are constants rather than something to
// query.
int Joystick::getAxisCount() const   { return open_ ? 4 : 0; }   // 2 sticks
int Joystick::getButtonCount() const { return open_ ? 16 : 0; }
int Joystick::getHatCount() const    { return 0; }               // D-pad is buttons

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
	default: return 0.0f;
	}

	return to_love_axis(love::libretro::state.input_state_cb(
		(unsigned) port, RETRO_DEVICE_ANALOG, index, id));
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

Joystick::Hat Joystick::getHat(int /*hatindex*/) const
{
	// The RetroPad has no hat: its D-pad is reported as four buttons.
	return HAT_INVALID;
}

bool Joystick::isDown(const std::vector<int> &buttonlist) const
{
	if (!open_ || !love::libretro::state.input_state_cb)
		return false;

	for (int button : buttonlist)
	{
		// LOVE numbers buttons from 1 in Lua.
		const int index = button - 1;
		if (index < 0 || index >= 16)
			continue;

		if (love::libretro::state.input_state_cb((unsigned) port, RETRO_DEVICE_JOYPAD,
		                                         0, (unsigned) index))
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
	case GAMEPAD_AXIS_TRIGGERLEFT:
		return love::libretro::state.input_state_cb((unsigned) port, RETRO_DEVICE_JOYPAD,
			0, RETRO_DEVICE_ID_JOYPAD_L2) ? 1.0f : 0.0f;
	case GAMEPAD_AXIS_TRIGGERRIGHT:
		return love::libretro::state.input_state_cb((unsigned) port, RETRO_DEVICE_JOYPAD,
			0, RETRO_DEVICE_ID_JOYPAD_R2) ? 1.0f : 0.0f;

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
	// Whether the physical pad rumbles is the frontend's business; it will ignore
	// the request if it cannot. Saying yes lets a game offer the option.
	return true;
}

bool Joystick::setVibration(float left, float right, float /*duration*/)
{
	vibration_left  = std::fmax(0.0f, std::fmin(1.0f, left));
	vibration_right = std::fmax(0.0f, std::fmin(1.0f, right));

	// Rumble goes through RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, which the core
	// has not asked for yet. Remember what was requested so getVibration() stays
	// truthful, and leave the plumbing for when a game needs it.
	return false;
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
