/**
 * Copyright (c) 2006-2025 LOVE Development Team
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

#ifndef LOVE_EVENT_LIBRETRO_EVENT_H
#define LOVE_EVENT_LIBRETRO_EVENT_H

#include "common/config.h"

#ifdef LOVE_ENABLE_LIBRETRO

#include "event/Event.h"
#include "joystick/Joystick.h"

namespace love
{
namespace event
{
namespace libretro
{

// There is no event queue under libretro.
//
// The SDL backend spends most of its 800-odd lines translating SDL events into
// LOVE ones. A libretro frontend has nothing of the sort: it does not push
// events, it exposes the *current state* of the inputs, which the core polls
// once per frame. So the input backends (keyboard, joystick, mouse) read that
// state directly, and this module's job shrinks to almost nothing.
//
// pump() is where the state-to-event translation will live: comparing this
// frame's input state against last frame's is what produces keypressed /
// keyreleased, since the frontend will not tell us about the edges.
class Event final : public love::event::Event
{
public:

	Event();
	virtual ~Event() {}

	void pump() override;
	Message *wait() override;

	const char *getName() const override;

private:

	// The pointer half of pump(): mousemoved/mousepressed/mousereleased, built by
	// diffing the cursor state the core published this frame.
	void pumpMouse();

	// The gamepad half: gamepadpressed/gamepadreleased, the joystick* forms, and
	// axis movement. Games written around a controller listen for these rather
	// than polling, so a core without them answers no input in such a game.
	void pumpJoystick();

	// Last frame's pad state, which is what the events are diffed against. The
	// joystick backend exposes four ports, and LOVE's gamepad enum bounds the
	// buttons; axes are the two sticks. Kept here rather than in the backend
	// because they exist only to find edges, which is this class's job.
	static const int MAX_PORTS = 4;
	// Two sticks and the two triggers: SDL reports triggers as axes, so they are
	// axes here too.
	static const int MAX_AXES  = 6;
	// L2 and R2, which the gamepad enum has no entry for.
	static const int NUM_EXTRA_BUTTONS = 2;
	bool  prevGamepad[MAX_PORTS][love::joystick::Joystick::GAMEPAD_BUTTON_MAX_ENUM] = {};
	float prevAxis[MAX_PORTS][MAX_AXES] = {};
	bool  prevExtra[MAX_PORTS][NUM_EXTRA_BUTTONS] = {};

}; // Event

} // libretro
} // event
} // love

#endif // LOVE_ENABLE_LIBRETRO
#endif // LOVE_EVENT_LIBRETRO_EVENT_H
