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

#ifndef LOVE_JOYSTICK_LIBRETRO_JOYSTICK_H
#define LOVE_JOYSTICK_LIBRETRO_JOYSTICK_H

#include "common/config.h"

#ifdef LOVE_ENABLE_LIBRETRO

#include "joystick/Joystick.h"

#include <string>

namespace love
{
namespace joystick
{
namespace libretro
{

// One RetroPad, seen as a LOVE joystick.
//
// This is simpler than the SDL backend in one important way: a RetroPad is
// always the same pad. There is no device database to consult, no GUID to match
// against a mapping table, no hotplug -- the frontend has already normalised
// whatever physical controller the player owns into a fixed layout. So the
// gamepad mapping is the identity, and most of this class is reporting that
// fixed shape.
//
// Note the pad is reported even when nothing is plugged in. libretro gives no
// way to ask "is a controller connected", and a game that refuses to start
// without a joystick would then never start at all.
class Joystick final : public love::joystick::Joystick
{
public:

	explicit Joystick(int id);
	Joystick(int id, int deviceindex);

	bool open(int deviceindex) override;
	void close() override;

	bool isConnected() const override;

	const char *getName() const override;

	int getAxisCount() const override;
	int getButtonCount() const override;
	int getHatCount() const override;

	float getAxis(int axisindex) const override;
	std::vector<float> getAxes() const override;
	Hat getHat(int hatindex) const override;

	bool isDown(const std::vector<int> &buttonlist) const override;

	bool openGamepad(int deviceindex) override;
	bool isGamepad() const override;

	float getGamepadAxis(GamepadAxis axis) const override;
	bool isGamepadDown(const std::vector<GamepadButton> &blist) const override;

	JoystickInput getGamepadMapping(const GamepadInput &input) const override;
	std::string getGamepadMappingString() const override;

	void *getHandle() const override;

	std::string getGUID() const override;
	int getInstanceID() const override;
	int getID() const override;

	void getDeviceInfo(int &vendorID, int &productID, int &productVersion) const override;

	bool isVibrationSupported() override;
	bool setVibration(float left, float right, float duration = -1.0f) override;
	bool setVibration() override;
	void getVibration(float &left, float &right) override;

private:

	// The libretro port this pad reads from. LOVE numbers joysticks from 1 in
	// Lua and from 0 here; the port is the same number.
	int port;

	std::string name;
	std::string guid;

	bool open_;

	// Remembered so getVibration() can report what was asked for. The frontend
	// gives no way to read the current rumble state back.
	float vibration_left  = 0.0f;
	float vibration_right = 0.0f;

}; // Joystick

} // libretro
} // joystick
} // love

#endif // LOVE_ENABLE_LIBRETRO
#endif // LOVE_JOYSTICK_LIBRETRO_JOYSTICK_H
