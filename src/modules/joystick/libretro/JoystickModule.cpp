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

#include "JoystickModule.h"
#include "Joystick.h"

namespace love
{
namespace joystick
{
namespace libretro
{

// How many ports to expose. RetroArch supports more, but a .love game that wants
// four players is already unusual, and every extra pad is one more thing a game
// has to enumerate. Raise it if a game ever needs it.
static const int NUM_PORTS = 4;

JoystickModule::JoystickModule()
{
	// Created up front rather than on connection: libretro has no hotplug event
	// to hang discovery off, so waiting for one would mean waiting forever.
	for (int i = 0; i < NUM_PORTS; i++)
	{
		auto *stick = new Joystick(i);
		stick->open(i);
		joysticks.push_back(stick);
	}
}

JoystickModule::~JoystickModule()
{
	for (auto *stick : joysticks)
	{
		stick->close();
		stick->release();
	}
}

const char *JoystickModule::getName() const
{
	return "love.joystick.libretro";
}

love::joystick::Joystick *JoystickModule::addJoystick(int deviceindex)
{
	if (deviceindex < 0 || deviceindex >= (int) joysticks.size())
		return nullptr;

	// The pad already exists -- there is nothing to add. Hand back the one that
	// owns that port.
	return joysticks[deviceindex];
}

void JoystickModule::removeJoystick(love::joystick::Joystick * /*joystick*/)
{
	// Ports do not go away under libretro. Doing nothing here is not laziness: a
	// game that "removes" a pad and then asks for it again should still find it.
}

love::joystick::Joystick *JoystickModule::getJoystickFromID(int instanceid)
{
	for (auto *stick : joysticks)
	{
		if (stick->getInstanceID() == instanceid)
			return stick;
	}

	return nullptr;
}

// Only the ports the frontend says have something on them.
//
// The four Joystick objects are built once and never destroyed (there is no
// hotplug event to hang that off), so "how many pads are there" cannot be the
// size of that list -- it would always be four, and a game with a player-count
// menu would offer four players to someone holding one pad. SDL answers with
// its activeSticks; the equivalent here is the ports the frontend has not
// declared empty.
int JoystickModule::connectedCount() const
{
	int n = 0;
	for (auto *stick : joysticks)
	{
		if (stick->isConnected())
			n++;
	}

	return n;
}

love::joystick::Joystick *JoystickModule::getJoystick(int joyindex)
{
	// joyindex counts connected pads, not ports: with port 2 empty, the pad on
	// port 3 must still be reachable as joystick 2. Anything else makes a game
	// enumerating getJoysticks() skip a player.
	if (joyindex < 0)
		return nullptr;

	for (auto *stick : joysticks)
	{
		if (!stick->isConnected())
			continue;

		if (joyindex == 0)
			return stick;

		joyindex--;
	}

	return nullptr;
}

int JoystickModule::getIndex(const love::joystick::Joystick *joystick)
{
	for (size_t i = 0; i < joysticks.size(); i++)
	{
		if (joysticks[i] == joystick)
			return (int) i;
	}

	return -1;
}

int JoystickModule::getJoystickCount() const
{
	return connectedCount();
}

// --- Gamepad mappings ----------------------------------------------------
//
// These exist so a game can teach SDL how to read an unknown controller. A
// RetroPad is not unknown -- the frontend has already mapped the player's real
// controller onto it, and that is the layer where remapping belongs (RetroArch
// exposes it in its own menus). So there is nothing here to configure, and
// pretending otherwise would let a game think it had changed something.

bool JoystickModule::setGamepadMapping(const std::string & /*guid*/,
	love::joystick::Joystick::GamepadInput /*gpinput*/,
	love::joystick::Joystick::JoystickInput /*joyinput*/)
{
	return false;
}

void JoystickModule::loadGamepadMappings(const std::string & /*mappings*/)
{
}

std::string JoystickModule::saveGamepadMappings()
{
	return "";
}

std::string JoystickModule::getGamepadMappingString(const std::string & /*guid*/) const
{
	return "";
}

} // libretro
} // joystick
} // love

#endif // LOVE_ENABLE_LIBRETRO
