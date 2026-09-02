/**
 * libretro_options.cpp -- see libretro_options.h.
 *
 * The options are declared twice, on purpose. Modern frontends get the V2 form,
 * which groups them under named categories (sub-menus in RetroArch) instead of
 * scattering every entry through one flat list. Older frontends
 * that do not understand V2 fall back to the flat V1 form, so the options never
 * simply vanish.
 */

#include "libretro_options.h"
#include "libretro_options_fr.h"
#include "libretro_state.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace love {
namespace libretro {

namespace {

const char *POINTER_CATEGORY_KEY = "pointer";
const char *TIMING_CATEGORY_KEY = "timing";
const char *VIDEO_CATEGORY_KEY = "video";

// "60" resolves to the exact NTSC rate, not a round 60.0. crtswitchres builds
// its modeline from the fps we report, and a round 60.0 makes a real CRT sync
// unstably; 60.0988 is the rate a CRT actually runs at. The difference is
// invisible to a game's logic (0.16%) but decisive for the display.
constexpr double NTSC_FPS = 60.0988;

// The target fps option. A .love game never declares its intended frame rate, so
// there is nothing to detect automatically; 60 is what almost every game is
// written for, and the other values are there for the rare game built for 30, or
// to match a 50 Hz display. Filled by options_update().
double current_fps = NTSC_FPS;
double current_render_scale = 0.0;   // 0.0 = auto

// Whether the player asked for LuaJIT. Read once at boot (libretro_boot.cpp);
// changing it mid-game would do nothing, since the compiler state is set up
// before any game code runs.
bool current_jit = true;

// The pointer.
//
// Speed is in game pixels per frame at full stick deflection, and 0 means the
// core does not drive a cursor at all. Expressed in the game's own pixels rather
// than the screen's so the feel does not change with the render scale.
//
// Off by default, and that default was bought with a bug. A game that handles the
// pad itself often keeps its own on-screen cursor and watches the system mouse to
// decide who is in charge: gen1recomp drops its pad cursor the moment
// love.mouse.getPosition moves more than 3 pixels (src/ui/PadCursor.lua). A core
// cursor riding the same stick makes that game's cursor vanish as soon as the
// stick is touched.
double current_pointer_speed = 0.0;

// The pad button that clicks. NO_PAD_BUTTON means the stick moves a cursor that
// cannot click, which is only useful if the game reads the position alone.
unsigned current_pointer_click = RETRO_DEVICE_ID_JOYPAD_B;

// The pad buttons a click can be bound to, named as the RetroPad names them.
struct ClickChoice
{
	const char *name;
	unsigned    pad_id;
};

const ClickChoice CLICK_CHOICES[] =
{
	{ "b",    RETRO_DEVICE_ID_JOYPAD_B },
	{ "a",    RETRO_DEVICE_ID_JOYPAD_A },
	{ "y",    RETRO_DEVICE_ID_JOYPAD_Y },
	{ "x",    RETRO_DEVICE_ID_JOYPAD_X },
	{ "l",    RETRO_DEVICE_ID_JOYPAD_L },
	{ "r",    RETRO_DEVICE_ID_JOYPAD_R },
	{ "none", NO_PAD_BUTTON },
};

constexpr int NUM_CLICK_CHOICES = (int) (sizeof(CLICK_CHOICES) / sizeof(CLICK_CHOICES[0]));

unsigned click_from_name(const char *name)
{
	if (name == nullptr)
		return RETRO_DEVICE_ID_JOYPAD_B;

	for (const ClickChoice &c : CLICK_CHOICES)
	{
		if (std::strcmp(c.name, name) == 0)
			return c.pad_id;
	}

	return RETRO_DEVICE_ID_JOYPAD_B;
}

double fps_from_name(const char *name)
{
	if (name == nullptr)
		return NTSC_FPS;
	if (std::strcmp(name, "50") == 0)
		return 50.0;
	if (std::strcmp(name, "30") == 0)
		return 30.0;
	return NTSC_FPS;
}

// Build a full options set (categories + definitions) into caller-owned static
// storage, using the button labels and category strings passed in. One call per
// language: English is the reference, French (or another) is the translation.
//
// The value list -- the keys a button can send -- is the same in every language,
// so it is not translated.
// cats must hold 4 entries (timing, video, pointer, terminator); defs must hold
// 7 (LuaJIT, single boot, fps, render scale, pointer speed, pointer click,
// terminator).
void build_options_v2(retro_core_option_v2_category *cats,
                      retro_core_option_v2_definition *defs,
                      const char *timing_cat_name,
                      const char *timing_cat_info,
                      const char *fps_label,
                      const char *jit_label,
                      const char *video_cat_name,
                      const char *video_cat_info,
                      const char *scale_label,
                      const char *pointer_cat_name,
                      const char *pointer_cat_info,
                      const char *pointer_speed_label,
                      const char *pointer_click_label)
{
	cats[0].key  = TIMING_CATEGORY_KEY;
	cats[0].desc = timing_cat_name;
	cats[0].info = timing_cat_info;
	cats[1].key  = VIDEO_CATEGORY_KEY;
	cats[1].desc = video_cat_name;
	cats[1].info = video_cat_info;
	cats[2].key  = POINTER_CATEGORY_KEY;
	cats[2].desc = pointer_cat_name;
	cats[2].info = pointer_cat_info;
	std::memset(&cats[3], 0, sizeof(cats[3]));

	int d = 0;

	// The LuaJIT option.
	//
	// love's jitsetup.lua ends with jit.off() on arm/arm64, and upstream's
	// reason is not caution: LuaJIT 2.1 can only allocate machine code within a
	// short branch reach on ARM, SDL can exhaust that window, and compilation
	// then "both fail and take a long time". The core has been re-enabling it,
	// on the theory that interpreted Lua on a board is simply slower -- but a
	// failing compiler that retries costs far more than the interpreter it was
	// meant to beat, and that shows up as multi-hundred-millisecond stalls in a
	// heavy game rather than as a lower average.
	//
	// Which way wins depends on the board and on what the game asks of the JIT,
	// so it is the player's to choose rather than ours to guess. Default off:
	// that is what upstream ships, and what the native ARM builds of these games
	// run with.
	defs[d].key              = "love_jit";
	defs[d].desc             = jit_label;
	defs[d].desc_categorized = jit_label;
	defs[d].info             = nullptr;
	defs[d].info_categorized = nullptr;
	defs[d].category_key     = TIMING_CATEGORY_KEY;
	defs[d].values[0] = { "on",  "on"  };
	defs[d].values[1] = { "off", "off" };
	defs[d].values[2] = { nullptr, nullptr };
	defs[d].default_value = "on";
	d++;

	// The fps option.
	defs[d].key              = "love_fps";
	defs[d].desc             = fps_label;
	defs[d].desc_categorized = fps_label;
	defs[d].info             = nullptr;
	defs[d].info_categorized = nullptr;
	defs[d].category_key     = TIMING_CATEGORY_KEY;
	defs[d].values[0] = { "60", "60" };
	defs[d].values[1] = { "50", "50" };
	defs[d].values[2] = { "30", "30" };
	defs[d].values[3] = { nullptr, nullptr };
	defs[d].default_value = "60";
	d++;

	// The render scale. The one lever against a GPU-bound game on a weak board:
	// the game lays out and renders at the reduced size and the frontend scales
	// the finished frame up, so every full-screen pass shrinks with it. Takes
	// effect when the game (re)creates its window -- in practice, on restart.
	defs[d].key              = "love_render_scale";
	defs[d].desc             = scale_label;
	defs[d].desc_categorized = scale_label;
	defs[d].info             = nullptr;
	defs[d].info_categorized = nullptr;
	defs[d].category_key     = VIDEO_CATEGORY_KEY;
	// A 15kHz CRT shows 320x240, so a game laying out for 1024x768 is painting
	// about ten times the pixels the screen can display -- and a 3D game paints
	// them several times over, once per pass. Hence a way to render smaller.
	//
	// A percentage is a request for less GPU work, not a display size, and the
	// window backend treats it as such: it fits a standard display mode rather
	// than handing the display an arbitrary size (see Window::setWindow). A size
	// no CRT has a mode for gets a synthesised modeline and a soft picture.
	//
	// Four rungs, each with a distinct intent. The percentages that used to sit
	// between them were either redundant once Auto exists (66% of 1024x768 is
	// the 640x480 Auto already picks) or landed on sizes no display has a mode
	// for (40% -> 408x306, 25% -> 256x192) -- and 75% was actively harmful,
	// putting a 60Hz core on 768x576, a PAL mode, which made the frontend
	// resample the audio and distort it.
	defs[d].values[0] = { "auto", "Auto" };
	defs[d].values[1] = { "100",  "100%" };
	defs[d].values[2] = { "50",   "50%"  };
	defs[d].values[3] = { "33",   "33%"  };
	defs[d].values[4] = { nullptr, nullptr };
	defs[d].default_value = "auto";
	d++;

	// Pointer speed. See current_pointer_speed for why Off is the default.
	defs[d].key              = "love_pointer_speed";
	defs[d].desc             = pointer_speed_label;
	defs[d].desc_categorized = pointer_speed_label;
	defs[d].info             = nullptr;
	defs[d].info_categorized = nullptr;
	defs[d].category_key     = POINTER_CATEGORY_KEY;
	defs[d].values[0] = { "0",  "Off"    };
	defs[d].values[1] = { "12", "Normal" };
	defs[d].values[2] = { "6",  "Slow"   };
	defs[d].values[3] = { "20", "Fast"   };
	defs[d].values[4] = { nullptr, nullptr };
	defs[d].default_value = "0";
	d++;

	// Which button clicks.
	defs[d].key              = "love_pointer_click";
	defs[d].desc             = pointer_click_label;
	defs[d].desc_categorized = pointer_click_label;
	defs[d].info             = nullptr;
	defs[d].info_categorized = nullptr;
	defs[d].category_key     = POINTER_CATEGORY_KEY;
	for (int c = 0; c < NUM_CLICK_CHOICES; c++)
	{
		defs[d].values[c].value = CLICK_CHOICES[c].name;
		defs[d].values[c].label = CLICK_CHOICES[c].name;
	}
	defs[d].values[NUM_CLICK_CHOICES].value = nullptr;
	defs[d].values[NUM_CLICK_CHOICES].label = nullptr;
	defs[d].default_value = "b";
	d++;

	std::memset(&defs[d], 0, sizeof(defs[d]));
}

// Try the V2 (categorised) form, with translations. Returns false if the
// frontend does not support V2, so the caller can fall back to V1.
bool set_options_v2(retro_environment_t environ_cb)
{
	unsigned version = 0;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) || version < 2)
		return false;

