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
// The cursor itself lives in the shared state and is moved once per frame by
// update_pointer(), because two devices drive it: the frontend's mouse (which
// libretro reports as a per-frame delta, never a position) and the left analog
// stick, which is what makes a mouse-driven game playable on a pad. Accumulating
// here instead would give each of them its own cursor, and sample the mouse
// again on every call rather than once a frame.

double Mouse::getX() const
{
	return love::libretro::state.mouse_x;
}

double Mouse::getY() const
{
	return love::libretro::state.mouse_y;
}

void Mouse::getPosition(double &x, double &y) const
{
	x = love::libretro::state.mouse_x;
	y = love::libretro::state.mouse_y;
}

// Warping the pointer is meaningless when the frontend owns it, but a game that
// centres the mouse each frame (a common way to read relative movement) should
// not break -- so the position is moved, even though no physical pointer is.
void Mouse::setX(double x)
{
	love::libretro::state.mouse_x = x;
}

void Mouse::setY(double y)
{
	love::libretro::state.mouse_y = y;
}

void Mouse::setPosition(double x, double y)
{
	love::libretro::state.mouse_x = x;
	love::libretro::state.mouse_y = y;
}

// --- Buttons -------------------------------------------------------------

bool Mouse::isDown(const std::vector<int> &buttons) const
{
	if (!love::libretro::state.input_state_cb)
		return false;

	// Read from the shared state rather than the frontend, so that the pad button
	// bound to a click counts as a click here too -- a game polling isDown must
	// see what a game listening for mousepressed sees.
	for (int button : buttons)
	{
		if (love::libretro::mouse_down(button))
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
