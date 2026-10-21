#pragma once
#include "nvs.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);   // wipes <data>/nvs.bin (factory reset)
#ifdef __cplusplus
}
#endif
