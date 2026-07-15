# P4-ONLY component: AT-over-SDIO Wi-Fi/BLE driver for the on-board ESP32-C6 (which ships running
# Espressif ESP-AT firmware, reachable over SDIO slot 1). This component exists ONLY in the
# tdisplay_p4 build tree, so no other board (Tanmatsu, T-Deck, V4, ...) can even see it. Clean-room:
# uses only Espressif's public SDMMC + esp_serial_slave_link (ESSL) APIs — no third-party driver code.
idf_component_register(
    SRCS "c6_at.c"
    INCLUDE_DIRS "include"
    REQUIRES sdmmc esp_serial_slave_link driver esp_timer esp_mm
)
