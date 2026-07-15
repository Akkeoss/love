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

#include "Mouse.h"
#include "libretro_state.h"

namespace love
{
namespace mouse
{
namespace libretro
{

Mouse::Mouse()
{
	// Deliberately empty. The SDL backend initialises SDL's video subsystem
	// here, which is exactly what must not happen on a board with no display
	// server.
}

const char *Mouse::getName() const
{
	return "love.mouse.libretro";
}

// --- Cursors -------------------------------------------------------------
//
// The frontend owns the screen and draws its own cursor if it wants one. A core
// cannot put a cursor on top of that, so these report honestly that there is no
// cursor rather than pretending to set one.

Cursor *Mouse::newCursor(love::image::ImageData * /*data*/, int /*hotx*/, int /*hoty*/)
{
	throw love::Exception("Custom mouse cursors are not supported in the libretro core.");
}

Cursor *Mouse::getSystemCursor(Cursor::SystemCursor /*cursortype*/)
{
	throw love::Exception("System mouse cursors are not supported in the libretro core.");
}

void Mouse::setCursor(Cursor * /*cursor*/) {}
void Mouse::setCursor() {}

Cursor *Mouse::getCursor() const
{
	return nullptr;
}

bool Mouse::isCursorSupported() const
{
	return false;
}

// --- Position ------------------------------------------------------------
//
// libretro's mouse reports movement as a delta per frame, not an absolute
// position, so an absolute one has to be accumulated here and clamped to the
// screen. RETRO_DEVICE_POINTER would give absolute coordinates but only for
// touch devices; the mouse is the more common case on a handheld or a box.

namespace {

double mouse_x = 0.0;
double mouse_y = 0.0;

void accumulate()
{
	if (!love::libretro::state.input_state_cb)
		return;

	const int16_t dx = love::libretro::state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
	                                        RETRO_DEVICE_ID_MOUSE_X);
	const int16_t dy = love::libretro::state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0,
	                                        RETRO_DEVICE_ID_MOUSE_Y);

	mouse_x += dx;
	mouse_y += dy;

	if (mouse_x < 0.0) mouse_x = 0.0;
	if (mouse_y < 0.0) mouse_y = 0.0;
	if (mouse_x > love::libretro::state.width)  mouse_x = love::libretro::state.width;
	if (mouse_y > love::libretro::state.height) mouse_y = love::libretro::state.height;
}

} // anonymous namespace

double Mouse::getX() const
{
	accumulate();
	return mouse_x;
}

double Mouse::getY() const
{
	accumulate();
	return mouse_y;
}

void Mouse::getPosition(double &x, double &y) const
{
	accumulate();
	x = mouse_x;
	y = mouse_y;
}

// Warping the pointer is meaningless when the frontend owns it, but a game that
// centres the mouse each frame (a common way to read relative movement) should
// not break -- so the position is moved, even though no physical pointer is.
void Mouse::setX(double x)
{
	mouse_x = x;
}

void Mouse::setY(double y)
{
	mouse_y = y;
}

void Mouse::setPosition(double x, double y)
{
	mouse_x = x;
	mouse_y = y;
}

// --- Buttons -------------------------------------------------------------

bool Mouse::isDown(const std::vector<int> &buttons) const
{
	if (!love::libretro::state.input_state_cb)
		return false;

	for (int button : buttons)
	{
		unsigned id;
		switch (button)
		{
		case 1:  id = RETRO_DEVICE_ID_MOUSE_LEFT;   break;
		case 2:  id = RETRO_DEVICE_ID_MOUSE_RIGHT;  break;
		case 3:  id = RETRO_DEVICE_ID_MOUSE_MIDDLE; break;
		default: continue;   // LOVE numbers buttons from 1; anything else is ours to ignore
		}

		if (love::libretro::state.input_state_cb(0, RETRO_DEVICE_MOUSE, 0, id))
			return true;
	}

	return false;
}

// --- Visibility and grabbing ---------------------------------------------
//
// All of this belongs to the frontend. Reporting a fixed answer is the only
// honest thing to do; pretending to have changed something would make a game
// believe it had control it does not have.

void Mouse::setVisible(bool /*visible*/) {}

bool Mouse::isVisible() const
{
	return false;
}

void Mouse::setGrabbed(bool /*grab*/) {}

bool Mouse::isGrabbed() const
{
	return false;
}

bool Mouse::setRelativeMode(bool /*relative*/)
{
	// libretro's mouse is already relative -- it only ever reports deltas -- but
	// there is no mode to switch, so this cannot claim to have set one.
	return false;
}

bool Mouse::getRelativeMode() const
{
	return false;
}

} // libretro
} // mouse
} // love

#endif // LOVE_ENABLE_LIBRETRO
