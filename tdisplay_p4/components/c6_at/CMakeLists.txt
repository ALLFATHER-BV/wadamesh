# P4-ONLY component: AT-over-SDIO Wi-Fi/BLE driver for the on-board ESP32-C6 (which ships running
# Espressif ESP-AT firmware, reachable over SDIO slot 1). This component exists ONLY in the
# tdisplay_p4 build tree, so no other board (Tanmatsu, T-Deck, V4, ...) can even see it. The SDIO
# register framing follows LilyGo's GPL-3.0 cpp_bus_driver EspAt transport at pinned commit
# ff99314e37f125fe9f18b41340639f000c2f1e83; WadaMesh owns the AT worker and Wi-Fi/socket layers.
idf_component_register(
    SRCS "c6_at.c"
    INCLUDE_DIRS "include"
    REQUIRES sdmmc driver esp_timer esp_mm
)
