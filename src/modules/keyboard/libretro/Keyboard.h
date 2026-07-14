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

#ifndef LOVE_KEYBOARD_LIBRETRO_KEYBOARD_H
#define LOVE_KEYBOARD_LIBRETRO_KEYBOARD_H

#include "common/config.h"

#ifdef LOVE_ENABLE_LIBRETRO

#include "keyboard/Keyboard.h"

namespace love
{
namespace keyboard
{
namespace libretro
{

// Reads the key state the core publishes each frame. That state is fed by both
// the real keyboard and the RetroPad (see platform/libretro/libretro_input.cpp),
// and this class cannot tell them apart -- deliberately, since a game asking for
// "left" only cares that left is held.
class Keyboard final : public love::keyboard::Keyboard
{
public:

	Keyboard();

	void setKeyRepeat(bool enable) override;
	bool hasKeyRepeat() const override;

	bool isDown(const std::vector<Key> &keylist) const override;
	bool isScancodeDown(const std::vector<Scancode> &scancodelist) const override;

	Key getKeyFromScancode(Scancode scancode) const override;
	Scancode getScancodeFromKey(Key key) const override;

	void setTextInput(bool enable) override;
	void setTextInput(bool enable, double x, double y, double w, double h) override;
	bool hasTextInput() const override;

	bool hasScreenKeyboard() const override;

	const char *getName() const override;

	// LOVE's Key <-> libretro's retro_key. Used by the event backend too, to turn
	// the core's key state into keypressed/keyreleased.
	static bool getConstant(Key in, int &out);
	static bool getConstant(int in, Key &out);

private:

	bool key_repeat;
	bool text_input;

}; // Keyboard

} // libretro
} // keyboard
} // love

#endif // LOVE_ENABLE_LIBRETRO
#endif // LOVE_KEYBOARD_LIBRETRO_KEYBOARD_H
