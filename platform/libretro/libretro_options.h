/**
 * libretro_options.h -- core options, and the button->key mapping they drive.
 *
 * Every .love game has its own key conventions -- one wants z/x, another s/d,
 * another the arrow keys -- and no fixed pad mapping can satisfy them all. So the
 * mapping is not fixed: each action button is a core option whose value is the
 * key it sends, and the player sets them per game in the frontend.
 *
 * The D-pad is deliberately not configurable. It maps to the arrow keys, which
 * every game agrees on, and making it an option would only add noise.
 */

#pragma once

#include <libretro.h>

namespace love {
namespace libretro {

// Declare the options to the frontend. Call once, from retro_set_environment.
void options_set(retro_environment_t environ_cb);

// Read the option values (once at startup, and whenever the frontend signals a
// change). Rebuilds the button->key mapping the input code reads.
void options_update(retro_environment_t environ_cb);

// The key a pad button currently sends, or 0 for none. id is a
// RETRO_DEVICE_ID_JOYPAD_* value. Read by update_input().
int option_key_for_button(unsigned id);

// The target frames per second the player selected (60, 50 or 30). The core
// reports this to the frontend as the content's fps, and derives the game's dt
// from it. Valid after the first options_update().
double option_fps();

} // namespace libretro
} // namespace love
