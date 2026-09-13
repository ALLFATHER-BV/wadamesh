#pragma once

// T-Deck Pro-specific LVGL config wrapper.
//
// Use a unique LV_CONF_PATH so the project copy wins over MeshCore's stale
// include/lv_conf.h. LVGL 8.4 then defines the scroll threshold/throw macros
// unconditionally in lv_hal_indev.h, so remove the shared touch overrides
// after loading the rest of the project configuration to avoid redefinitions.
#ifndef LV_CONF_H
#define LV_CONF_H
#endif

#include "lv_conf.h"

#ifdef LV_INDEV_DEF_SCROLL_LIMIT
#undef LV_INDEV_DEF_SCROLL_LIMIT
#endif

#ifdef LV_INDEV_DEF_SCROLL_THROW
#undef LV_INDEV_DEF_SCROLL_THROW
#endif