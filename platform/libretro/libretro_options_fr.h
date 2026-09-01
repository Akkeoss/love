/**
 * libretro_options_fr.h -- French labels for the core options.
 *
 * Kept apart from the option logic so a translation is a data change, not a code
 * change, and so more languages can be added the same way later. Only the
 * human-readable strings are here; the option keys and values (the letters a
 * button can be mapped to) are the same in every language.
 */

#pragma once

namespace love {
namespace libretro {

// Categories shown as sub-menus.
#define LOVE_FR_CATEGORY_TIMING_NAME "Cadence"
#define LOVE_FR_CATEGORY_TIMING_INFO \
	"Frequence d'images. Laisser a 60 sauf si un jeu va trop vite, ou pour " \
	"correspondre a un ecran 50 Hz."

#define LOVE_FR_FPS_LABEL "Images par seconde"

#define LOVE_FR_JIT_LABEL "LuaJIT (mettre off si des images se figent)"

#define LOVE_FR_SINGLEBOOT_LABEL \
	"Demarrage unique (plus rapide ; peut decaler l'image sur CRT)"

#define LOVE_FR_CATEGORY_VIDEO_NAME "Affichage"
#define LOVE_FR_CATEGORY_VIDEO_INFO \
	"Rendu. Baisser l'echelle de rendu allege beaucoup un jeu 3D lourd, au " \
	"prix de la nettete."

#define LOVE_FR_SCALE_LABEL "Echelle de rendu"

#define LOVE_FR_CATEGORY_POINTER_NAME "Pointeur"
#define LOVE_FR_CATEGORY_POINTER_INFO \
	"Le stick gauche peut deplacer un pointeur de souris, pour les jeux prevus " \
	"pour la souris."

#define LOVE_FR_POINTER_SPEED_LABEL "Vitesse du pointeur (stick gauche)"
#define LOVE_FR_POINTER_CLICK_LABEL "Bouton de clic"

} // namespace libretro
} // namespace love
