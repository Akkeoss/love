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

#ifndef LOVE_MOUSE_LIBRETRO_MOUSE_H
#define LOVE_MOUSE_LIBRETRO_MOUSE_H

#include "common/config.h"

#ifdef LOVE_ENABLE_LIBRETRO

#include "mouse/Mouse.h"

namespace love
{
namespace mouse
{
namespace libretro
{

// The SDL mouse backend calls SDL_InitSubSystem(SDL_INIT_VIDEO) in its
// constructor. On a board with no display server there is nothing for SDL to
// initialise against, so that call fails and takes the whole core down with it
// -- before a single frame is drawn. That, not the mouse itself, is why this
// backend exists.
//
// It still reports a position: libretro exposes RETRO_DEVICE_MOUSE, so a game
// that reads love.mouse works if the player has a mouse plugged in. There is no
// cursor to draw, though, because the frontend owns the screen.
class Mouse final : public love::mouse::Mouse
{
public:

	Mouse();

	Cursor *newCursor(love::image::ImageData *data, int hotx, int hoty) override;
	Cursor *getSystemCursor(Cursor::SystemCursor cursortype) override;
	void setCursor(Cursor *cursor) override;
	void setCursor() override;
	Cursor *getCursor() const override;
	bool isCursorSupported() const override;

	double getX() const override;
	double getY() const override;
	void getPosition(double &x, double &y) const override;
	void setX(double x) override;
	void setY(double y) override;
	void setPosition(double x, double y) override;

	void setVisible(bool visible) override;
	bool isVisible() const override;

	bool isDown(const std::vector<int> &buttons) const override;

	void setGrabbed(bool grab) override;
	bool isGrabbed() const override;

	bool setRelativeMode(bool relative) override;
	bool getRelativeMode() const override;

	const char *getName() const override;

}; // Mouse

} // libretro
} // mouse
} // love

#endif // LOVE_ENABLE_LIBRETRO
#endif // LOVE_MOUSE_LIBRETRO_MOUSE_H