	// All of these are handed to the frontend, which keeps the pointers, so they
	// must outlive the call -- hence static. Sizes must match what
	// build_options_v2 writes, and it writes past neither: 4 categories (timing,
	// video, pointer, terminator) and 7 definitions (LuaJIT, single boot, fps,
	// render scale, pointer speed, pointer click, terminator). Adding an option
	// means growing both here and in the contract stated above build_options_v2 --
	// an overrun here would be silent.
	static retro_core_option_v2_category   us_cats[4];
	static retro_core_option_v2_definition us_defs[7];
	static retro_core_option_v2_category   fr_cats[4];
	static retro_core_option_v2_definition fr_defs[7];
	static bool built = false;

	if (!built)
	{
		build_options_v2(us_cats, us_defs,
		                 "Timing",
		                 "Frame rate. Leave at 60 unless a game runs too fast, or "
		                 "to match a 50 Hz display.",
		                 "Frames per second",
		                 "LuaJIT (turn off if frames stall on your board)",
		                 "Video",
		                 "Rendering. Lowering the render scale makes a heavy 3D "
		                 "game much cheaper to draw, at the cost of sharpness.",
		                 "Render scale",
		                 "Pointer",
		                 "The left analog stick can move a mouse pointer, for "
		                 "games meant to be played with a mouse.",
		                 "Pointer speed (left stick)",
		                 "Pointer click button");

		build_options_v2(fr_cats, fr_defs,
		                 LOVE_FR_CATEGORY_TIMING_NAME,
		                 LOVE_FR_CATEGORY_TIMING_INFO,
		                 LOVE_FR_FPS_LABEL,
		                 LOVE_FR_JIT_LABEL,
		                 LOVE_FR_CATEGORY_VIDEO_NAME,
		                 LOVE_FR_CATEGORY_VIDEO_INFO,
		                 LOVE_FR_SCALE_LABEL,
		                 LOVE_FR_CATEGORY_POINTER_NAME,
		                 LOVE_FR_CATEGORY_POINTER_INFO,
		                 LOVE_FR_POINTER_SPEED_LABEL,
		                 LOVE_FR_POINTER_CLICK_LABEL);
		built = true;
	}

