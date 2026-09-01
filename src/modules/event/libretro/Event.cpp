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
#include "joystick/JoystickModule.h"
#include "joystick/Joystick.h"
#include "joystick/libretro/Joystick.h"
#include "libretro_state.h"

#include <cmath>
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
	{
		// No keyboard module is not a reason to swallow the pointer's events too.
		pumpMouse();
		return;
	}

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

	pumpMouse();
	pumpJoystick();
}

// The gamepad's events.
//
// A game that polls love.joystick works without these, but one written around
// love.gamepadpressed does not -- and that is how a controller-first game is
// normally written. Balatro is the case in point: its menus are driven entirely
// by gamepad events, so without this the core renders its title screen and
// answers no input at all, while the same .love plays fine under stock LOVE.
//
// Manufactured by diffing, like the keys, because libretro reports state.
void Event::pumpJoystick()
{
	auto joymodule = Module::getInstance<love::joystick::JoystickModule>(Module::M_JOYSTICK);
	if (joymodule == nullptr)
		return;

	love::Type *joysticktype = &love::joystick::Joystick::type;

	using J = love::joystick::Joystick;

	for (int p = 0; p < (int) joymodule->getJoystickCount(); p++)
	{
		J *stick = joymodule->getJoystick(p);
		if (stick == nullptr)
			continue;

		// Buttons. Both event pairs are sent for each one: joystickpressed takes a
		// number and gamepadpressed a name, and a game listens for whichever suits
		// it. LOVE numbers joystick buttons from 1.
		for (int b = 0; b < (int) J::GAMEPAD_BUTTON_MAX_ENUM; b++)
		{
			const J::GamepadButton padbutton = (J::GamepadButton) b;

			const bool now  = stick->isGamepadDown({ padbutton });
			const bool prev = prevGamepad[p][b];

			if (now == prev)
				continue;

			prevGamepad[p][b] = now;

			const char *txt = nullptr;
			if (!J::getConstant(padbutton, txt) || txt == nullptr)
				continue;

			{
				std::vector<Variant> vargs;
				vargs.emplace_back(joysticktype, stick);
				vargs.emplace_back(txt, strlen(txt));
				StrongRef<Message> msg(new Message(now ? "gamepadpressed" : "gamepadreleased",
				                                   vargs), Acquire::NORETAIN);
				push(msg);
			}

			// The raw-button form, for a game that reads the pad as a plain
			// joystick rather than as a gamepad.
			//
			// The number comes from the same table isDown uses, so both paths call
			// a button by the same name. A button a plain joystick does not report
			// (guide) numbers 0 and is skipped here rather than shifting every
			// button after it.
			const int rawnum = love::joystick::libretro::Joystick::rawButtonNumber(padbutton);
			if (rawnum > 0)
			{
				std::vector<Variant> vargs;
				vargs.emplace_back(joysticktype, stick);
				vargs.emplace_back((double) rawnum);
				StrongRef<Message> msg(new Message(now ? "joystickpressed" : "joystickreleased",
				                                   vargs), Acquire::NORETAIN);
				push(msg);
			}
		}

		// The buttons that exist on the pad but not in the gamepad enum -- L2 and
		// R2. They are ordinary joystick buttons here (a RetroPad's triggers are
		// digital), numbered after the gamepad ones, so a game polling isDown and
		// a game listening for events still agree.
		for (int e = 0; e < NUM_EXTRA_BUTTONS; e++)
		{
			const int rawnum = (int) love::joystick::libretro::Joystick::rawButtonCount()
			                 - NUM_EXTRA_BUTTONS + e + 1;

			const bool now  = stick->isDown({ rawnum - 1 });
			const bool prev = prevExtra[p][e];

			if (now == prev)
				continue;

			prevExtra[p][e] = now;

			std::vector<Variant> vargs;
			vargs.emplace_back(joysticktype, stick);
			vargs.emplace_back((double) rawnum);
			StrongRef<Message> msg(new Message(now ? "joystickpressed" : "joystickreleased",
			                                   vargs), Acquire::NORETAIN);
			push(msg);
		}

		// Axes. A stick never sits exactly still, so an event goes out only when
		// the value has actually moved -- otherwise a resting pad would flood the
		// queue every frame.
		for (int a = 0; a < stick->getAxisCount() && a < MAX_AXES; a++)
		{
			const float now = stick->getAxis(a);

			if (std::fabs(now - prevAxis[p][a]) < 0.01f)
				continue;

			prevAxis[p][a] = now;

			std::vector<Variant> vargs;
			vargs.emplace_back(joysticktype, stick);
			vargs.emplace_back((double) (a + 1));
			vargs.emplace_back((double) now);
			StrongRef<Message> msg(new Message("joystickaxis", vargs), Acquire::NORETAIN);
			push(msg);
		}
	}
}

// The pointer's events, manufactured the same way and for the same reason: a
// game that only polls love.mouse works without these, but one that waits for
// love.mousepressed -- which is the ordinary way to write a mouse-driven game --
// would never see a click at all.
void Event::pumpMouse()
{
	if (love::libretro::state.mouse_dx != 0.0 || love::libretro::state.mouse_dy != 0.0)
	{
		std::vector<Variant> vargs;
		vargs.reserve(5);
		vargs.emplace_back(love::libretro::state.mouse_x);
		vargs.emplace_back(love::libretro::state.mouse_y);
		vargs.emplace_back(love::libretro::state.mouse_dx);
		vargs.emplace_back(love::libretro::state.mouse_dy);
		vargs.emplace_back(false);   // istouch

		StrongRef<Message> msg(new Message("mousemoved", vargs), Acquire::NORETAIN);
		push(msg);
	}

	// Button 0 is unused so that the index reads as LOVE's button number.
	for (int b = 1; b < love::libretro::State::NUM_MOUSE_BUTTONS; b++)
	{
		const bool pressed  = love::libretro::mouse_pressed(b);
		const bool released = love::libretro::mouse_released(b);

		if (!pressed && !released)
			continue;

		std::vector<Variant> vargs;
		vargs.reserve(5);
		vargs.emplace_back(love::libretro::state.mouse_x);
		vargs.emplace_back(love::libretro::state.mouse_y);
		vargs.emplace_back((double) b);
		vargs.emplace_back(false);   // istouch
		vargs.emplace_back(false);   // presses -- no double-click detection here

		StrongRef<Message> msg(new Message(pressed ? "mousepressed" : "mousereleased",
		                                   vargs), Acquire::NORETAIN);
		push(msg);
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
