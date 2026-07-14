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

#include "Window.h"
#include "libretro_state.h"

namespace love
{
namespace window
{

// Declared in window/Window.cpp and implemented by whichever backend is built.
// There is no DPI scaling to allow or disallow here: the frontend's framebuffer
// is exactly the size we asked for.
void setHighDPIAllowedImplementation(bool /*enable*/)
{
}

namespace libretro
{

// The window backend that owns no window.
//
// Under libretro the frontend owns the window, the GL context and the swap. So
// this class creates nothing and swaps nothing: setWindow() only tells
// love.graphics to initialise GL against the context the frontend has already
// made current, and reports back the size the frontend asked for.
//
// Most of the Window interface is about things that do not exist here -- there
// is no title bar to set, no cursor to grab, no display to enumerate, no dialog
// to show. Those are answered with the most honest value available rather than
// pretended: a game that asks "am I fullscreen?" gets true, because from its
// point of view it fills the whole screen.

Window::Window()
	: open(false)
{
	// The frontend has already decided the size; take it from the shared state
	// rather than inventing a default that would then have to be corrected.
	width  = (int) love::libretro::state.width;
	height = (int) love::libretro::state.height;
}

Window::~Window()
{
	close();
}

const char *Window::getName() const
{
	return "love.window.libretro";
}

void Window::setGraphics(graphics::Graphics *g)
{
	graphics.set(g);
}

bool Window::setWindow(int w, int h, WindowSettings *settings)
{
	// A game calling love.window.setMode() is asking for a size. We cannot
	// resize the frontend's window, but we can tell it our geometry changed and
	// let it letterbox or scale -- which is what a libretro core is expected to
	// do when its resolution changes.
	if (w > 0 && h > 0)
	{
		width  = w;
		height = h;

		love::libretro::state.width  = (unsigned) w;
		love::libretro::state.height = (unsigned) h;
	}

	if (settings)
		this->settings = *settings;

	// Bring love.graphics up on the context the frontend made current. The
	// context argument is unused by the OpenGL backend (it initialises whatever
	// context is current), which is exactly what we need -- we have no context
	// handle of our own to give it.
	if (graphics.get())
	{
		// In 11.x setMode takes the dimensions directly, so the 0x0-viewport trap
		// that bit us on the 12 branch cannot happen here: you cannot forget an
		// argument that is in the signature.
		graphics->setMode(width, height, width, height,
		                  settings ? settings->stencil : true);
	}

	open = true;
	return true;
}

void Window::getWindow(int &w, int &h, WindowSettings &s)
{
	w = width;
	h = height;
	s = settings;
}

void Window::close()
{
	if (graphics.get())
		graphics->unSetMode();
	open = false;
}

bool Window::setFullscreen(bool /*fullscreen*/, FullscreenType /*fstype*/)
{
	// Always fullscreen from the game's point of view: it fills whatever the
	// frontend gives it. Refusing would be a lie in the other direction.
	return true;
}

bool Window::setFullscreen(bool fullscreen)
{
	return setFullscreen(fullscreen, settings.fstype);
}

bool Window::onSizeChanged(int w, int h)
{
	width  = w;
	height = h;
	love::libretro::state.width  = (unsigned) w;
	love::libretro::state.height = (unsigned) h;
	return true;
}

int Window::getDisplayCount() const
{
	return 1;
}

const char *Window::getDisplayName(int /*displayindex*/) const
{
	return "libretro";
}

Window::DisplayOrientation Window::getDisplayOrientation(int /*displayindex*/) const
{
	return ORIENTATION_LANDSCAPE;
}

std::vector<Window::WindowSize> Window::getFullscreenSizes(int /*displayindex*/) const
{
	// One size: the one the frontend gave us.
	std::vector<WindowSize> sizes;
	WindowSize size = {};
	size.width  = width;
	size.height = height;
	sizes.push_back(size);
	return sizes;
}

void Window::getDesktopDimensions(int /*displayindex*/, int &w, int &h) const
{
	w = width;
	h = height;
}

void Window::setPosition(int, int, int) {}

void Window::getPosition(int &x, int &y, int &displayindex)
{
	x = 0;
	y = 0;
	displayindex = 0;
}

Rect Window::getSafeArea() const
{
	Rect r = { 0, 0, width, height };
	return r;
}

bool Window::isOpen() const
{
	return open;
}

void Window::setWindowTitle(const std::string &t)
{
	title = t;
}

const std::string &Window::getWindowTitle() const
{
	return title;
}

bool Window::setIcon(love::image::ImageData * /*imgd*/)
{
	return false;
}

love::image::ImageData *Window::getIcon()
{
	return nullptr;
}

void Window::setVSync(int /*vsync*/)
{
	// The frontend controls presentation and pacing. A game asking for vsync
	// gets whatever the frontend does; there is nothing for us to switch.
}

int Window::getVSync() const
{
	return 1;
}

void Window::setDisplaySleepEnabled(bool) {}

bool Window::isDisplaySleepEnabled() const
{
	return false;
}

void Window::minimize() {}
void Window::maximize() {}
void Window::restore() {}

bool Window::isMaximized() const { return true; }
bool Window::isMinimized() const { return false; }

bool Window::hasFocus() const      { return true; }
bool Window::hasMouseFocus() const { return true; }

bool Window::isVisible() const { return true; }

void Window::setMouseGrab(bool) {}

bool Window::isMouseGrabbed() const
{
	return false;
}

int Window::getWidth() const  { return width; }
int Window::getHeight() const { return height; }

// No DPI scaling: the frontend's framebuffer is exactly the size we asked for,
// so window coordinates and pixel coordinates are the same thing.
int Window::getPixelWidth() const  { return width; }
int Window::getPixelHeight() const { return height; }

void Window::clampPositionInWindow(double *wx, double *wy) const
{
	if (wx)
		*wx = std::min(std::max(*wx, 0.0), (double) getWidth());
	if (wy)
		*wy = std::min(std::max(*wy, 0.0), (double) getHeight());
}

void Window::windowToPixelCoords(double *, double *) const {}
void Window::pixelToWindowCoords(double *, double *) const {}
void Window::windowToDPICoords(double *, double *) const {}
void Window::DPIToWindowCoords(double *, double *) const {}

double Window::getDPIScale() const       { return 1.0; }
double Window::getNativeDPIScale() const { return 1.0; }

double Window::toPixels(double x) const { return x; }

void Window::toPixels(double wx, double wy, double &px, double &py) const
{
	px = wx;
	py = wy;
}

double Window::fromPixels(double x) const { return x; }

void Window::fromPixels(double px, double py, double &wx, double &wy) const
{
	wx = px;
	wy = py;
}

const void *Window::getHandle() const
{
	// There is no window handle. Anything that needs one (native dialogs, the
	// system module's clipboard) has to cope with null, which it must anyway on
	// a headless build.
	return nullptr;
}

bool Window::showMessageBox(const std::string &/*title*/, const std::string &message,
                            MessageBoxType /*type*/, bool /*attachtowindow*/)
{
	// No UI to put a dialog on. Log it instead of silently swallowing it: this
	// is the path LOVE takes to report a fatal error, and losing that message
	// would turn a clear crash into a black screen.
	if (love::libretro::state.log)
		love::libretro::state.log(RETRO_LOG_ERROR, "[LOVE] %s\n", message.c_str());
	return false;
}

int Window::showMessageBox(const MessageBoxData &data)
{
	if (love::libretro::state.log)
		love::libretro::state.log(RETRO_LOG_ERROR, "[LOVE] %s\n", data.message.c_str());
	return -1;
}

void Window::requestAttention(bool) {}

} // libretro
} // window
} // love

#endif // LOVE_ENABLE_LIBRETRO
