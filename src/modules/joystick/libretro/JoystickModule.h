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

#ifndef LOVE_JOYSTICK_LIBRETRO_JOYSTICKMODULE_H
#define LOVE_JOYSTICK_LIBRETRO_JOYSTICKMODULE_H

#include "common/config.h"

#ifdef LOVE_ENABLE_LIBRETRO

#include "joystick/JoystickModule.h"

#include <vector>

namespace love
{
namespace joystick
{
namespace libretro
{

// The pads libretro exposes, presented as LOVE joysticks.
//
// Unlike SDL's, this module does not discover devices: libretro has a fixed set
// of ports and no hotplug notification. So the pads are created up front, one
// per port, and simply exist. A port with nothing plugged into it reads as a pad
// whose buttons are never pressed -- which is indistinguishable, from the game's
// side, from a pad nobody is touching.
class JoystickModule final : public love::joystick::JoystickModule
{
public:

	JoystickModule();
	~JoystickModule();

	love::joystick::Joystick *addJoystick(int deviceindex) override;
	void removeJoystick(love::joystick::Joystick *joystick) override;

	love::joystick::Joystick *getJoystickFromID(int instanceid) override;
	love::joystick::Joystick *getJoystick(int joyindex) override;

	int getIndex(const love::joystick::Joystick *joystick) override;
	int getJoystickCount() const override;

	bool setGamepadMapping(const std::string &guid, love::joystick::Joystick::GamepadInput gpinput,
	                       love::joystick::Joystick::JoystickInput joyinput) override;

	void loadGamepadMappings(const std::string &mappings) override;
	std::string saveGamepadMappings() override;
	std::string getGamepadMappingString(const std::string &guid) const override;

	const char *getName() const override;

private:

	// How many ports currently have a device on them.
	int connectedCount() const;

	std::vector<love::joystick::Joystick *> joysticks;

}; // JoystickModule

} // libretro
} // joystick
} // love

#endif // LOVE_ENABLE_LIBRETRO
#endif // LOVE_JOYSTICK_LIBRETRO_JOYSTICKMODULE_H