	static retro_core_options_v2 us_options    = { us_cats, us_defs };
	static retro_core_options_v2 fr_options    = { fr_cats, fr_defs };

	// The frontend picks the translation by its language. us is always the
	// reference; local is the localised set the frontend prefers.
	unsigned language = RETRO_LANGUAGE_ENGLISH;
	environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language);

	retro_core_options_v2_intl intl;
	intl.us    = &us_options;
	intl.local = (language == RETRO_LANGUAGE_FRENCH) ? &fr_options : nullptr;

	return environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL, &intl);
}

// The flat V1 fallback: no categories, just "Label; a|b|c".
void set_options_v1(retro_environment_t environ_cb)
{
	static retro_variable vars[7];

	vars[0].key   = "love_fps";
	vars[0].value = "Frames per second; 60|50|30";

	vars[1].key   = "love_render_scale";
	vars[1].value = "Render scale; auto|100|50|33";

	vars[2].key   = "love_jit";
	vars[2].value = "LuaJIT; on|off";

	vars[3].key   = "love_pointer_speed";
	vars[3].value = "Pointer speed (left stick); 0|12|6|20";

	vars[4].key   = "love_pointer_click";
	vars[4].value = "Pointer click button; b|a|y|x|l|r|none";

	vars[5].key   = nullptr;
	vars[5].value = nullptr;

	environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
}

} // anonymous namespace

