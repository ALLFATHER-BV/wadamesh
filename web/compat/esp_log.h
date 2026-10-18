#pragma once
#include <stdio.h>
// ESP log macros. The firmware builds with CORE_DEBUG_LEVEL=0, which compiles every
// log_x() out; so does the host unless WADA_HOST_LOG is defined (then errors and
// warnings go to stderr).
#if defined(WADA_HOST_LOG)
#define log_e(fmt, ...) fprintf(stderr, "[E] " fmt "\n", ##__VA_ARGS__)
#define log_w(fmt, ...) fprintf(stderr, "[W] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#define log_e(fmt, ...) do {} while (0)
#define log_w(fmt, ...) do {} while (0)
#define ESP_LOGE(tag, fmt, ...) do {} while (0)
#define ESP_LOGW(tag, fmt, ...) do {} while (0)
#endif
#define log_i(fmt, ...) do {} while (0)
#define log_d(fmt, ...) do {} while (0)
#define log_v(fmt, ...) do {} while (0)
#define ESP_LOGI(tag, fmt, ...) do {} while (0)
#define ESP_LOGD(tag, fmt, ...) do {} while (0)
#define ESP_LOGV(tag, fmt, ...) do {} while (0)
