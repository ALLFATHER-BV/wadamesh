# LVGL 8.4 as an IDF component. Payload (upstream/) vendored by fetch-deps.sh (gitignored);
# the config is the repo's own include/lv_conf.h (same one the S3 touch build uses).
file(GLOB_RECURSE LVGL_SRCS "${CMAKE_CURRENT_LIST_DIR}/upstream/src/*.c")
list(FILTER LVGL_SRCS EXCLUDE REGEX "/(examples|demos)/")

idf_component_register(
  SRCS ${LVGL_SRCS}
  INCLUDE_DIRS "upstream" "${CMAKE_CURRENT_LIST_DIR}/../../../include"
  REQUIRES espressif__arduino-esp32
)
# LV_CONF_INCLUDE_SIMPLE → lvgl does #include "lv_conf.h" (found via the repo include/ above).
# LV_INDEV_DEF_SCROLL_THROW matches the S3 build (-D, since lv_conf leaves it unset).
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE=1 LV_INDEV_DEF_SCROLL_THROW=7)
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