void options_set(retro_environment_t environ_cb)
{
	if (environ_cb == nullptr)
		return;

	// V2 groups the buttons under a sub-menu; V1 is the flat fallback for older
	// frontends. Never leave the player with no options at all.
	if (!set_options_v2(environ_cb))
		set_options_v1(environ_cb);
}

void options_update(retro_environment_t environ_cb)
{
	if (environ_cb == nullptr)
		return;

	// fps.
	retro_variable fps_var;
	fps_var.key   = "love_fps";
	fps_var.value = nullptr;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &fps_var) && fps_var.value != nullptr)
		current_fps = fps_from_name(fps_var.value);
	else
		current_fps = 60.0;

	// LuaJIT. Read like the rest, but only ever consumed once, at boot: the
	// compiler's state is decided before any game code runs.
	retro_variable jit_var;
	jit_var.key   = "love_jit";
	jit_var.value = nullptr;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &jit_var) && jit_var.value != nullptr)
		current_jit = std::strcmp(jit_var.value, "off") != 0;
	else
		current_jit = true;

	// Single boot.

	// Render scale: "auto", or a percentage from "100" down to "25".
	//
	// 0.0 is the sentinel for auto -- the window backend reads it as "fit a
	// standard display mode" rather than "multiply by this". A percentage
	// cannot suit every game, since it is applied to whatever size the game
	// happens to ask for: 66% is right for a 1024x768 game and takes a
	// 320x240 one down to 210x158. Auto leaves the small one alone.
	retro_variable scale_var;
	scale_var.key   = "love_render_scale";
	scale_var.value = nullptr;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &scale_var) && scale_var.value != nullptr)
	{
		if (std::strcmp(scale_var.value, "auto") == 0)
			current_render_scale = 0.0;
		else
		{
			int pct = std::atoi(scale_var.value);
			current_render_scale = (pct >= 25 && pct <= 100) ? pct / 100.0 : 1.0;
		}
	}
	else
		current_render_scale = 0.0;

	// Pointer speed, in game pixels per frame at full stick deflection.
	retro_variable ptr_var;
	ptr_var.key   = "love_pointer_speed";
	ptr_var.value = nullptr;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &ptr_var) && ptr_var.value != nullptr)
		current_pointer_speed = std::atof(ptr_var.value);
	else
		current_pointer_speed = 0.0;

	// Which pad button clicks.
	retro_variable click_var;
	click_var.key   = "love_pointer_click";
	click_var.value = nullptr;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &click_var) && click_var.value != nullptr)
		current_pointer_click = click_from_name(click_var.value);
	else
		current_pointer_click = RETRO_DEVICE_ID_JOYPAD_B;
}

bool option_jit()
{
	return current_jit;
}

double option_pointer_speed()
{
	return current_pointer_speed;
}

unsigned option_pointer_click_button()
{
	return current_pointer_click;
}


double option_fps()
{
	return current_fps;
}

double option_render_scale()
{
	return current_render_scale;
}

} // namespace libretro
} // namespace love
