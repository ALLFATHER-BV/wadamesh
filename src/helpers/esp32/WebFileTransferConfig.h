#pragma once

// Browser file transfer requires the shared WebSocket transport and a removable
// card backend. Keep internal SPIFFS/LittleFS-only boards out: /transfer can hold
// large uploads and is intended to remain user-removable storage. The exception
// is the Attaky Mesh Deck, which has no card slot at all: it transfers to its
// internal LittleFS "tiles" partition (never SPIFFS, which holds the settings,
// contacts and chats), within that partition's free space.
#if defined(MULTI_TRANSPORT_COMPANION) && \
    (defined(HAS_TDECK_GT911) || defined(HAS_TDECK_PRO) || defined(TLORA_PAGER) || \
     defined(HAS_THINKNODE_M9) || defined(HELTEC_LORA_V4_R8) || \
     defined(HAS_WIO_TRACKER_L2) || defined(HAS_CROWPANEL_35) || defined(HAS_TDISPLAY_P4) || \
     defined(HAS_TANMATSU) || defined(ATTAKY_MESH_SERIES))
  #define WADA_WEB_FILE_TRANSFER 1
#else
  #define WADA_WEB_FILE_TRANSFER 0
#endif

#if WADA_WEB_FILE_TRANSFER && \
    (defined(HAS_WIO_TRACKER_L2) || defined(HAS_TDISPLAY_P4) || \
     defined(HAS_TANMATSU))
  #define WADA_WEB_FILE_TRANSFER_SDMMC 1
#else
  #define WADA_WEB_FILE_TRANSFER_SDMMC 0
#endif

#if WADA_WEB_FILE_TRANSFER && defined(ATTAKY_MESH_SERIES)
  #define WADA_WEB_FILE_TRANSFER_INTERNAL 1
#else
  #define WADA_WEB_FILE_TRANSFER_INTERNAL 0
#endif

#if WADA_WEB_FILE_TRANSFER && !WADA_WEB_FILE_TRANSFER_SDMMC && !WADA_WEB_FILE_TRANSFER_INTERNAL
  #define WADA_WEB_FILE_TRANSFER_SPI_SD 1
#else
  #define WADA_WEB_FILE_TRANSFER_SPI_SD 0
#endif