# OVERRIDE: replace the registry espressif/esp_hosted with badge.team's Tanmatsu fork.
#
# arduino-esp32 and espressif/esp_wifi_remote both pull espressif/esp_hosted, whose C6 SDIO
# host driver (port_esp_hosted_host_sdio.c, whole-archive) duplicates the one already provided
# by nicolaielectronics/esp-hosted-tanmatsu — and only the fork is wired to the Tanmatsu's real
# SDIO pins. Two whole-archive copies => "multiple definition" at link.
#
# IDF gives a local component precedence over a managed one of the same name, so this empty
# forwarder (component name "esp_hosted") shadows espressif__esp_hosted and re-exports the
# fork's headers + symbols. Result: a single esp-hosted (the hardware-correct one) that both
# arduino's WiFi and badge.team's stack link against. Keeps our shared src/ WiFi.h code intact.
idf_component_register(REQUIRES esp-hosted-tanmatsu)
