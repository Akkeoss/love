/**
 * libretro_options.cpp -- see libretro_options.h.
 *
 * The options are declared twice, on purpose. Modern frontends get the V2 form,
 * which groups them under a named category (a sub-menu in RetroArch) instead of
 * scattering eight "Button X" entries through the main list. Older frontends
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

// The keys a button can be mapped to, as they appear in the frontend menu.
//
// A curated shortlist, not every key LOVE knows: a menu offering forty values is
// unusable on a gamepad. It covers what .love games actually bind -- the letters
// most used for actions, the arrows, and the special keys games check for menus.
// "none" lets a player silence a button.
struct KeyChoice
{
	const char *name;   // shown in the menu, and stored as the option value
	int retro_key;      // 0 for "none"
};

const KeyChoice KEY_CHOICES[] =
{
	{ "none",   0 },

	{ "z", RETROK_z }, { "x", RETROK_x }, { "c", RETROK_c }, { "v", RETROK_v },
	{ "a", RETROK_a }, { "s", RETROK_s }, { "d", RETROK_d }, { "w", RETROK_w },
	{ "q", RETROK_q }, { "e", RETROK_e }, { "f", RETROK_f }, { "r", RETROK_r },

	{ "space",  RETROK_SPACE },
	{ "return", RETROK_RETURN },
	{ "escape", RETROK_ESCAPE },
	{ "lshift", RETROK_LSHIFT },
	{ "lctrl",  RETROK_LCTRL },
	{ "tab",    RETROK_TAB },

	{ "up",    RETROK_UP },
	{ "down",  RETROK_DOWN },
	{ "left",  RETROK_LEFT },
	{ "right", RETROK_RIGHT },

	{ "1", RETROK_1 }, { "2", RETROK_2 }, { "3", RETROK_3 },
};

constexpr int NUM_CHOICES = (int) (sizeof(KEY_CHOICES) / sizeof(KEY_CHOICES[0]));

// The configurable buttons, each with the key it defaults to. Defaults are the
// old fixed mapping, so a player who changes nothing gets exactly what worked
// before -- the conventions most .love games follow.
struct ButtonOption
{
	unsigned    pad_id;        // RETRO_DEVICE_ID_JOYPAD_*
	const char *key;           // core option variable key
	const char *label;         // shown in the menu
	const char *default_value; // one of KEY_CHOICES[].name
	int         current;       // resolved retro_key, filled by options_update
};

ButtonOption BUTTONS[] =
{
	{ RETRO_DEVICE_ID_JOYPAD_A,      "love_btn_a",      "Button A",      "z",      RETROK_z },
	{ RETRO_DEVICE_ID_JOYPAD_B,      "love_btn_b",      "Button B",      "x",      RETROK_x },
	{ RETRO_DEVICE_ID_JOYPAD_X,      "love_btn_x",      "Button X",      "c",      RETROK_c },
	{ RETRO_DEVICE_ID_JOYPAD_Y,      "love_btn_y",      "Button Y",      "v",      RETROK_v },
	{ RETRO_DEVICE_ID_JOYPAD_L,      "love_btn_l",      "Button L",      "q",      RETROK_q },
	{ RETRO_DEVICE_ID_JOYPAD_R,      "love_btn_r",      "Button R",      "e",      RETROK_e },
	{ RETRO_DEVICE_ID_JOYPAD_START,  "love_btn_start",  "Button Start",  "return", RETROK_RETURN },
	// Select defaults to nothing on purpose. It is easy to press by accident, and
	// escape -- the obvious mapping -- is what many .love games read to quit. A
	// player who wants Select to do something can map it; by default it is safe.
	{ RETRO_DEVICE_ID_JOYPAD_SELECT, "love_btn_select", "Button Select", "none",   0 },
};

constexpr int NUM_BUTTONS = (int) (sizeof(BUTTONS) / sizeof(BUTTONS[0]));

const char *CATEGORY_KEY = "input_mapping";
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

// Whether to read the game's size from conf.lua before the frontend asks, which
// removes the second boot. Off by default: it is known to break the picture on
// a 15 kHz CRT (see the call site in retro_load_game), and that is not a
// trade-off to make for a player without asking.
bool current_single_boot = false;

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

int key_from_name(const char *name)
{
	if (name == nullptr)
		return 0;

	for (const KeyChoice &c : KEY_CHOICES)
	{
		if (std::strcmp(c.name, name) == 0)
			return c.retro_key;
	}

	return 0;
}

// The V1 description strings ("Label; default|other|...") the frontend holds
// pointers into, so they must outlive the call: built once, kept static.
std::string v1_strings[NUM_BUTTONS];

// Build a full options set (categories + definitions) into caller-owned static
// storage, using the button labels and category strings passed in. One call per
// language: English is the reference, French (or another) is the translation.
//
// The value list -- the keys a button can send -- is the same in every language,
// so it is not translated.
// cats must hold 4 entries (input, timing, video, terminator); defs must hold
// NUM_BUTTONS + 3 (the buttons, the fps option, the render scale, terminator).
void build_options_v2(retro_core_option_v2_category *cats,
                      retro_core_option_v2_definition *defs,
                      const char *input_cat_name,
                      const char *input_cat_info,
                      const char *timing_cat_name,
                      const char *timing_cat_info,
                      const char *fps_label,
                      const char *jit_label,
                      const char *singleboot_label,
                      const char *video_cat_name,
                      const char *video_cat_info,
                      const char *scale_label,
                      const char *const *button_labels)
{
	cats[0].key  = CATEGORY_KEY;
	cats[0].desc = input_cat_name;
	cats[0].info = input_cat_info;
	cats[1].key  = TIMING_CATEGORY_KEY;
	cats[1].desc = timing_cat_name;
	cats[1].info = timing_cat_info;
	cats[2].key  = VIDEO_CATEGORY_KEY;
	cats[2].desc = video_cat_name;
	cats[2].info = video_cat_info;
	std::memset(&cats[3], 0, sizeof(cats[3]));

	int d = 0;

	for (int b = 0; b < NUM_BUTTONS; b++, d++)
	{
		defs[d].key              = BUTTONS[b].key;
		defs[d].desc             = button_labels[b];
		defs[d].desc_categorized = button_labels[b];
		defs[d].info             = nullptr;
		defs[d].info_categorized = nullptr;
		defs[d].category_key     = CATEGORY_KEY;

		for (int c = 0; c < NUM_CHOICES; c++)
		{
			defs[d].values[c].value = KEY_CHOICES[c].name;
			defs[d].values[c].label = KEY_CHOICES[c].name;
		}
		defs[d].values[NUM_CHOICES].value = nullptr;
		defs[d].values[NUM_CHOICES].label = nullptr;

		defs[d].default_value = BUTTONS[b].default_value;
	}

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

	// Single boot.
	//
	// The core boots the game, learns its real resolution, and corrects it
	// through SET_SYSTEM_AV_INFO -- which makes the frontend rebuild the GL
	// context, so the game boots a second time. That costs roughly 0.4s twice
	// over, and every shader and texture the game loaded is built twice.
	//
	// Reading conf.lua up front avoids it, and works. It is off by default
	// because on a 15 kHz CRT the context rebuild is also what keeps KMS and the
	// modeline in agreement: without it the picture sits shifted with a black
	// band at the top. HDMI does not show that, which is exactly why this is a
	// choice rather than a default.
	defs[d].key              = "love_single_boot";
	defs[d].desc             = singleboot_label;
	defs[d].desc_categorized = singleboot_label;
	defs[d].info             = nullptr;
	defs[d].info_categorized = nullptr;
	defs[d].category_key     = VIDEO_CATEGORY_KEY;
	defs[d].values[0] = { "off", "off" };
	defs[d].values[1] = { "on",  "on"  };
	defs[d].values[2] = { nullptr, nullptr };
	defs[d].default_value = "off";
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

	std::memset(&defs[d], 0, sizeof(defs[d]));
}

// The English button labels, matching BUTTONS[] order.
const char *const EN_BUTTON_LABELS[NUM_BUTTONS] =
{
	"Button A", "Button B", "Button X", "Button Y",
	"Button L", "Button R", "Button Start", "Button Select",
};

// Try the V2 (categorised) form, with translations. Returns false if the
// frontend does not support V2, so the caller can fall back to V1.
bool set_options_v2(retro_environment_t environ_cb)
{
	unsigned version = 0;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) || version < 2)
		return false;

	// All of these are handed to the frontend, which keeps the pointers, so they
	// must outlive the call -- hence static. Sizes must match what
	// build_options_v2 writes, and it writes past neither: 4 categories (input,
	// timing, video, terminator) and NUM_BUTTONS + 5 definitions (the buttons,
	// LuaJIT, single boot, fps, render scale, terminator). Adding an option means growing both here
	// and in the contract stated above build_options_v2 -- an overrun here would
	// be silent.
	static retro_core_option_v2_category   us_cats[4];
	static retro_core_option_v2_definition us_defs[NUM_BUTTONS + 5];
	static retro_core_option_v2_category   fr_cats[4];
	static retro_core_option_v2_definition fr_defs[NUM_BUTTONS + 5];
	static bool built = false;

	if (!built)
	{
		build_options_v2(us_cats, us_defs,
		                 "Input mapping",
		                 "Which keyboard key each gamepad button sends. Set these "
		                 "to match the keys the game expects.",
		                 "Timing",
		                 "Frame rate. Leave at 60 unless a game runs too fast, or "
		                 "to match a 50 Hz display.",
		                 "Frames per second",
		                 "LuaJIT (turn off if frames stall on your board)",
		                 "Single boot (faster start; may shift the picture on a CRT)",
		                 "Video",
		                 "Rendering. Lowering the render scale makes a heavy 3D "
		                 "game much cheaper to draw, at the cost of sharpness.",
		                 "Render scale",
		                 EN_BUTTON_LABELS);

		build_options_v2(fr_cats, fr_defs,
		                 LOVE_FR_CATEGORY_INPUT_NAME,
		                 LOVE_FR_CATEGORY_INPUT_INFO,
		                 LOVE_FR_CATEGORY_TIMING_NAME,
		                 LOVE_FR_CATEGORY_TIMING_INFO,
		                 LOVE_FR_FPS_LABEL,
		                 LOVE_FR_JIT_LABEL,
		                 LOVE_FR_SINGLEBOOT_LABEL,
		                 LOVE_FR_CATEGORY_VIDEO_NAME,
		                 LOVE_FR_CATEGORY_VIDEO_INFO,
		                 LOVE_FR_SCALE_LABEL,
		                 LOVE_FR_BUTTON_LABELS);
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

// The flat V1 fallback: no categories, just "Label; a|b|c". One extra slot for
// the fps option after the buttons.
void set_options_v1(retro_environment_t environ_cb)
{
	static retro_variable vars[NUM_BUTTONS + 5];

	for (int i = 0; i < NUM_BUTTONS; i++)
	{
		std::string s = BUTTONS[i].label;
		s += "; ";
		s += BUTTONS[i].default_value;

		for (const KeyChoice &c : KEY_CHOICES)
		{
			if (std::strcmp(c.name, BUTTONS[i].default_value) != 0)
			{
				s += "|";
				s += c.name;
			}
		}

		v1_strings[i] = s;
		vars[i].key   = BUTTONS[i].key;
		vars[i].value = v1_strings[i].c_str();
	}

	vars[NUM_BUTTONS].key   = "love_fps";
	vars[NUM_BUTTONS].value = "Frames per second; 60|50|30";

	vars[NUM_BUTTONS + 1].key   = "love_render_scale";
	vars[NUM_BUTTONS + 1].value = "Render scale; auto|100|50|33";

	vars[NUM_BUTTONS + 2].key   = "love_jit";
	vars[NUM_BUTTONS + 2].value = "LuaJIT; on|off";

	vars[NUM_BUTTONS + 3].key   = "love_single_boot";
	vars[NUM_BUTTONS + 3].value = "Single boot; off|on";

	vars[NUM_BUTTONS + 4].key   = nullptr;
	vars[NUM_BUTTONS + 4].value = nullptr;

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

	for (int i = 0; i < NUM_BUTTONS; i++)
	{
		retro_variable var;
		var.key   = BUTTONS[i].key;
		var.value = nullptr;

		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value != nullptr)
			BUTTONS[i].current = key_from_name(var.value);
		else
			BUTTONS[i].current = key_from_name(BUTTONS[i].default_value);
	}

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
	retro_variable sb_var;
	sb_var.key   = "love_single_boot";
	sb_var.value = nullptr;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &sb_var) && sb_var.value != nullptr)
		current_single_boot = std::strcmp(sb_var.value, "on") == 0;
	else
		current_single_boot = false;

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
}

int option_key_for_button(unsigned id)
{
	for (int i = 0; i < NUM_BUTTONS; i++)
	{
		if (BUTTONS[i].pad_id == id)
			return BUTTONS[i].current;
	}

	return 0;
}

bool option_jit()
{
	return current_jit;
}

bool option_single_boot()
{
	return current_single_boot;
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
