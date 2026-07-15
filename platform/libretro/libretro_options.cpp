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

// The target fps option. A .love game never declares its intended frame rate, so
// there is nothing to detect automatically; 60 is what almost every game is
// written for, and the other values are there for the rare game built for 30, or
// to match a 50 Hz display. Filled by options_update().
double current_fps = 60.0;

double fps_from_name(const char *name)
{
	if (name == nullptr)
		return 60.0;
	if (std::strcmp(name, "50") == 0)
		return 50.0;
	if (std::strcmp(name, "30") == 0)
		return 30.0;
	return 60.0;
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
// cats must hold 3 entries (input, timing, terminator); defs must hold
// NUM_BUTTONS + 2 (the buttons, the fps option, terminator).
void build_options_v2(retro_core_option_v2_category *cats,
                      retro_core_option_v2_definition *defs,
                      const char *input_cat_name,
                      const char *input_cat_info,
                      const char *timing_cat_name,
                      const char *timing_cat_info,
                      const char *fps_label,
                      const char *const *button_labels)
{
	cats[0].key  = CATEGORY_KEY;
	cats[0].desc = input_cat_name;
	cats[0].info = input_cat_info;
	cats[1].key  = TIMING_CATEGORY_KEY;
	cats[1].desc = timing_cat_name;
	cats[1].info = timing_cat_info;
	std::memset(&cats[2], 0, sizeof(cats[2]));

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
	// must outlive the call -- hence static. Sizes: 3 categories (input, timing,
	// terminator) and NUM_BUTTONS + 2 definitions (buttons, fps, terminator).
	static retro_core_option_v2_category   us_cats[3];
	static retro_core_option_v2_definition us_defs[NUM_BUTTONS + 2];
	static retro_core_option_v2_category   fr_cats[3];
	static retro_core_option_v2_definition fr_defs[NUM_BUTTONS + 2];
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
		                 EN_BUTTON_LABELS);

		build_options_v2(fr_cats, fr_defs,
		                 LOVE_FR_CATEGORY_INPUT_NAME,
		                 LOVE_FR_CATEGORY_INPUT_INFO,
		                 LOVE_FR_CATEGORY_TIMING_NAME,
		                 LOVE_FR_CATEGORY_TIMING_INFO,
		                 LOVE_FR_FPS_LABEL,
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
	static retro_variable vars[NUM_BUTTONS + 2];

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

	vars[NUM_BUTTONS + 1].key   = nullptr;
	vars[NUM_BUTTONS + 1].value = nullptr;

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

double option_fps()
{
	return current_fps;
}

} // namespace libretro
} // namespace love
