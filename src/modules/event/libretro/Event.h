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

}; // Event

} // libretro
} // event
} // love

#endif // LOVE_ENABLE_LIBRETRO
#endif // LOVE_EVENT_LIBRETRO_EVENT_H
