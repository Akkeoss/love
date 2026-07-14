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

#include "Keyboard.h"
#include "libretro_state.h"

#include <cstring>

namespace love
{
namespace keyboard
{
namespace libretro
{

// retro_key -> the name LOVE gives that key.
//
// Going through names rather than numbers is deliberate, and was learned the
// hard way. It is tempting to assume the two enums line up on ASCII -- RETROK_z
// is 122, and so is 'z' -- but in LOVE 11.x, Key is a plain sequential enum:
// KEY_UNKNOWN=0, KEY_RETURN=1, KEY_ESCAPE=2, and so on. Casting a retro_key
// straight to a Key therefore produces nonsense: RETROK_RETURN (13) lands on
// KEY_HASH, so a player pressing Start finds the game receiving '#', and nothing
// in the input path looks wrong while it happens.
//
// (The assumption *is* true in LOVE 12, which is where it came from. It does not
// survive the move to 11.5.)
//
// The names are the one thing both sides agree on, and LOVE exposes a public
// lookup for them, so that is the bridge.
struct KeyName
{
	int retro_key;
	const char *love_name;
};

static const KeyName KEY_NAMES[] =
{
	{ RETROK_RETURN,    "return" },
	{ RETROK_ESCAPE,    "escape" },
	{ RETROK_BACKSPACE, "backspace" },
	{ RETROK_TAB,       "tab" },
	{ RETROK_SPACE,     "space" },

	{ RETROK_UP,        "up" },
	{ RETROK_DOWN,      "down" },
	{ RETROK_LEFT,      "left" },
	{ RETROK_RIGHT,     "right" },

	{ RETROK_LSHIFT,    "lshift" },
	{ RETROK_RSHIFT,    "rshift" },
	{ RETROK_LCTRL,     "lctrl" },
	{ RETROK_RCTRL,     "rctrl" },
	{ RETROK_LALT,      "lalt" },
	{ RETROK_RALT,      "ralt" },

	{ RETROK_DELETE,    "delete" },
	{ RETROK_INSERT,    "insert" },
	{ RETROK_HOME,      "home" },
	{ RETROK_END,       "end" },
	{ RETROK_PAGEUP,    "pageup" },
	{ RETROK_PAGEDOWN,  "pagedown" },

	{ RETROK_a, "a" }, { RETROK_b, "b" }, { RETROK_c, "c" }, { RETROK_d, "d" },
	{ RETROK_e, "e" }, { RETROK_f, "f" }, { RETROK_g, "g" }, { RETROK_h, "h" },
	{ RETROK_i, "i" }, { RETROK_j, "j" }, { RETROK_k, "k" }, { RETROK_l, "l" },
	{ RETROK_m, "m" }, { RETROK_n, "n" }, { RETROK_o, "o" }, { RETROK_p, "p" },
	{ RETROK_q, "q" }, { RETROK_r, "r" }, { RETROK_s, "s" }, { RETROK_t, "t" },
	{ RETROK_u, "u" }, { RETROK_v, "v" }, { RETROK_w, "w" }, { RETROK_x, "x" },
	{ RETROK_y, "y" }, { RETROK_z, "z" },

	{ RETROK_0, "0" }, { RETROK_1, "1" }, { RETROK_2, "2" }, { RETROK_3, "3" },
	{ RETROK_4, "4" }, { RETROK_5, "5" }, { RETROK_6, "6" }, { RETROK_7, "7" },
	{ RETROK_8, "8" }, { RETROK_9, "9" },

	{ RETROK_MINUS,  "-" },
	{ RETROK_EQUALS, "=" },
	{ RETROK_COMMA,  "," },
	{ RETROK_PERIOD, "." },
	{ RETROK_SLASH,  "/" },

	{ RETROK_F1,  "f1" },  { RETROK_F2,  "f2" },  { RETROK_F3,  "f3" },
	{ RETROK_F4,  "f4" },  { RETROK_F5,  "f5" },  { RETROK_F6,  "f6" },
	{ RETROK_F7,  "f7" },  { RETROK_F8,  "f8" },  { RETROK_F9,  "f9" },
	{ RETROK_F10, "f10" }, { RETROK_F11, "f11" }, { RETROK_F12, "f12" },
};

bool Keyboard::getConstant(int in, Key &out)
{
	for (const KeyName &e : KEY_NAMES)
	{
		if (e.retro_key == in)
			return love::keyboard::Keyboard::getConstant(e.love_name, out);
	}

	return false;
}

bool Keyboard::getConstant(Key in, int &out)
{
	const char *name = nullptr;
	if (!love::keyboard::Keyboard::getConstant(in, name) || name == nullptr)
		return false;

	for (const KeyName &e : KEY_NAMES)
	{
		if (std::strcmp(e.love_name, name) == 0)
		{
			out = e.retro_key;
			return true;
		}
	}

	return false;
}

Keyboard::Keyboard()
	: key_repeat(false)
	, text_input(false)
{
}

const char *Keyboard::getName() const
{
	return "love.keyboard.libretro";
}

void Keyboard::setKeyRepeat(bool enable)
{
	key_repeat = enable;
}

bool Keyboard::hasKeyRepeat() const
{
	return key_repeat;
}

bool Keyboard::isDown(const std::vector<Key> &keylist) const
{
	for (Key key : keylist)
	{
		int rk = 0;
		if (getConstant(key, rk) && love::libretro::key_down(rk))
			return true;
	}

	return false;
}

bool Keyboard::isScancodeDown(const std::vector<Scancode> &scancodelist) const
{
	// Scancodes name a physical key position, independent of layout. libretro
	// reports keycodes only, so the honest answer goes through the key that sits
	// at that position on a US layout -- the compromise every libretro core makes,
	// because the frontend never says what layout the player has.
	for (Scancode scancode : scancodelist)
	{
		Key key = getKeyFromScancode(scancode);
		int rk = 0;
		if (getConstant(key, rk) && love::libretro::key_down(rk))
			return true;
	}

	return false;
}

Keyboard::Key Keyboard::getKeyFromScancode(Scancode scancode) const
{
	// Key and Scancode are separate enums in 11.x, and SDL is what normally
	// bridges them -- there is no SDL here. But LOVE gives both the same name
	// ("left" names KEY_LEFT and SCANCODE_LEFT alike), so the name bridges them
	// through public API, without assuming anything about the numbering.
	const char *name = nullptr;
	if (!love::keyboard::Keyboard::getConstant(scancode, name) || name == nullptr)
		return KEY_UNKNOWN;

	Key key = KEY_UNKNOWN;
	love::keyboard::Keyboard::getConstant(name, key);
	return key;
}

Keyboard::Scancode Keyboard::getScancodeFromKey(Key key) const
{
	const char *name = nullptr;
	if (!love::keyboard::Keyboard::getConstant(key, name) || name == nullptr)
		return SCANCODE_UNKNOWN;

	Scancode scancode = SCANCODE_UNKNOWN;
	love::keyboard::Keyboard::getConstant(name, scancode);
	return scancode;
}

void Keyboard::setTextInput(bool enable)
{
	// No IME and no on-screen keyboard to raise. Remembering the flag is what
	// keeps love.keyboard.hasTextInput() honest, which is all a game can observe.
	text_input = enable;
}

void Keyboard::setTextInput(bool enable, double, double, double, double)
{
	text_input = enable;
}

bool Keyboard::hasTextInput() const
{
	return text_input;
}

bool Keyboard::hasScreenKeyboard() const
{
	return false;
}

} // libretro
} // keyboard
} // love

#endif // LOVE_ENABLE_LIBRETRO
