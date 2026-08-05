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

#include "Thread.h"

#ifdef LOVE_ENABLE_LIBRETRO
// SDL_GetTicks / SDL_Delay, for the bounded join in ~Thread. Thread.h only
// pulls in SDL_thread.h.
#include <SDL.h>
#endif

namespace love
{
namespace thread
{
namespace sdl
{
Thread::Thread(Threadable *t)
	: t(t)
	, running(false)
	, thread(nullptr)
{
}

Thread::~Thread()
{
	// Clean up handle
	if (thread)
	{
#ifdef LOVE_ENABLE_LIBRETRO
		// Wait, do not detach.
		//
		// Detaching says "nobody will ever join this thread" -- true for the
		// executable, which is about to exit() and take the address space with
		// it. A core has no exit(): the frontend dlclose()s us and keeps running,
		// so a thread still inside this library then executes unmapped pages.
		// That is a SIGSEGV *after* the frontend's last "unloading core" line,
		// which reads as a crash on exit with nothing to explain it. Seen on real
		// hardware, with the kernel naming the game's own audio worker.
		//
		// Not bounded on purpose: a timeout only narrows the race, it does not
		// close it. What makes an unbounded wait safe is the teardown order --
		// love.quit runs first (libretro_boot.cpp calls it, and pushes a quit to
		// the worker channels itself), lua_close after -- so a thread reaching
		// here has already been told to stop. LOVE thread bodies are loops that
		// end on that signal, not blocking waits.
		//
		// Except when the caller IS the thread. thread_runner ends with
		// t->release(), and if that drops the last reference the Threadable
		// destructs on the worker itself, reaching here through ~Threadable's
		// `delete owner`. Joining yourself is undefined, and detaching is safe in
		// that one case for the reason it is unsafe otherwise: the thread is
		// already leaving threadFunction, so no body is left to run.
		if (SDL_GetThreadID(thread) == SDL_ThreadID())
			SDL_DetachThread(thread);
		else
			SDL_WaitThread(thread, nullptr);
#else
		SDL_DetachThread(thread);
#endif
	}
}

bool Thread::start()
{
#if defined(LOVE_LINUX)
	// Temporarly block signals, as the thread inherits this mask
	love::thread::ScopedDisableSignals disableSignals;
#endif

	Lock l(mutex);

	if (running)
		return false;

	if (thread) // Clean old handle up
		SDL_WaitThread(thread, nullptr);

	// Keep the threadable around until the thread is done with it.
	// This is done before thread_runner executes because there can be a delay
	// between CreateThread and the start of the thread code's execution.
	t->retain();

	thread = SDL_CreateThread(thread_runner, t->getThreadName(), this);
	running = (thread != nullptr);

	if (!running)
		t->release(); // thread_runner is never called in this situation.

	return running;
}

void Thread::wait()
{
	{
		Lock l(mutex);
		if (!thread)
			return;
	}
	SDL_WaitThread(thread, nullptr);
	Lock l(mutex);
	running = false;
	thread = nullptr;
}

bool Thread::isRunning()
{
	Lock l(mutex);
	return running;
}

int Thread::thread_runner(void *data)
{
	Thread *self = (Thread *) data; // some compilers don't like 'this'

	self->t->threadFunction();

	{
		Lock l(self->mutex);
		self->running = false;
	}

	// This was retained in start().
	self->t->release();
	return 0;
}
} // sdl
} // thread
} // love
