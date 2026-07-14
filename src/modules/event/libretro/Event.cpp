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

#include "common/config.h"

#ifdef LOVE_ENABLE_LIBRETRO

#include "Event.h"

#include "keyboard/libretro/Keyboard.h"
#include "libretro_state.h"

#include <cstring>

namespace love
{
namespace event
{
namespace libretro
{

Event::Event()
{
}

const char *Event::getName() const
{
	return "love.event.libretro";
}

void Event::pump()
{
	// There is no queue to drain: libretro reports input *state*, never events.
	// So this is where the events are manufactured, by diffing the state the core
	// sampled at the top of this frame against the previous frame's.

	auto kb = Module::getInstance<love::keyboard::Keyboard>(Module::M_KEYBOARD);
	if (kb == nullptr)
		return;

	using LKeyboard = love::keyboard::libretro::Keyboard;

	for (int rk = 0; rk < love::libretro::State::NUM_KEYS; rk++)
	{
		const bool pressed  = love::libretro::key_pressed(rk);
		const bool released = love::libretro::key_released(rk);

		if (!pressed && !released)
			continue;

		love::keyboard::Keyboard::Key key = love::keyboard::Keyboard::KEY_UNKNOWN;
		if (!LKeyboard::getConstant(rk, key))
			continue;   // a key LOVE has no name for; nothing useful to report

		love::keyboard::Keyboard::Scancode scancode = kb->getScancodeFromKey(key);

		std::vector<Variant> vargs;
		vargs.reserve(3);

		const char *keyname = nullptr;
		const char *scancodename = nullptr;
		love::keyboard::Keyboard::getConstant(key, keyname);
		love::keyboard::Keyboard::getConstant(scancode, scancodename);

		if (keyname == nullptr || scancodename == nullptr)
			continue;

		vargs.emplace_back(keyname, strlen(keyname));
		vargs.emplace_back(scancodename, strlen(scancodename));

		if (pressed)
		{
			// Third argument of love.keypressed is isrepeat. The frontend gives us
			// state, not repeats, so a held key produces exactly one press event --
			// which is what a game expects from a gamepad anyway.
			vargs.emplace_back(false);
			StrongRef<Message> msg(new Message("keypressed", vargs), Acquire::NORETAIN);
			push(msg);
		}
		else
		{
			StrongRef<Message> msg(new Message("keyreleased", vargs), Acquire::NORETAIN);
			push(msg);
		}
	}
}

Message *Event::wait()
{
	// love.event.wait() blocks until an event arrives. A libretro core must
	// never block inside retro_run() -- the frontend is driving the clock, and a
	// core that does not return hangs the whole emulator. Returning null means
	// "no event", which is the truthful answer and keeps the frame moving.
	return nullptr;
}

} // libretro
} // event
} // love

#endif // LOVE_ENABLE_LIBRETRO
