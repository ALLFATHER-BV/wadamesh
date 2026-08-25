#pragma once

// M9-specific LVGL config wrapper.
//
// Why this exists:
// - LVGL 8.4's lv_hal_indev.h unconditionally defines LV_INDEV_DEF_SCROLL_*.
// - Our shared lv_conf defines LV_INDEV_DEF_SCROLL_LIMIT for touch tuning, and
//   M9's build flags define LV_INDEV_DEF_SCROLL_THROW. That combination emits
//   macro-redefinition warnings, even though M9 has no touch input path.
//
// For M9, include the shared project LV config, then drop touch-scroll tuning
// macros so LVGL's keypad-only defaults can apply without warnings.
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
