#pragma once
// OTA: CAP_OTA is 0 on the web board; these exist so shared code links. No slots.
#include "esp_partition.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef uint32_t esp_ota_handle_t;
typedef enum { ESP_OTA_IMG_NEW = 0, ESP_OTA_IMG_PENDING_VERIFY = 1, ESP_OTA_IMG_VALID = 2,
               ESP_OTA_IMG_INVALID = 3, ESP_OTA_IMG_ABORTED = 4, ESP_OTA_IMG_UNDEFINED = -1 } esp_ota_img_states_t;
typedef struct { uint32_t magic_word, secure_version, reserv1[2]; char version[32]; char project_name[32];
                 char time[16]; char date[16]; char idf_ver[32]; uint8_t app_elf_sha256[32]; uint32_t reserv2[20]; } esp_app_desc_t;
static inline const esp_partition_t* esp_ota_get_running_partition(void) { return NULL; }
static inline const esp_partition_t* esp_ota_get_boot_partition(void) { return NULL; }
static inline const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* p) { (void)p; return NULL; }
static inline esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p) { (void)p; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t esp_ota_get_state_partition(const esp_partition_t* p, esp_ota_img_states_t* s) { (void)p; if (s) *s = ESP_OTA_IMG_UNDEFINED; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }
static inline esp_err_t esp_ota_get_partition_description(const esp_partition_t* p, esp_app_desc_t* d) { (void)p; (void)d; return ESP_ERR_NOT_SUPPORTED; }
static inline const esp_app_desc_t* esp_ota_get_app_description(void) { return NULL; }
static inline const esp_app_desc_t* esp_app_get_description(void) { return NULL; }
#ifdef __cplusplus
}
#endif
