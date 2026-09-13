// SPDX-License-Identifier: GPL-3.0-or-later
//
// The single wadamesh definition of the 1.17 core's UIColor palette (semantic
// ColorVal statics declared in helpers/ui/DisplayDriver.h). This file compiles
// into EVERY target: all PIO envs build src/ wholesale, and both IDF apps glob
// src/**/*.cpp into the whole-archive wadamesh_app component — so the palette
// must be defined here EXACTLY ONCE, never in a display-driver .cpp (the P4
// builds compile both panel-SKU drivers into one archive, and the core's own
// driver .cpps carry upstream definitions that stay unlinked only while nothing
// references them).
#include <helpers/ui/DisplayDriver.h>

#if defined(HAS_TDECK_PRO)
ColorVal UIColor::window_bkg    = 0xFFFF;   // white paper
ColorVal UIColor::title_bkg     = 0xFFFF;
ColorVal UIColor::title_txt     = 0x0000;
ColorVal UIColor::primary_txt   = 0x0000;   // black ink
ColorVal UIColor::secondary_txt = 0x0000;
ColorVal UIColor::warning_txt   = 0x0000;
ColorVal UIColor::popup_bkg     = 0xFFFF;
ColorVal UIColor::popup_txt     = 0x0000;
ColorVal UIColor::corp_blue     = 0x0000;
#else
ColorVal UIColor::window_bkg    = 0x0000;   // black (our TFT UIs are dark)
ColorVal UIColor::title_bkg     = 0x0000;
ColorVal UIColor::title_txt     = 0xFFFF;
ColorVal UIColor::primary_txt   = 0xFFFF;   // white -- old LIGHT
ColorVal UIColor::secondary_txt = 0x8410;   // mid grey
ColorVal UIColor::warning_txt   = 0xF800;   // red -- old RED
ColorVal UIColor::popup_bkg     = 0x0000;
ColorVal UIColor::popup_txt     = 0xFFFF;
ColorVal UIColor::corp_blue     = 0x0339;
#endif
