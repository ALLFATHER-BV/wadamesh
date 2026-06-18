#include "MyMesh.h"

#include <Arduino.h> // needed for PlatformIO
#include <Mesh.h>
#include <ArduinoJson.h>   // settings backup import (uiImportBackup)
#include <string.h>
#include <SHA256.h>   // derive a region's flood-scope key from its #hashtag name
#include <time.h>     // gmtime_r for the "clock" CLI command
#ifdef ESP32
#include <esp_system.h>          // esp_restart for the "bootloader" CLI command
#include <soc/rtc_cntl_reg.h>    // RTC_CNTL_OPTION1_REG / FORCE_DOWNLOAD_BOOT
#endif
#include <helpers/AdvertDataHelpers.h>
#include <helpers/HttpOtaDisplayState.h>
#include <helpers/RepeaterTcpOtaEmit.h>
#include "WiFiConfig.h"
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
#include <WiFi.h>
#endif
#ifdef MULTI_TRANSPORT_COMPANION
#include <helpers/esp32/MultiTransportCompanionInterface.h>
#endif
#endif

#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)
/** While `ota url` runs, pin WS/TCP reply target so OTA progress survives yield() and checkRecvFrame. */
static int s_companion_ota_pinned_reply_target = -1;
#endif

#define CMD_APP_START                 1
#define CMD_SEND_TXT_MSG              2
#define CMD_SEND_CHANNEL_TXT_MSG      3
#define CMD_GET_CONTACTS              4 // with optional 'since' (for efficient sync)
#define CMD_GET_DEVICE_TIME           5
#define CMD_SET_DEVICE_TIME           6
#define CMD_SEND_SELF_ADVERT          7
#define CMD_SET_ADVERT_NAME           8
#define CMD_ADD_UPDATE_CONTACT        9
#define CMD_SYNC_NEXT_MESSAGE         10
#define CMD_SET_RADIO_PARAMS          11
#define CMD_SET_RADIO_TX_POWER        12
#define CMD_RESET_PATH                13
#define CMD_SET_ADVERT_LATLON         14
#define CMD_REMOVE_CONTACT            15
#define CMD_SHARE_CONTACT             16
#define CMD_EXPORT_CONTACT            17
#define CMD_IMPORT_CONTACT            18
#define CMD_REBOOT                    19
#define CMD_GET_BATT_AND_STORAGE      20   // was CMD_GET_BATTERY_VOLTAGE
#define CMD_SET_TUNING_PARAMS         21
#define CMD_DEVICE_QUERY              22
#define CMD_EXPORT_PRIVATE_KEY        23
#define CMD_IMPORT_PRIVATE_KEY        24
#define CMD_SEND_RAW_DATA             25
#define CMD_SEND_LOGIN                26
#define CMD_SEND_STATUS_REQ           27
#define CMD_HAS_CONNECTION            28
#define CMD_LOGOUT                    29 // 'Disconnect'
#define CMD_GET_CONTACT_BY_KEY        30
#define CMD_GET_CHANNEL               31
#define CMD_SET_CHANNEL               32
#define CMD_SIGN_START                33
#define CMD_SIGN_DATA                 34
#define CMD_SIGN_FINISH               35
#define CMD_SEND_TRACE_PATH           36
#define CMD_SET_DEVICE_PIN            37
#define CMD_SET_OTHER_PARAMS          38
#define CMD_SEND_TELEMETRY_REQ        39  // can deprecate this
#define CMD_GET_CUSTOM_VARS           40
#define CMD_SET_CUSTOM_VAR            41
#define CMD_GET_ADVERT_PATH           42
#define CMD_GET_TUNING_PARAMS         43
// NOTE: CMD range 44..49 parked, potentially for WiFi operations
#define CMD_SEND_BINARY_REQ           50
#define CMD_FACTORY_RESET             51
#define CMD_SEND_PATH_DISCOVERY_REQ   52
#define CMD_SET_FLOOD_SCOPE           54   // v8+
#define CMD_SEND_CONTROL_DATA         55   // v8+
#define CMD_GET_STATS                 56   // v8+, second byte is stats type
#define CMD_SEND_ANON_REQ             57
#define CMD_SET_AUTOADD_CONFIG        58
#define CMD_GET_AUTOADD_CONFIG        59
#define CMD_GET_ALLOWED_REPEAT_FREQ   60
#define CMD_SET_PATH_HASH_MODE        61  // v10+: payload [0, mode]; mode 0..2 (1/2/3-byte path hashes when sending)
#define CMD_SYNC_SINCE                62  // client sends T (4 bytes LE Unix sec); response: stream of 7/8/16/17 then 61
#define CMD_SEND_CHANNEL_DATA         62
#define CMD_SET_DEFAULT_FLOOD_SCOPE   63
#define CMD_GET_DEFAULT_FLOOD_SCOPE   64
#define CMD_SEND_RAW_PACKET           65

// Stats sub-types for CMD_GET_STATS
#define STATS_TYPE_CORE               0
#define STATS_TYPE_RADIO              1
#define STATS_TYPE_PACKETS             2

#define RESP_CODE_OK                  0
#define RESP_CODE_ERR                 1
#define RESP_CODE_CONTACTS_START      2  // first reply to CMD_GET_CONTACTS
#define RESP_CODE_CONTACT             3  // multiple of these (after CMD_GET_CONTACTS)
#define RESP_CODE_END_OF_CONTACTS     4  // last reply to CMD_GET_CONTACTS
#define RESP_CODE_SELF_INFO           5  // reply to CMD_APP_START
#define RESP_CODE_SENT                6  // reply to CMD_SEND_TXT_MSG
#define RESP_CODE_CONTACT_MSG_RECV    7  // a reply to CMD_SYNC_NEXT_MESSAGE (ver < 3)
#define RESP_CODE_CHANNEL_MSG_RECV    8  // a reply to CMD_SYNC_NEXT_MESSAGE (ver < 3)
#define RESP_CODE_CURR_TIME           9  // a reply to CMD_GET_DEVICE_TIME
#define RESP_CODE_NO_MORE_MESSAGES    10 // a reply to CMD_SYNC_NEXT_MESSAGE
#define RESP_CODE_EXPORT_CONTACT      11
#define RESP_CODE_BATT_AND_STORAGE    12 // a reply to a CMD_GET_BATT_AND_STORAGE
#define RESP_CODE_DEVICE_INFO         13 // a reply to CMD_DEVICE_QUERY
#define RESP_CODE_PRIVATE_KEY         14 // a reply to CMD_EXPORT_PRIVATE_KEY
#define RESP_CODE_DISABLED            15
#define RESP_CODE_CONTACT_MSG_RECV_V3 16 // a reply to CMD_SYNC_NEXT_MESSAGE (ver >= 3)
#define RESP_CODE_CHANNEL_MSG_RECV_V3 17 // a reply to CMD_SYNC_NEXT_MESSAGE (ver >= 3)
#define RESP_CODE_CHANNEL_INFO        18 // a reply to CMD_GET_CHANNEL
#define RESP_CODE_SIGN_START          19
#define RESP_CODE_SIGNATURE           20
#define RESP_CODE_CUSTOM_VARS         21
#define RESP_CODE_ADVERT_PATH         22
#define RESP_CODE_TUNING_PARAMS       23
#define RESP_CODE_STATS               24   // v8+, second byte is stats type
#define RESP_CODE_AUTOADD_CONFIG      25
#define RESP_ALLOWED_REPEAT_FREQ      26
#define RESP_CODE_CHANNEL_DATA_RECV   27
#define RESP_CODE_DEFAULT_FLOOD_SCOPE 28

#define MAX_CHANNEL_DATA_LENGTH       (MAX_FRAME_SIZE - 9)
#define RESP_CODE_SYNC_SINCE_DONE     61  // sent once after SyncSince delta stream; client sets last-sync to now

#define SEND_TIMEOUT_BASE_MILLIS        500
#define FLOOD_SEND_TIMEOUT_FACTOR       16.0f
#define DIRECT_SEND_PERHOP_FACTOR       6.0f
#define DIRECT_SEND_PERHOP_EXTRA_MILLIS 250
#define LAZY_CONTACTS_WRITE_DELAY       5000

#define PUBLIC_GROUP_PSK                "izOH6cXN6mrJ5e26oRXNcg=="

// these are _pushed_ to client app at any time
#define PUSH_CODE_ADVERT                0x80
#define PUSH_CODE_PATH_UPDATED          0x81
#define PUSH_CODE_SEND_CONFIRMED        0x82
#define PUSH_CODE_MSG_WAITING           0x83
#define PUSH_CODE_RAW_DATA              0x84
#define PUSH_CODE_LOGIN_SUCCESS         0x85
#define PUSH_CODE_LOGIN_FAIL            0x86
#define PUSH_CODE_STATUS_RESPONSE       0x87
#define PUSH_CODE_LOG_RX_DATA           0x88
#define PUSH_CODE_TRACE_DATA            0x89
#define PUSH_CODE_NEW_ADVERT            0x8A
#define PUSH_CODE_TELEMETRY_RESPONSE    0x8B
#define PUSH_CODE_BINARY_RESPONSE       0x8C
#define PUSH_CODE_PATH_DISCOVERY_RESPONSE 0x8D
#define PUSH_CODE_CONTROL_DATA          0x8E   // v8+
#define PUSH_CODE_CONTACT_DELETED       0x8F // used to notify client app of deleted contact when overwriting oldest
#define PUSH_CODE_CONTACTS_FULL         0x90 // used to notify client app that contacts storage is full

#define ERR_CODE_UNSUPPORTED_CMD        1
#define ERR_CODE_NOT_FOUND              2
#define ERR_CODE_TABLE_FULL             3
#define ERR_CODE_BAD_STATE              4
#define ERR_CODE_FILE_IO_ERROR          5
#define ERR_CODE_ILLEGAL_ARG            6

#define MAX_SIGN_DATA_LEN               (8 * 1024) // 8K

#ifndef COMPANION_SYNC_DEBUG
#define COMPANION_SYNC_DEBUG 0
#endif

#if COMPANION_SYNC_DEBUG
#define SYNC_DEBUG_PRINTLN(F, ...) Serial.printf("SYNCDBG: " F "\n", ##__VA_ARGS__)
struct SyncDebugCounters {
  uint32_t req;
  uint32_t had_frame;
  uint32_t no_more;
  uint32_t write_ok;
  uint32_t write_fail;
  uint32_t retries;
};
static SyncDebugCounters g_sync_dbg = {0, 0, 0, 0, 0, 0};
#else
#define SYNC_DEBUG_PRINTLN(...) do {} while (0)
#endif

// Auto-add config bitmask
// Bit 0: If set, overwrite oldest non-favourite contact when contacts file is full
// Bits 1-4: these indicate which contact types to auto-add when manual_contact_mode = 0x01
#define AUTO_ADD_OVERWRITE_OLDEST (1 << 0)  // 0x01 - overwrite oldest non-favourite when full
#define AUTO_ADD_CHAT             (1 << 1)  // 0x02 - auto-add Chat (Companion) (ADV_TYPE_CHAT)
#define AUTO_ADD_REPEATER         (1 << 2)  // 0x04 - auto-add Repeater (ADV_TYPE_REPEATER)
#define AUTO_ADD_ROOM_SERVER      (1 << 3)  // 0x08 - auto-add Room Server (ADV_TYPE_ROOM)
#define AUTO_ADD_SENSOR           (1 << 4)  // 0x10 - auto-add Sensor (ADV_TYPE_SENSOR)

#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
#define MESHCOMOD_WIFI_SCAN_MAX 12
static char s_meshcomod_scan_ssids[MESHCOMOD_WIFI_SCAN_MAX][WIFI_CONFIG_SSID_MAX];
static int s_meshcomod_scan_count = 0;

// Watchdog-safe Wi-Fi scan. Starts an ASYNC scan and polls to completion with
// vTaskDelay yields + a hard time cap, so the calling task never blocks long
// enough to starve an idle task / the 5 s task watchdog. Replaces the old
// pattern of two back-to-back *synchronous* scans (~8 s total) with a
// WiFi.disconnect() wedged between them — which, with no AP present (Wi-Fi on
// but no SSID connected), always ran BOTH passes and tripped the task watchdog
// -> panic reboot. Returns the AP count (0 on none/failure/timeout); read
// results with WiFi.SSID(i).
static int wifiScanWatchdogSafe(uint32_t cap_ms) {
  WiFi.scanDelete();
  if (WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true, /*passive=*/false,
                        /*max_ms_per_chan=*/300, /*channel=*/0) == WIFI_SCAN_FAILED) {
    return 0;
  }
  const uint32_t deadline = millis() + cap_ms;
  for (;;) {
    const int16_t st = WiFi.scanComplete();   // >=0 = AP count, -1 running, -2 failed
    if (st >= 0)                return st;
    if (st == WIFI_SCAN_FAILED) return 0;
    if ((int32_t)(millis() - deadline) > 0) { WiFi.scanDelete(); return 0; }
    vTaskDelay(pdMS_TO_TICKS(50));            // yield -> IDLE runs -> feeds the task watchdog
  }
}
#endif
#endif
static const char* kMeshcomodHelpMsg =
  "help\n"
  "status\n"
  "ota start\n"
  "ota url <https://...bin>\n"
  "ota netdiag\n"
  "ota status\n"
  "wifi scan\n"
  "wifi use <n>\n"
  "wifi set ssid <v>\n"
  "wifi set pwd <v>\n"
  "wifi status\n"
  "wifi on\n"
  "wifi off\n"
  "wifi apply\n"
  "wifi clear\n"
  "tcp on\n"
  "tcp off\n"
  "tcp status\n"
  "ble on\n"
  "ble off\n"
  "ble status";

#define MESHCOMOD_CMD_CACHE_SIZE 6
struct MeshcomodCmdCacheEntry {
  uint32_t ts;
  uint32_t seen_ms;
  char text[128];
};
static MeshcomodCmdCacheEntry s_meshcomod_cmd_cache[MESHCOMOD_CMD_CACHE_SIZE] = {};
static int s_meshcomod_cmd_cache_next = 0;
static uint32_t s_meshcomod_last_reply_ts = 0;
static uint32_t s_last_cmd_txt_ts = 0;
static uint8_t s_last_cmd_txt_pub6[6] = {0};
static uint32_t s_last_cmd_txt_body_crc = 0;
static uint32_t s_last_cmd_txt_ack = 0;
static uint32_t s_last_cmd_txt_est_timeout = 0;
static uint32_t s_last_cmd_txt_seen_ms = 0;

enum MeshcomodPendingAction {
  MESHCOMOD_PENDING_NONE = 0,
  MESHCOMOD_PENDING_TCP_OFF = 1,
  MESHCOMOD_PENDING_BLE_OFF = 2,
};
static MeshcomodPendingAction s_meshcomod_pending_action = MESHCOMOD_PENDING_NONE;
static uint32_t s_meshcomod_pending_until_ms = 0;


static char* trimWsInPlace(char* s) {
  if (!s) return s;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  int n = (int)strlen(s);
  while (n > 0) {
    char c = s[n - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      s[n - 1] = '\0';
      n--;
    } else {
      break;
    }
  }
  return s;
}

static char* unquoteInPlace(char* s) {
  s = trimWsInPlace(s);
  if (!s) return s;
  int n = (int)strlen(s);
  if (n >= 2) {
    char q = s[0];
    if ((q == '"' || q == '\'') && s[n - 1] == q) {
      s[n - 1] = '\0';
      s++;
    }
  }
  return s;
}

static bool isMeshcomodDuplicate(uint32_t msg_ts, const char* text) {
  if (!text || !*text) return false;
  uint32_t now_ms = millis();
  for (int i = 0; i < MESHCOMOD_CMD_CACHE_SIZE; i++) {
    const MeshcomodCmdCacheEntry& e = s_meshcomod_cmd_cache[i];
    if (e.ts == 0 || e.text[0] == '\0') continue;
    // Primary dedupe key: app message timestamp + same text
    if (e.ts == msg_ts && strcmp(e.text, text) == 0) return true;
    // Secondary dedupe: same text repeated very quickly with a new timestamp
    if (strcmp(e.text, text) == 0 && (now_ms - e.seen_ms) < 1800UL) return true;
  }
  return false;
}

static void rememberMeshcomodCommand(uint32_t msg_ts, const char* text) {
  MeshcomodCmdCacheEntry& e = s_meshcomod_cmd_cache[s_meshcomod_cmd_cache_next];
  e.ts = msg_ts;
  e.seen_ms = millis();
  if (text)
    StrHelper::strzcpy(e.text, text, sizeof(e.text));
  else
    e.text[0] = '\0';
  s_meshcomod_cmd_cache_next = (s_meshcomod_cmd_cache_next + 1) % MESHCOMOD_CMD_CACHE_SIZE;
}

void MyMesh::writeOKFrame() {
  uint8_t buf[1];
  buf[0] = RESP_CODE_OK;
  _serial->writeFrame(buf, 1);
}
void MyMesh::writeErrFrame(uint8_t err_code) {
  uint8_t buf[2];
  buf[0] = RESP_CODE_ERR;
  buf[1] = err_code;
  _serial->writeFrame(buf, 2);
}

void MyMesh::writeDisabledFrame() {
  uint8_t buf[1];
  buf[0] = RESP_CODE_DISABLED;
  _serial->writeFrame(buf, 1);
}

size_t MyMesh::writeContactRespFrame(uint8_t code, const ContactInfo &contact, bool to_all) {
  int i = 0;
  out_frame[i++] = code;
  memcpy(&out_frame[i], contact.id.pub_key, PUB_KEY_SIZE);
  i += PUB_KEY_SIZE;
  out_frame[i++] = contact.type;
  out_frame[i++] = contact.flags;
  out_frame[i++] = contact.out_path_len;
  memcpy(&out_frame[i], contact.out_path, MAX_PATH_SIZE);
  i += MAX_PATH_SIZE;
  StrHelper::strzcpy((char *)&out_frame[i], contact.name, 32);
  i += 32;
  memcpy(&out_frame[i], &contact.last_advert_timestamp, 4);
  i += 4;
  memcpy(&out_frame[i], &contact.gps_lat, 4);
  i += 4;
  memcpy(&out_frame[i], &contact.gps_lon, 4);
  i += 4;
  memcpy(&out_frame[i], &contact.lastmod, 4);
  i += 4;
  if (to_all)
    return _serial->writeFrameToAll(out_frame, i);
  return _serial->writeFrame(out_frame, i);
}

const uint8_t MyMesh::MESHCOMOD_PUB_KEY_PREFIX[6] = { 0x4D, 0x45, 0x53, 0x48, 0x43, 0x4D }; // "MESHCM"

void MyMesh::getMeshcomodContact(ContactInfo& dest) {
  memset(&dest, 0, sizeof(dest));
  memcpy(dest.id.pub_key, MESHCOMOD_PUB_KEY_PREFIX, 6);
  StrHelper::strncpy(dest.name, MESHCOMOD_NAME, sizeof(dest.name) - 1);
  dest.type = ADV_TYPE_CHAT;
  dest.flags = 0;
  dest.out_path_len = -1;
  dest.last_advert_timestamp = 0;
  dest.lastmod = 0;
}

bool MyMesh::isMeshcomodRecipient(const uint8_t* pub_key_prefix_6) const {
  return pub_key_prefix_6 && memcmp(pub_key_prefix_6, MESHCOMOD_PUB_KEY_PREFIX, 6) == 0;
}

// On-device terminal sink: when the Terminal UI is open it registers a callback
// here so command replies (and async ones like "wifi scan") also land in the
// terminal log, not just the companion serial frames.
static void (*s_terminal_sink)(const char*) = nullptr;
void MyMesh::setTerminalSink(void (*cb)(const char*)) { s_terminal_sink = cb; }
void MyMesh::runLocalCli(const char* cmd) {
  if (cmd && *cmd) handleMeshcomodCommand(cmd, (int)strlen(cmd));
}

void MyMesh::pushMeshcomodReply(const char* text, bool immediate_current) {
  if (!text) return;
  if (s_terminal_sink) s_terminal_sink(text);
  int total_len = (int)strlen(text);
  if (total_len <= 0) return;

  // Header bytes before message text:
  // code(1), snr/reserved(3), sender_prefix(6), path_len(1), txt_type(1), timestamp(4) = 16
  const int header_len = 16;
  const int max_text_per_frame = MAX_FRAME_SIZE - header_len;
  if (max_text_per_frame <= 0) return;

  int pos = 0;
  while (pos < total_len) {
    int remaining = total_len - pos;
    int take = remaining < max_text_per_frame ? remaining : max_text_per_frame;

    // Prefer splitting on newline so command/help lines stay intact.
    if (remaining > max_text_per_frame) {
      int split = -1;
      for (int k = take - 1; k >= 0; k--) {
        char c = text[pos + k];
        if (c == '\n') {
          split = k + 1;
          break;
        }
      }
      // Always split at previous newline when available.
      // If no newline exists in this window, this is a single very long line and we must hard-split.
      if (split > 0) take = split;
    }

    int j = 0;
    out_frame[j++] = RESP_CODE_CONTACT_MSG_RECV_V3;
    out_frame[j++] = 0;
    out_frame[j++] = 0;
    out_frame[j++] = 0;
    memcpy(&out_frame[j], MESHCOMOD_PUB_KEY_PREFIX, 6);
    j += 6;
    out_frame[j++] = 0xFF;
    out_frame[j++] = TXT_TYPE_PLAIN;
    uint32_t ts = getRTCClock()->getCurrentTimeUnique();
    // Some clients coalesce messages by sender+timestamp; enforce strictly monotonic ts.
    if (ts <= s_meshcomod_last_reply_ts) ts = s_meshcomod_last_reply_ts + 1;
    s_meshcomod_last_reply_ts = ts;
    memcpy(&out_frame[j], &ts, 4);
    j += 4;

    memcpy(&out_frame[j], &text[pos], take);
    j += take;

    if (immediate_current && _serial->isConnected()) {
      // Immediate emit to current client connection (no waiting for sync polling).
      _serial->writeFrame(out_frame, j);
    }
    addToHistoryRing(out_frame, j);
    if (_serial->isConnected()) {
      uint8_t tickle[1] = { PUSH_CODE_MSG_WAITING };
      _serial->writeFrameToAll(tickle, 1);
    }
    pos += take;
  }
}

bool MyMesh::handleMeshcomodCommand(const char* text, int text_len) {
  if (!text || text_len <= 0) {
    pushMeshcomodReply(kMeshcomodHelpMsg);
    return true;
  }
  char buf[128];
  if ((size_t)text_len >= sizeof(buf)) text_len = (int)sizeof(buf) - 1;
  memcpy(buf, text, (size_t)text_len);
  buf[text_len] = '\0';

  const char* p = buf;
  while (*p == ' ' || *p == '\t') p++;

  // Safety confirmation flow for disruptive transport-off commands.
  if (s_meshcomod_pending_action != MESHCOMOD_PENDING_NONE) {
    uint32_t now_ms = millis();
    if ((int32_t)(s_meshcomod_pending_until_ms - now_ms) < 0) {
      s_meshcomod_pending_action = MESHCOMOD_PENDING_NONE;
      s_meshcomod_pending_until_ms = 0;
      pushMeshcomodReply("pending confirmation expired");
    } else if (strncasecmp(p, "ok", 2) == 0 && (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
      if (s_meshcomod_pending_action == MESHCOMOD_PENDING_TCP_OFF) {
        _serial->disableTcp();
        pushMeshcomodReply("OK tcp=off");
      } else if (s_meshcomod_pending_action == MESHCOMOD_PENDING_BLE_OFF) {
        _serial->disableBle();
        pushMeshcomodReply("OK ble=off");
      }
      s_meshcomod_pending_action = MESHCOMOD_PENDING_NONE;
      s_meshcomod_pending_until_ms = 0;
      return true;
    } else if (strncasecmp(p, "cancel", 6) == 0 && (p[6] == '\0' || p[6] == ' ' || p[6] == '\t')) {
      s_meshcomod_pending_action = MESHCOMOD_PENDING_NONE;
      s_meshcomod_pending_until_ms = 0;
      pushMeshcomodReply("cancelled");
      return true;
    }
  }

  if (strncasecmp(p, "help", 4) == 0 && (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
    pushMeshcomodReply(kMeshcomodHelpMsg);
    return true;
  }

  // ---- Native MeshCore CLI commands (on-device terminal) ----
  auto isCmd = [](const char* s, const char* name) -> bool {
    size_t n = strlen(name);
    return strncasecmp(s, name, n) == 0 && (s[n] == '\0' || s[n] == ' ' || s[n] == '\t');
  };

  if (isCmd(p, "ver") || isCmd(p, "version")) {
    char r[96];
    snprintf(r, sizeof r, "Meshcomod %s\nbuild %s  (code %d)",
             FIRMWARE_VERSION, FIRMWARE_BUILD_DATE, FIRMWARE_VER_CODE);
    pushMeshcomodReply(r);
    return true;
  }
  if (isCmd(p, "clock") || isCmd(p, "time")) {
    time_t tt = (time_t)getRTCClock()->getCurrentTime();
    struct tm tmv; gmtime_r(&tt, &tmv);
    char r[64];
    snprintf(r, sizeof r, "clock: %04d-%02d-%02d %02d:%02d:%02d UTC",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    pushMeshcomodReply(r);
    return true;
  }
  if (isCmd(p, "advert.zerohop")) {
    pushMeshcomodReply(sendAdvert(false) ? "advert sent (zero-hop)" : "advert failed");
    return true;
  }
  if (isCmd(p, "advert")) {
    pushMeshcomodReply(sendAdvert(true) ? "advert sent (flood)" : "advert failed");
    return true;
  }
  if (isCmd(p, "reboot")) {
    pushMeshcomodReply("rebooting...");
    delay(150);
    board.reboot();
    return true;   // not reached
  }
  if (isCmd(p, "bootloader") || isCmd(p, "dfu")) {
#ifdef ESP32
    // Force the ROM into serial/USB download mode on the next reset, so the
    // board can be flashed without the (flaky) trackball+reset combo. The
    // FORCE_DOWNLOAD_BOOT bit lives in the RTC domain and survives the restart.
    pushMeshcomodReply("rebooting into download mode (screen goes dark)...");
    delay(200);
#ifdef PIN_TFT_LEDA_CTL
    if (PIN_TFT_LEDA_CTL >= 0) {        // blank the backlight = clear "in download mode" signal
      pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
      digitalWrite(PIN_TFT_LEDA_CTL, LOW);
    }
#endif
    // Read-modify-write ONLY the force-download bit. A full REG_WRITE zeroes the
    // rest of RTC_CNTL_OPTION1 and wedges the RTC so esp_restart() hangs instead
    // of resetting (that was the earlier "freeze"). board.reboot() == esp_restart,
    // the proven reset path on this board; the RTC bit survives it.
    uint32_t opt1 = REG_READ(RTC_CNTL_OPTION1_REG);
    REG_WRITE(RTC_CNTL_OPTION1_REG, opt1 | RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    board.reboot();
#else
    pushMeshcomodReply("bootloader: ESP32-only");
#endif
    return true;   // not reached on ESP32
  }
  if (isCmd(p, "get")) {
    NodePrefs* pr = getNodePrefs();
    char r[256];
    snprintf(r, sizeof r,
             "name: %s\nfreq: %.3f MHz\nbw: %.1f kHz\nsf: %u\ncr: %u\ntx: %d dBm\nlat: %.5f\nlon: %.5f",
             pr->node_name, pr->freq, pr->bw, (unsigned)pr->sf, (unsigned)pr->cr,
             (int)pr->tx_power_dbm, sensors.node_lat, sensors.node_lon);
    pushMeshcomodReply(r);
    return true;
  }
  if (isCmd(p, "set")) {
    const char* q = p + 3;
    while (*q == ' ' || *q == '\t') q++;
    char param[16]; int pi = 0;
    while (*q && *q != ' ' && *q != '\t' && pi < (int)sizeof(param) - 1) param[pi++] = *q++;
    param[pi] = '\0';
    while (*q == ' ' || *q == '\t') q++;   // q -> value
    NodePrefs* pr = getNodePrefs();
    bool ok = true;
    if      (strcasecmp(param, "name") == 0) { StrHelper::strncpy(pr->node_name, q, sizeof(pr->node_name) - 1); }
    else if (strcasecmp(param, "freq") == 0) { pr->freq = (float)atof(q); }
    else if (strcasecmp(param, "bw")   == 0) { pr->bw   = (float)atof(q); }
    else if (strcasecmp(param, "sf")   == 0) { pr->sf   = (uint8_t)atoi(q); }
    else if (strcasecmp(param, "cr")   == 0) { pr->cr   = (uint8_t)atoi(q); }
    else if (strcasecmp(param, "tx")   == 0) { pr->tx_power_dbm = (int8_t)atoi(q); }
    else ok = false;
    if (ok) { savePrefs(); pushMeshcomodReply("ok (radio changes apply after reboot)"); }
    else    { pushMeshcomodReply("set <name|freq|bw|sf|cr|tx> <value>"); }
    return true;
  }

  if (strncasecmp(p, "status", 6) == 0 && (p[6] == '\0' || p[6] == ' ' || p[6] == '\t')) {
    const char* tcp = _serial->isTcpEnabled() ? "on" : "off";
    char ble[32];
    if (_serial->hasBleCapability()) {
      if (_serial->isBleEnabled()) {
        char peer[24];
        if (_serial->getBlePeerAddress(peer, sizeof(peer)))
          snprintf(ble, sizeof(ble), "on (%s)", peer);
        else
          snprintf(ble, sizeof(ble), "on");
      } else {
        snprintf(ble, sizeof(ble), "off");
      }
    } else {
      snprintf(ble, sizeof(ble), "n/a");
    }
    char ws_line[24];
    if (_serial->isWsStarted())
      snprintf(ws_line, sizeof(ws_line), "ws: %u", (unsigned)_serial->getWsPort());
    else
      snprintf(ws_line, sizeof(ws_line), "ws: off");
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
    char wifi[120];
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      String ssid = WiFi.SSID();
      snprintf(wifi, sizeof(wifi), "wifi: connected\nssid: %s\nip: %d.%d.%d.%d",
               ssid.length() ? ssid.c_str() : "(unknown)", ip[0], ip[1], ip[2], ip[3]);
    } else {
      snprintf(wifi, sizeof(wifi), "wifi: disconnected");
    }
    char status[280];
    snprintf(status, sizeof(status), "companion status:\nusb: on\nble: %s\ntcp: %s\n%s\n%s", ble, tcp, ws_line, wifi);
    pushMeshcomodReply(status);
    return true;
#endif
#endif
    char status_basic[180];
    snprintf(status_basic, sizeof(status_basic), "companion status:\nusb: on\nble: %s\ntcp: %s\n%s", ble, tcp, ws_line);
    pushMeshcomodReply(status_basic);
    return true;
  }

  if (strncasecmp(p, "tcp", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "on", 2) == 0 && (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
      _serial->enableTcp();
      pushMeshcomodReply("OK\ntcp: on");
      return true;
    }
    if (strncasecmp(p, "off", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
      s_meshcomod_pending_action = MESHCOMOD_PENDING_TCP_OFF;
      s_meshcomod_pending_until_ms = millis() + 30000UL;
      pushMeshcomodReply("warning: turning TCP off may remove wireless access to this companion.");
      pushMeshcomodReply("if BLE is also off, you will need physical USB access.");
      pushMeshcomodReply("reply 'ok' within 30s to confirm, or 'cancel'.");
      return true;
    }
    if (strncasecmp(p, "status", 6) == 0 && (p[6] == '\0' || p[6] == ' ' || p[6] == '\t')) {
      pushMeshcomodReply(_serial->isTcpEnabled() ? "tcp: on" : "tcp: off");
      return true;
    }
    pushMeshcomodReply("usage:\n- tcp on\n- tcp off\n- tcp status");
    return true;
  }

  if (strncasecmp(p, "ble", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
    if (!_serial->hasBleCapability()) {
      pushMeshcomodReply("ble=n/a");
      return true;
    }
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "on", 2) == 0 && (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
      _serial->enableBle();
      pushMeshcomodReply("OK\nble: on");
      return true;
    }
    if (strncasecmp(p, "off", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
      s_meshcomod_pending_action = MESHCOMOD_PENDING_BLE_OFF;
      s_meshcomod_pending_until_ms = millis() + 30000UL;
      pushMeshcomodReply("warning: turning BLE off may remove wireless access to this companion.");
      pushMeshcomodReply("if TCP is also off, you will need physical USB access.");
      pushMeshcomodReply("reply 'ok' within 30s to confirm, or 'cancel'.");
      return true;
    }
    if (strncasecmp(p, "status", 6) == 0 && (p[6] == '\0' || p[6] == ' ' || p[6] == '\t')) {
      if (_serial->isBleEnabled()) {
        char peer[24];
        if (_serial->getBlePeerAddress(peer, sizeof(peer))) {
          char msg[56];
          snprintf(msg, sizeof(msg), "ble: on\npeer: %s", peer);
          pushMeshcomodReply(msg);
        } else {
          pushMeshcomodReply("ble: on");
        }
      } else {
        pushMeshcomodReply("ble: off");
      }
      return true;
    }
    pushMeshcomodReply("usage:\n- ble on\n- ble off\n- ble status");
    return true;
  }

  if (strncasecmp(p, "ota", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "start", 5) == 0 && (p[5] == '\0' || p[5] == ' ' || p[5] == '\t')) {
      char reply[160];
      if (board.startOTAUpdate(_prefs.node_name, reply)) {
        pushMeshcomodReply(reply);
      } else {
        pushMeshcomodReply("ERR: OTA not supported in this build");
      }
      return true;
    }
    if (strncasecmp(p, "netdiag", 7) == 0 && (p[7] == '\0' || p[7] == ' ' || p[7] == '\t')) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
      board.emitHttpOtaNetDiagnosticLines();
      pushMeshcomodReply("OK ota netdiag (see binary lines)");
#else
      pushMeshcomodReply("ERR: ota netdiag n/a");
#endif
#else
      pushMeshcomodReply("ERR: ota netdiag n/a");
#endif
      return true;
    }
    if (strncasecmp(p, "url", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
      p += 3;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '\0') {
        pushMeshcomodReply("ERR: missing URL");
        return true;
      }
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
      {
        char reply[160] = {0};
#ifdef MULTI_TRANSPORT_COMPANION
        {
          int rt = _serial ? _serial->getReplyTarget() : REPLY_TARGET_USB;
          if (rt == REPLY_TARGET_USB || rt == REPLY_TARGET_BLE) {
            meshcoreRepeaterTcpOtaEmitLine("OTA: rejected need Wi-Fi TCP/WS control session");
            pushMeshcomodReply(rt == REPLY_TARGET_BLE
                                   ? "ERR: HTTP OTA must be started from Wi-Fi TCP or WebSocket (not BLE)"
                                   : "ERR: HTTP OTA must be started from Wi-Fi TCP or WebSocket (not USB)");
            return true;
          }
          s_companion_ota_pinned_reply_target = rt;
          if (_serial) _serial->prepareForHttpOta();
          bool handled = board.startHttpOtaFromUrl(p, reply);
          if (_serial && (strncmp(reply, "ERR:", 4) == 0 || !handled)) _serial->restoreAfterHttpOta();
          if (!handled) {
            StrHelper::strncpy(reply, "ERR: OTA URL not supported", sizeof(reply));
          }
          if (_serial) _serial->setReplyTarget(rt);
          pushMeshcomodReply(reply);
          pushCompanionOtaProgressLine(reply);
          s_companion_ota_pinned_reply_target = -1;
        }
#elif defined(WIFI_SSID)
        {
          if (!_serial || !_serial->isHttpOtaWifiControlSession()) {
            meshcoreRepeaterTcpOtaEmitLine("OTA: rejected need active Wi-Fi companion TCP session");
            pushMeshcomodReply(WiFi.status() != WL_CONNECTED
                                   ? "ERR: WiFi not connected"
                                   : "ERR: HTTP OTA must be started from an active Wi-Fi companion connection");
            return true;
          }
          bool handled = board.startHttpOtaFromUrl(p, reply);
          if (!handled) {
            StrHelper::strncpy(reply, "ERR: OTA URL not supported", sizeof(reply));
          }
          pushMeshcomodReply(reply);
          pushCompanionOtaProgressLine(reply);
        }
#endif
      }
#else
      char reply[160];
      if (board.startHttpOtaFromUrl(p, reply)) {
        pushMeshcomodReply(reply);
      } else {
        pushMeshcomodReply("ERR: OTA URL not supported");
      }
#endif
#else
      char reply[160];
      if (board.startHttpOtaFromUrl(p, reply)) {
        pushMeshcomodReply(reply);
      } else {
        pushMeshcomodReply("ERR: OTA URL not supported");
      }
#endif
      return true;
    }
    if (strncasecmp(p, "status", 6) == 0 && (p[6] == '\0' || p[6] == ' ' || p[6] == '\t')) {
      char line[96];
      if (g_meshcore_http_ota_display_active) {
        if (g_meshcore_http_ota_display_pct == 0xFF) {
          snprintf(line, sizeof(line), "ota: active\n%s", g_meshcore_http_ota_display_line[0] ? g_meshcore_http_ota_display_line : "working");
        } else {
          snprintf(line, sizeof(line), "ota: %u%%\n%s", (unsigned)g_meshcore_http_ota_display_pct,
                   g_meshcore_http_ota_display_line[0] ? g_meshcore_http_ota_display_line : "working");
        }
      } else {
        snprintf(line, sizeof(line), "ota: idle");
      }
      pushMeshcomodReply(line);
      return true;
    }
    pushMeshcomodReply("usage:\n- ota start\n- ota url <https://...bin>\n- ota netdiag\n- ota status");
    return true;
  }

  if (strncasecmp(p, "wifi", 4) != 0 || (p[4] != '\0' && p[4] != ' ' && p[4] != '\t')) {
    pushMeshcomodReply(kMeshcomodHelpMsg);
    return true;
  }
  p += 4;
  while (*p == ' ' || *p == '\t') p++;

#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
  if (strncasecmp(p, "on", 2) == 0 && (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
    wifiConfigSetRadioEnabled(true);
    pushMeshcomodReply("OK wifi radio on");
    return true;
  }
  if (strncasecmp(p, "off", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
    wifiConfigSetRadioEnabled(false);
    pushMeshcomodReply("OK wifi radio off");
    return true;
  }
  if (strncasecmp(p, "set", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "ssid", 4) == 0 && (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
      p += 4;
      while (*p == ' ' || *p == '\t') p++;
      char* ssid = unquoteInPlace((char*)p);
      if (wifiConfigSetSsid(ssid)) {
        char ok[168];
        snprintf(ok, sizeof(ok),
                 "OK ssid=\"%s\"\nnext: wifi set pwd \"<password>\" (or \"\" for open)\nthen: wifi apply",
                 ssid);
        pushMeshcomodReply(ok);
      } else {
        pushMeshcomodReply("error: ssid too long or invalid");
      }
      return true;
    }
    if (strncasecmp(p, "pwd", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
      p += 3;
      while (*p == ' ' || *p == '\t') p++;
      char* pwd = unquoteInPlace((char*)p);
      if (wifiConfigSetPwd(pwd)) {
        pushMeshcomodReply("OK\npwd set\nnext: wifi apply");
      } else {
        pushMeshcomodReply("error: password too long");
      }
      return true;
    }
    pushMeshcomodReply("error: usage wifi set ssid|pwd \"<value>\"");
    return true;
  }
  if (strncasecmp(p, "scan", 4) == 0 && (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
    if (!wifiConfigGetRadioEnabled()) {
      pushMeshcomodReply("error: wifi radio off — send wifi on first");
      return true;
    }
    // Ensure STA is fully up before scan; disconnected STA needs a bit more settle time.
    wifi_mode_t mode = WiFi.getMode();
    if ((mode & WIFI_MODE_STA) == 0) {
      WiFi.mode(WIFI_STA);
      delay(180);
    } else {
      delay(40);
    }
    s_meshcomod_scan_count = 0;
    // Watchdog-safe: one bounded, yielding async pass (see wifiScanWatchdogSafe).
    // This handler runs on the main/loop task, so the old twin synchronous scans
    // (~8 s) could starve the task watchdog with no AP present and panic-reboot.
    int found = wifiScanWatchdogSafe(8000);
    if (found <= 0) {
      pushMeshcomodReply("scan: no networks (2.4GHz only)");
      WiFi.scanDelete();
      return true;
    }
    String scanMsg = "scan results:";
    for (int idx = 0; idx < found && s_meshcomod_scan_count < MESHCOMOD_WIFI_SCAN_MAX; idx++) {
      String ssid = WiFi.SSID(idx);
      if (ssid.length() <= 0) continue;
      bool dup = false;
      for (int k = 0; k < s_meshcomod_scan_count; k++) {
        if (strcmp(s_meshcomod_scan_ssids[k], ssid.c_str()) == 0) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      StrHelper::strzcpy(s_meshcomod_scan_ssids[s_meshcomod_scan_count], ssid.c_str(), WIFI_CONFIG_SSID_MAX);
      int ch = WiFi.channel(idx);
      const char* band = (ch >= 1 && ch <= 14) ? "2.4GHz" : "5GHz";
      char line[96];
      snprintf(line, sizeof(line), "\n%d) %s [%s]", s_meshcomod_scan_count + 1, s_meshcomod_scan_ssids[s_meshcomod_scan_count], band);
      scanMsg += line;
      s_meshcomod_scan_count++;
    }
    // Fallback: if scan didn't surface any visible SSIDs, still show connected SSID.
    if (s_meshcomod_scan_count == 0) {
      String curr = WiFi.SSID();
      if (curr.length() > 0) {
        StrHelper::strzcpy(s_meshcomod_scan_ssids[0], curr.c_str(), WIFI_CONFIG_SSID_MAX);
        s_meshcomod_scan_count = 1;
        int ch = WiFi.channel();
        const char* band = (ch >= 1 && ch <= 14) ? "2.4GHz" : "5GHz";
        char line[96];
        snprintf(line, sizeof(line), "\n1) %s [%s] (connected)", s_meshcomod_scan_ssids[0], band);
        scanMsg += line;
      }
    }
    if (s_meshcomod_scan_count == 0) {
      pushMeshcomodReply("scan: no usable SSIDs");
      pushMeshcomodReply("tip: try wifi status or move closer to AP");
    } else {
      scanMsg += "\nselect SSID: wifi use <n>";
      scanMsg += "\nthen set password: wifi set pwd \"<password>\" and wifi apply";
      pushMeshcomodReply(scanMsg.c_str());
    }
    WiFi.scanDelete();
    return true;
  }
  if (strncasecmp(p, "use", 3) == 0 && (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    int n = atoi(p);
    if (s_meshcomod_scan_count <= 0) {
      pushMeshcomodReply("error: no scan results; run wifi scan");
      return true;
    }
    if (n < 1 || n > s_meshcomod_scan_count) {
      pushMeshcomodReply("error: invalid index");
      return true;
    }
    if (!wifiConfigSetSsid(s_meshcomod_scan_ssids[n - 1])) {
      pushMeshcomodReply("error: ssid too long or invalid");
      return true;
    }
    char line[192];
    snprintf(line, sizeof(line), "OK ssid=\"%s\"\nnext: wifi set pwd \"<password>\" (or \"\" for open)\nthen: wifi apply", s_meshcomod_scan_ssids[n - 1]);
    pushMeshcomodReply(line);
    return true;
  }
  if (strncasecmp(p, "status", 6) == 0 && (p[6] == '\0' || p[6] == ' ' || p[6] == '\t')) {
    char ssid[WIFI_CONFIG_SSID_MAX];
    wifiConfigGetSsid(ssid, sizeof(ssid));
    bool has_runtime = wifiConfigHasRuntime();
    int re = wifiConfigGetRadioEnabled() ? 1 : 0;
    char reply[112];
    if (!has_runtime || ssid[0] == '\0') {
      snprintf(reply, sizeof(reply), "radio_enabled=%d ssid=(none) runtime=0", re);
    } else {
      int connected = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
      if (connected) {
        IPAddress ip = WiFi.localIP();
        snprintf(reply, sizeof(reply), "radio_enabled=%d ssid=%s connected=1 ip=%d.%d.%d.%d", re, ssid,
                 ip[0], ip[1], ip[2], ip[3]);
      } else {
        snprintf(reply, sizeof(reply), "radio_enabled=%d ssid=%s connected=0", re, ssid);
      }
    }
    pushMeshcomodReply(reply);
    return true;
  }
  if (strncasecmp(p, "clear", 5) == 0 && (p[5] == '\0' || p[5] == ' ' || p[5] == '\t')) {
    wifiConfigClear();
    pushMeshcomodReply("OK");
    return true;
  }
  if (strncasecmp(p, "apply", 5) == 0 && (p[5] == '\0' || p[5] == ' ' || p[5] == '\t')) {
    if (!wifiConfigGetRadioEnabled()) {
      pushMeshcomodReply("error: wifi radio off — send wifi on first");
      return true;
    }
    if (!wifiConfigHasRuntime()) {
      pushMeshcomodReply("No runtime credentials; set ssid/pwd first");
      return true;
    }
    wifiConfigApply();
    pushMeshcomodReply("OK reconnecting");
    return true;
  }
#endif
#endif
  pushMeshcomodReply(kMeshcomodHelpMsg);
  return true;
}

void MyMesh::updateContactFromFrame(ContactInfo &contact, uint32_t& last_mod, const uint8_t *frame, int len) {
  int i = 0;
  uint8_t code = frame[i++]; // eg. CMD_ADD_UPDATE_CONTACT
  memcpy(contact.id.pub_key, &frame[i], PUB_KEY_SIZE);
  i += PUB_KEY_SIZE;
  contact.type = frame[i++];
  contact.flags = frame[i++];
  contact.out_path_len = frame[i++];
  memcpy(contact.out_path, &frame[i], MAX_PATH_SIZE);
  i += MAX_PATH_SIZE;
  memcpy(contact.name, &frame[i], 32);
  i += 32;
  memcpy(&contact.last_advert_timestamp, &frame[i], 4);
  i += 4;
  if (len >= i + 8) { // optional fields
    memcpy(&contact.gps_lat, &frame[i], 4);
    i += 4;
    memcpy(&contact.gps_lon, &frame[i], 4);
    i += 4;
    if (len >= i + 4) {
      memcpy(&last_mod, &frame[i], 4);
    }
  }
}

bool MyMesh::Frame::isChannelMsg() const {
  return buf[0] == RESP_CODE_CHANNEL_MSG_RECV || buf[0] == RESP_CODE_CHANNEL_MSG_RECV_V3;
}

void MyMesh::addToOfflineQueue(const uint8_t frame[], int len) {
  if (offline_queue_len >= OFFLINE_QUEUE_SIZE) {
    MESH_DEBUG_PRINTLN("WARN: offline_queue is full!");
    int pos = 0;
    while (pos < offline_queue_len) {
      if (offline_queue[pos].isChannelMsg()) {
        for (int i = pos; i < offline_queue_len - 1; i++) { // delete oldest channel msg from queue
          offline_queue[i] = offline_queue[i + 1];
        }
        MESH_DEBUG_PRINTLN("INFO: removed oldest channel message from queue.");
        offline_queue[offline_queue_len - 1].len = len;
        memcpy(offline_queue[offline_queue_len - 1].buf, frame, len);
        return;
      }
      pos++;
    }
    MESH_DEBUG_PRINTLN("INFO: no channel messages to remove from queue.");
  } else {
    offline_queue[offline_queue_len].len = len;
    memcpy(offline_queue[offline_queue_len].buf, frame, len);
    offline_queue_len++;
  }
}

int MyMesh::getFromOfflineQueue(uint8_t frame[]) {
  if (offline_queue_len > 0) {         // check offline queue
    size_t len = offline_queue[0].len; // take from top of queue
    memcpy(frame, offline_queue[0].buf, len);

    offline_queue_len--;
    for (int i = 0; i < offline_queue_len; i++) { // delete top item from queue
      offline_queue[i] = offline_queue[i + 1];
    }
    return len;
  }
  return 0; // queue is empty
}

uint32_t MyMesh::addToHistoryRing(const uint8_t frame[], int len) {
  if (len <= 0 || len > MAX_FRAME_SIZE) return 0;
  HistoryEntry& e = history_ring[history_head];
  e.len = (uint8_t)len;
  memcpy(e.buf, frame, len);
  uint32_t assigned = history_next_seq;
  e.seq = history_next_seq++;
  history_head = (history_head + 1) % HISTORY_RING_SIZE;
  if (history_count < HISTORY_RING_SIZE) {
    history_count++;
  }
  return assigned;
}

static bool clientIdEqual(const char* a, const char* b) {
  if (!a) a = "";
  if (!b) b = "";
  return strcmp(a, b) == 0;
}

static bool containsIgnoreCaseAscii(const char* haystack, const char* needle) {
  if (!haystack || !needle || !needle[0]) return false;
  for (int i = 0; haystack[i]; i++) {
    int j = 0;
    while (needle[j]) {
      char hc = haystack[i + j];
      if (!hc) return false;
      if (hc >= 'A' && hc <= 'Z') hc = (char)(hc - 'A' + 'a');
      char nc = needle[j];
      if (nc >= 'A' && nc <= 'Z') nc = (char)(nc - 'A' + 'a');
      if (hc != nc) break;
      j++;
    }
    if (!needle[j]) return true;
  }
  return false;
}

static bool appPrefersLiveAdvance(const char* app_name) {
  if (!app_name || !app_name[0]) return false;
  // meshcomod/web clients explicitly identify as mccli / meshcomod-*
  return containsIgnoreCaseAscii(app_name, "mccli") ||
         containsIgnoreCaseAscii(app_name, "meshcomod");
}

// Extract Unix timestamp from a message frame for SyncSince filtering. Returns 0 if not V3 or too short.
static uint32_t getMessageTimestampFromFrame(const uint8_t* buf, int len) {
  if (!buf || len < 11) return 0;
  uint8_t code = buf[0];
  if (code == RESP_CODE_CONTACT_MSG_RECV_V3 && len >= 16) {
    uint32_t t;
    memcpy(&t, &buf[12], 4);
    return t;
  }
  if (code == RESP_CODE_CHANNEL_MSG_RECV_V3 && len >= 11) {
    uint32_t t;
    memcpy(&t, &buf[7], 4);
    return t;
  }
  return 0;
}

// Delivers history to client for sync. Includes channel messages (0x08, 0x11); clients without channel support should skip those frame types.
int MyMesh::getNextFromHistoryForClient(const char* client_id, uint8_t frame[], uint32_t* out_seq, bool do_advance) {
  if (history_count <= 0) return 0;
  const char* cid = (client_id && client_id[0]) ? client_id : "";

  // Find or create client state
  int slot = -1;
  for (int i = 0; i < history_num_clients; i++) {
    if (clientIdEqual(history_clients[i].client_id, cid)) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    if (history_num_clients >= MAX_HISTORY_CLIENTS) return 0;
    slot = history_num_clients++;
    strncpy(history_clients[slot].client_id, cid, MAX_CLIENT_ID_LEN);
    history_clients[slot].client_id[MAX_CLIENT_ID_LEN] = '\0';
    history_clients[slot].last_delivered_seq = history_next_seq > (uint32_t)history_count
      ? history_next_seq - (uint32_t)history_count : 0;
  }

  uint32_t last = history_clients[slot].last_delivered_seq;
  int tail = (history_head - history_count + HISTORY_RING_SIZE) % HISTORY_RING_SIZE;
  for (int i = 0; i < history_count; i++) {
    int idx = (tail + i) % HISTORY_RING_SIZE;
    const HistoryEntry& e = history_ring[idx];
    if (e.seq > last) {
      // Sync stream should return only chat/channel message frames.
      // Other push types (e.g. RX log 0x88) are delivered live via push and can confuse client sync parsers.
      uint8_t code = e.buf[0];
      bool is_sync_message =
        code == RESP_CODE_CONTACT_MSG_RECV ||
        code == RESP_CODE_CONTACT_MSG_RECV_V3 ||
        code == RESP_CODE_CHANNEL_MSG_RECV ||
        code == RESP_CODE_CHANNEL_MSG_RECV_V3;
      if (!is_sync_message) {
        if (do_advance) history_clients[slot].last_delivered_seq = e.seq;
        last = e.seq;
        continue;
      }
      memcpy(frame, e.buf, e.len);
      if (out_seq) *out_seq = e.seq;
      if (do_advance) history_clients[slot].last_delivered_seq = e.seq;
      return e.len;
    }
  }
  return 0;
}

void MyMesh::commitHistoryForClient(const char* client_id, uint32_t seq) {
  const char* cid = (client_id && client_id[0]) ? client_id : "";
  for (int i = 0; i < history_num_clients; i++) {
    if (clientIdEqual(history_clients[i].client_id, cid)) {
      if (seq > history_clients[i].last_delivered_seq)
        history_clients[i].last_delivered_seq = seq;
      return;
    }
  }
}

void MyMesh::advanceHistoryClientsAfterV3Broadcast(uint32_t seq) {
  for (int i = 0; i < history_num_clients; i++) {
    if (!shouldAdvanceClientAfterV3Broadcast(history_clients[i].client_id))
      continue;
    if (seq > history_clients[i].last_delivered_seq)
      history_clients[i].last_delivered_seq = seq;
  }
}

void MyMesh::setClientTargetVer(const char* client_id, uint8_t target_ver) {
  const char* cid = (client_id && client_id[0]) ? client_id : "";
  for (int i = 0; i < proto_num_clients; i++) {
    if (clientIdEqual(proto_clients[i].client_id, cid)) {
      proto_clients[i].target_ver = target_ver;
      return;
    }
  }
  if (proto_num_clients < MAX_HISTORY_CLIENTS) {
    int slot = proto_num_clients++;
    strncpy(proto_clients[slot].client_id, cid, MAX_CLIENT_ID_LEN);
    proto_clients[slot].client_id[MAX_CLIENT_ID_LEN] = '\0';
    proto_clients[slot].target_ver = target_ver;
    proto_clients[slot].prefer_live_advance = false;
  }
}

uint8_t MyMesh::getClientTargetVer(const char* client_id) const {
  const char* cid = (client_id && client_id[0]) ? client_id : "";
  for (int i = 0; i < proto_num_clients; i++) {
    if (clientIdEqual(proto_clients[i].client_id, cid)) {
      return proto_clients[i].target_ver;
    }
  }
  // Unknown clients default to modern format.
  return 0xFF;
}

void MyMesh::setClientAppName(const char* client_id, const char* app_name) {
  const char* cid = (client_id && client_id[0]) ? client_id : "";
  bool prefer_live_advance = appPrefersLiveAdvance(app_name);
  for (int i = 0; i < proto_num_clients; i++) {
    if (clientIdEqual(proto_clients[i].client_id, cid)) {
      proto_clients[i].prefer_live_advance = prefer_live_advance;
      return;
    }
  }
  if (proto_num_clients < MAX_HISTORY_CLIENTS) {
    int slot = proto_num_clients++;
    strncpy(proto_clients[slot].client_id, cid, MAX_CLIENT_ID_LEN);
    proto_clients[slot].client_id[MAX_CLIENT_ID_LEN] = '\0';
    proto_clients[slot].target_ver = 0xFF;
    proto_clients[slot].prefer_live_advance = prefer_live_advance;
  }
}

bool MyMesh::shouldAdvanceClientAfterV3Broadcast(const char* client_id) const {
  const char* cid = (client_id && client_id[0]) ? client_id : "";
  for (int i = 0; i < proto_num_clients; i++) {
    if (clientIdEqual(proto_clients[i].client_id, cid)) {
      uint8_t tv = proto_clients[i].target_ver;
      if (tv < 3 && tv != 0xFF) return false; // legacy sync-adapt clients must keep replay
      return proto_clients[i].prefer_live_advance;
    }
  }
  // Unknown client app: conservative default for stock compatibility.
  return false;
}

int MyMesh::adaptHistoryFrameForClient(const char* client_id, const uint8_t src[], int src_len, uint8_t dest[]) const {
  if (!src || !dest || src_len <= 0 || src_len > MAX_FRAME_SIZE) return 0;
  uint8_t target_ver = getClientTargetVer(client_id);
  if (target_ver >= 3 || target_ver == 0xFF) {
    memcpy(dest, src, src_len);
    return src_len;
  }
  if (src[0] == RESP_CODE_CONTACT_MSG_RECV_V3 && src_len >= 4) {
    dest[0] = RESP_CODE_CONTACT_MSG_RECV;
    memcpy(&dest[1], &src[4], src_len - 4);
    return src_len - 3;
  }
  if (src[0] == RESP_CODE_CHANNEL_MSG_RECV_V3 && src_len >= 4) {
    dest[0] = RESP_CODE_CHANNEL_MSG_RECV;
    memcpy(&dest[1], &src[4], src_len - 4);
    return src_len - 3;
  }
  memcpy(dest, src, src_len);
  return src_len;
}

// SyncSince (CMD 62): send all message frames (7/8/16/17) with timestamp >= T from history ring, then SyncSinceDone (61).
// Client must use command 62 (not 60; 60 is CMD_GET_ALLOWED_REPEAT_FREQ). Response 61 = SyncSinceDone; client sets last-sync to now.
void MyMesh::sendSyncSinceDelta(uint32_t T) {
  char client_id[MAX_CLIENT_ID_LEN + 1];
  _serial->getCurrentClientId(client_id, sizeof(client_id));

  uint8_t send_buf[MAX_FRAME_SIZE];
  int tail = (history_head - history_count + HISTORY_RING_SIZE) % HISTORY_RING_SIZE;

  for (int i = 0; i < history_count; i++) {
    int idx = (tail + i) % HISTORY_RING_SIZE;
    const HistoryEntry& e = history_ring[idx];
    uint8_t code = e.buf[0];

    if (code != RESP_CODE_CONTACT_MSG_RECV && code != RESP_CODE_CONTACT_MSG_RECV_V3 &&
        code != RESP_CODE_CHANNEL_MSG_RECV && code != RESP_CODE_CHANNEL_MSG_RECV_V3)
      continue;

    bool include = false;
    if (code == RESP_CODE_CONTACT_MSG_RECV_V3 || code == RESP_CODE_CHANNEL_MSG_RECV_V3) {
      uint32_t ts = getMessageTimestampFromFrame(e.buf, e.len);
      include = (ts >= T);
    } else {
      include = (T == 0);
    }
    if (!include) continue;

    const uint8_t* ptr = e.buf;
    int len = e.len;
    int adapted = adaptHistoryFrameForClient(client_id, e.buf, e.len, send_buf);
    if (adapted > 0) {
      ptr = send_buf;
      len = adapted;
    }
    _serial->writeFrame(ptr, len);
  }

  out_frame[0] = RESP_CODE_SYNC_SINCE_DONE;
  _serial->writeFrame(out_frame, 1);
}

float MyMesh::getAirtimeBudgetFactor() const {
  return _prefs.airtime_factor;
}

int MyMesh::getInterferenceThreshold() const {
  return 0; // disabled for now, until currentRSSI() problem is resolved
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint8_t MyMesh::getExtraAckTransmitCount() const {
  return _prefs.multi_acks;
}

uint8_t MyMesh::getAutoAddMaxHops() const {
  return _prefs.autoadd_max_hops;
}

// Lightweight payload fingerprint (FNV-1a 32) for matching our own sent flood
// against its echoes heard back from repeaters (see logRxRaw / uiTrackSentFp).
// The payload is unchanged as repeaters re-flood (only the path grows), so the
// same bytes fingerprint identically at send and on every echo.
static uint32_t fnv1a32(const uint8_t* d, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
  return h ? h : 1;  // never return 0 (our "none" sentinel)
}
// Flood message types we track for "repeats heard": DM text (TXT_MSG 0x02) AND
// group/channel text (GRP_TXT 0x05). Channel posts are NOT TXT_MSG — easy to
// miss, and the reason channel sends fingerprinted to 0 at first.
static inline bool isMsgFloodType(uint8_t t) {
  return t == PAYLOAD_TYPE_TXT_MSG || t == PAYLOAD_TYPE_GRP_TXT;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
  // Live signal for the top-bar icon: SNR of whatever we just heard, with a
  // timestamp so the UI can dim the icon when the mesh has gone quiet.
  _ui_sig_snr_q4 = (int8_t)(snr * 4.0f);
  _ui_sig_rssi   = (int8_t)rssi;
  _ui_sig_ms     = millis();
  // Recent-RX ring for the Monitor app (see UiRxRec): parse the minimal header
  // (type / route / hops) and push newest-first. Header byte layout is
  //   [version:2][payload_type:4][route_type:2]
  // with 4 transport-code bytes following iff route is a TRANSPORT_* one; the
  // next byte's low 6 bits are the path length (hop count).
  if (len > 0) {
    const uint8_t rt = raw[0] & 0x03;
    const bool    xp = (rt == ROUTE_TYPE_TRANSPORT_FLOOD || rt == ROUTE_TYPE_TRANSPORT_DIRECT);
    const int     ps = 1 + (xp ? 4 : 0);
    const uint8_t hops = (ps < len) ? (uint8_t)(raw[ps] & 0x3F) : 0;
    uiRxLogPush(_ui_sig_ms, (int8_t)rssi, _ui_sig_snr_q4,
                (uint8_t)((raw[0] >> 2) & 0x0F), rt, hops,
                (uint8_t)(len > 255 ? 255 : len));
  }
#if defined(DISPLAY_CLASS)
  // Diagnostic: log EVERY received frame so we can prove what reaches the
  // radio. Header byte layout is
  //   [version:2][payload_type:4][route_type:2]   (bits 7..0)
  // so payload_type = (raw[0]>>2)&0x0F and route_type = raw[0]&0x03. The 4
  // transport-code bytes follow the header iff route_type is a TRANSPORT_* one
  // (there is NO "hasXportCodes" bit at 0x80 — that's the top of the version field).
  if (_ui && len > 0) {
    const uint8_t ptype = (raw[0] >> 2) & 0x0F;
    const uint8_t route = raw[0] & 0x03;
    const char* tname = "???";
    switch (ptype) {
      case 0x00: tname = "REQ"; break;
      case 0x01: tname = "RSP"; break;
      case 0x02: tname = "TXT"; break;
      case 0x03: tname = "ACK"; break;
      case 0x04: tname = "ADV"; break;
      case 0x05: tname = "GTX"; break;
      case 0x06: tname = "GDT"; break;
      case 0x07: tname = "ANR"; break;
      case 0x08: tname = "PTH"; break;
      case 0x09: tname = "TRC"; break;
      case 0x0A: tname = "MUL"; break;
      case 0x0B: tname = "CTL"; break;
      case 0x0F: tname = "RAW"; break;
    }
    // Show dest_hash byte (first payload byte after the path) so we can
    // see whether the packet is addressed to us. self_id.pub_key[0] is
    // what isHashMatch compares against — if dest doesn't equal that,
    // the dispatcher silently drops the packet at the "is this for us?"
    // gate without ever calling onPeerDataRecv.
    const uint8_t self_b0 = self_id.pub_key[0];
    uint8_t dest = 0xFF;
    // Header is byte 0; transport codes (if present) are 1-4; path_len is
    // next; then path bytes; then payload. We just want the first byte of
    // the payload as dest_hash for TXT/RSP/ACK style packets.
    const uint8_t rt0 = raw[0] & 0x03;
    const bool has_xport = (rt0 == ROUTE_TYPE_TRANSPORT_FLOOD || rt0 == ROUTE_TYPE_TRANSPORT_DIRECT);
    int payload_start = 1 + (has_xport ? 4 : 0);
    if (payload_start < len) {
      uint8_t path_byte = raw[payload_start];
      uint8_t path_count = path_byte & 0x3F;
      uint8_t hash_size = ((path_byte >> 6) & 0x03) + 1;
      int payload_off = payload_start + 1 + path_count * hash_size;
      if (payload_off < len) dest = raw[payload_off];
    }
    char dbg[80];
    // ADV/GTX/GDT/ACK don't have a dest_hash — show "--" instead so the
    // operator isn't tricked into reading the first ack_crc byte as if
    // it were addressing.
    bool has_dest_hash = (ptype == 0x00 || ptype == 0x01 || ptype == 0x02 ||
                          ptype == 0x07 || ptype == 0x08);
    if (has_dest_hash) {
      snprintf(dbg, sizeof(dbg), "RX %s r=%u dst=%02x me=%02x L=%d s=%d",
               tname, (unsigned)route, (unsigned)dest, (unsigned)self_b0,
               len, (int)rssi);
    } else {
      snprintf(dbg, sizeof(dbg), "RX %s r=%u me=%02x L=%d s=%d",
               tname, (unsigned)route, (unsigned)self_b0,
               len, (int)rssi);
    }
    _ui->appendDiag(dbg);
  }
  // "Repeats heard": our originated flood TXT comes back when repeaters
  // re-broadcast it (identical payload, longer path). Fingerprint the payload
  // and bump the matching sent message's count. The fp ring only holds our own
  // recent sends, so other nodes' traffic can't false-match.
  if (len > 0 && isMsgFloodType((raw[0] >> 2) & 0x0F)) {
    const uint8_t rt  = raw[0] & 0x03;                      // route_type (PH_ROUTE_MASK)
    const bool has_xp = (rt == ROUTE_TYPE_TRANSPORT_FLOOD || // 4 transport-code bytes follow the
                         rt == ROUTE_TYPE_TRANSPORT_DIRECT); // header iff route is a TRANSPORT_* one
    const int  ps     = 1 + (has_xp ? 4 : 0);
    if (ps < len) {
      const uint8_t pb    = raw[ps];
      const uint8_t cnt_p = pb & 0x3F;
      const uint8_t hsz_p = (uint8_t)(((pb >> 6) & 0x03) + 1);
      const int     poff  = ps + 1 + cnt_p * hsz_p;
      if (poff < len) {
        // last path hop = the repeater whose re-flood our radio just heard
        const uint8_t* lasthop = (cnt_p >= 1) ? (raw + poff - hsz_p) : nullptr;
        uiCountEcho(fnv1a32(raw + poff, len - poff), lasthop, lasthop ? hsz_p : 0);
      }
    }
  }
#endif
  if (len + 3 <= MAX_FRAME_SIZE) {
    int i = 0;
    out_frame[i++] = PUSH_CODE_LOG_RX_DATA;
    out_frame[i++] = (int8_t)(snr * 4);
    out_frame[i++] = (int8_t)(rssi);
    memcpy(&out_frame[i], raw, len);
    i += len;

    // Do not add RX log to the sync history ring — it would evict chat/channel messages
    // (e.g. overnight traffic causes late-connecting BLE client to miss messages). Push live only.
    if (_serial->isConnected()) {
      _serial->writeFrameToAll(out_frame, i);
    }
  }
}

void MyMesh::uiExportBackup(Print& out, double node_lat, double node_lon) {
  static const char* HX = "0123456789abcdef";
  auto hex = [&](const uint8_t* d, int n) {
    for (int i = 0; i < n; ++i) { out.write(HX[d[i] >> 4]); out.write(HX[d[i] & 0xF]); }
  };
  auto esc = [&](const char* s) {
    for (; s && *s; ++s) {
      char c = *s;
      if (c == '"' || c == '\\') { out.write('\\'); out.write((uint8_t)c); }
      else if (c == '\n') out.print("\\n");
      else if (c == '\r') out.print("\\r");
      else if (c == '\t') out.print("\\t");
      else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", (unsigned)(uint8_t)c); out.print(b); }
      else out.write((uint8_t)c);
    }
  };
  NodePrefs* p = getNodePrefs();
  uint8_t prv[64]; self_id.writeTo(prv, 64);

  out.print("{\n  \"name\": \""); esc(p ? p->node_name : ""); out.print("\",\n");
  out.print("  \"public_key\": \""); hex(self_id.pub_key, 32); out.print("\",\n");
  out.print("  \"private_key\": \""); hex(prv, 64); out.print("\",\n");
  if (p) {
    char l[176];
    snprintf(l, sizeof l,
      "  \"radio_settings\": {\"frequency\": %.4f, \"bandwidth\": %.1f, \"spreading_factor\": %u, \"coding_rate\": %u, \"tx_power\": %d},\n",
      (double)p->freq, (double)p->bw, (unsigned)p->sf, (unsigned)p->cr, (int)p->tx_power_dbm);
    out.print(l);
  }
  {
    char l[96];
    snprintf(l, sizeof l,
      "  \"position_settings\": {\"latitude\": %.6f, \"longitude\": %.6f},\n",
      node_lat, node_lon);
    out.print(l);
  }
  // Match the stock app's shape exactly (it always emits these two as null).
  out.print("  \"other_settings\": null,\n");
  out.print("  \"auto_add_settings\": null,\n");
  out.print("  \"channels\": [");
  bool first = true;
#ifdef MAX_GROUP_CHANNELS
  for (int i = 0; i < MAX_GROUP_CHANNELS; ++i) {
    ChannelDetails cd{};
    if (!getChannel(i, cd) || cd.name[0] == '\0') continue;
    out.print(first ? "\n    {\"name\": \"" : ",\n    {\"name\": \"");
    first = false;
    esc(cd.name);
    out.print("\", \"secret\": \""); hex(cd.channel.secret, 16); out.print("\"}");
  }
#endif
  out.print(first ? "],\n" : "\n  ],\n");
  out.print("  \"contacts\": [");
  first = true;
  uint32_t nc = getNumContacts();
  for (uint32_t i = 0; i < nc; ++i) {
    ContactInfo c;
    if (!getContactByIdx(i, c)) continue;
    char l[224];
    out.print(first ? "\n    {\"type\": " : ",\n    {\"type\": ");
    first = false;
    snprintf(l, sizeof l, "%u, \"name\": \"", (unsigned)c.type); out.print(l);
    esc(c.name);
    out.print("\", \"custom_name\": null, \"public_key\": \""); hex(c.id.pub_key, 32);
    snprintf(l, sizeof l,
      "\", \"flags\": %u, \"latitude\": \"%.6f\", \"longitude\": \"%.6f\", \"last_advert\": %lu, \"last_modified\": %lu, \"out_path\": null}",
      (unsigned)c.flags, (double)c.gps_lat / 1e6, (double)c.gps_lon / 1e6,
      (unsigned long)c.last_advert_timestamp, (unsigned long)c.lastmod);
    out.print(l);
  }
  out.print(first ? "]\n}\n" : "\n  ]\n}\n");
}

static uint8_t mc_hexNib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}
static int mc_hexToBytes(const char* hex, uint8_t* out, int max_bytes) {
  int n = 0;
  while (hex && hex[0] && hex[1] && n < max_bytes) { out[n++] = (mc_hexNib(hex[0]) << 4) | mc_hexNib(hex[1]); hex += 2; }
  return n;
}

bool MyMesh::uiImportBackup(Stream& in, uint8_t sections,
                            bool replace_channels, bool replace_contacts,
                            int* out_channels, int* out_contacts) {
  (void)replace_channels;
  if (out_channels) *out_channels = 0;
  if (out_contacts) *out_contacts = 0;
  // PSRAM-backed doc so a big backup (60 KB+) doesn't exhaust internal RAM.
  struct PsAlloc : ArduinoJson::Allocator {
    void* allocate(size_t n) override { void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM); return p ? p : malloc(n); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); return q ? q : realloc(p, n); }
  } alloc;
  JsonDocument doc(&alloc);
  if (deserializeJson(doc, in)) return false;
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) return false;

  bool prefs_dirty = false;
  if (sections & 0x01) {  // identity: name + private key
    const char* nm = root["name"].as<const char*>();
    if (nm && nm[0]) { strncpy(_prefs.node_name, nm, sizeof(_prefs.node_name) - 1); _prefs.node_name[sizeof(_prefs.node_name) - 1] = 0; prefs_dirty = true; }
    const char* pk = root["private_key"].as<const char*>();
    if (pk && strlen(pk) >= 128) { uint8_t prv[64]; mc_hexToBytes(pk, prv, 64); uiImportPrivKey(prv); }
  }
  if (sections & 0x02) {  // radio
    JsonObjectConst r = root["radio_settings"].as<JsonObjectConst>();
    if (!r.isNull()) {
      if (!r["frequency"].isNull())        { _prefs.freq = r["frequency"].as<float>(); prefs_dirty = true; }
      if (!r["bandwidth"].isNull())        { _prefs.bw = r["bandwidth"].as<float>(); prefs_dirty = true; }
      if (!r["spreading_factor"].isNull()) { _prefs.sf = (uint8_t)r["spreading_factor"].as<int>(); prefs_dirty = true; }
      if (!r["coding_rate"].isNull())      { _prefs.cr = (uint8_t)r["coding_rate"].as<int>(); prefs_dirty = true; }
      if (!r["tx_power"].isNull())         { _prefs.tx_power_dbm = (int8_t)r["tx_power"].as<int>(); prefs_dirty = true; }
    }
  }
  if (sections & 0x04) {  // position
    JsonObjectConst ps = root["position_settings"].as<JsonObjectConst>();
    if (!ps.isNull()) {
      if (!ps["latitude"].isNull())  { sensors.node_lat = ps["latitude"].as<double>(); prefs_dirty = true; }
      if (!ps["longitude"].isNull()) { sensors.node_lon = ps["longitude"].as<double>(); prefs_dirty = true; }
    }
  }
  if (prefs_dirty) savePrefs();

  int nch = 0;
  if (sections & 0x08) {  // channels (overwrite from slot 0)
    JsonArrayConst ch = root["channels"].as<JsonArrayConst>();
    if (!ch.isNull()) {
      int idx = 0;
      for (JsonVariantConst ev : ch) {
        JsonObjectConst e = ev.as<JsonObjectConst>();
        const char* nm = e["name"].as<const char*>();
        const char* sec = e["secret"].as<const char*>();
        if (!nm || !sec || strlen(sec) < 32) { idx++; continue; }
#ifdef MAX_GROUP_CHANNELS
        if (idx >= MAX_GROUP_CHANNELS) break;
#endif
        ChannelDetails cd{};
        strncpy(cd.name, nm, sizeof(cd.name) - 1);
        mc_hexToBytes(sec, cd.channel.secret, 16);
        if (setChannel((uint8_t)idx, cd)) nch++;
        idx++;
      }
      saveChannels();
    }
  }
  int nco = 0;
  if (sections & 0x10) {  // contacts
    JsonArrayConst co = root["contacts"].as<JsonArrayConst>();
    if (!co.isNull()) {
      if (replace_contacts) resetContacts();
      for (JsonVariantConst ev : co) {
        JsonObjectConst e = ev.as<JsonObjectConst>();
        const char* pk = e["public_key"].as<const char*>();
        if (!pk || strlen(pk) < 64) continue;
        uint8_t pub[32]; mc_hexToBytes(pk, pub, 32);
        const char* cn = e["custom_name"].as<const char*>();
        const char* nm = (cn && cn[0]) ? cn : e["name"].as<const char*>();
        double lat = 0, lon = 0;
        const char* la = e["latitude"].as<const char*>(); if (la) lat = atof(la);
        const char* lo = e["longitude"].as<const char*>(); if (lo) lon = atof(lo);
        if (uiAddContactFromBackup(pub, nm, e["type"].as<uint8_t>(), e["flags"].as<uint8_t>(),
                                   (int32_t)(lat * 1e6), (int32_t)(lon * 1e6),
                                   e["last_advert"].as<uint32_t>(), e["last_modified"].as<uint32_t>())) nco++;
      }
      saveContacts();
    }
  }
  if (out_channels) *out_channels = nch;
  if (out_contacts) *out_contacts = nco;
  return true;
}

bool MyMesh::isAutoAddEnabled() const {
  return (_prefs.manual_add_contacts & 1) == 0;
}

bool MyMesh::shouldAutoAddContactType(uint8_t contact_type) const {
  // Manual-add OFF: auto-add every type.
  if ((_prefs.manual_add_contacts & 1) == 0) {
    return true;
  }

  // Manual-add ON: honor each type's autoadd_config bit. Chat/person peers are
  // included now — they used to be force-added here unconditionally, which made
  // the UI's "auto-add chats" toggle a no-op and surprised users who'd turned
  // auto-add off (person adverts still landed straight in Contacts). With the
  // chat bit off they now go to the Discovered/"Found" list to be added by hand.
  //
  // Trade-off: decoding a sender's DM needs their pub key, which only arrives in
  // their advert — so a brand-new sender's *first* DM can't be decoded until
  // they're added. Add them from Found first, or turn the "auto-add chats"
  // toggle back on if you want strangers' messages to land automatically.
  // (A future enhancement can auto-add a sender the moment a DM from them
  // actually decodes, keeping Found clean while not dropping cold DMs.)
  uint8_t type_bit = 0;
  switch (contact_type) {
    case ADV_TYPE_CHAT:
      type_bit = AUTO_ADD_CHAT;
      break;
    case ADV_TYPE_REPEATER:
      type_bit = AUTO_ADD_REPEATER;
      break;
    case ADV_TYPE_ROOM:
      type_bit = AUTO_ADD_ROOM_SERVER;
      break;
    case ADV_TYPE_SENSOR:
      type_bit = AUTO_ADD_SENSOR;
      break;
    default:
      return false;  // Unknown type, don't auto-add
  }

  return (_prefs.autoadd_config & type_bit) != 0;
}

bool MyMesh::shouldOverwriteWhenFull() const {
  return (_prefs.autoadd_config & AUTO_ADD_OVERWRITE_OLDEST) != 0;
}

void MyMesh::onContactOverwrite(const uint8_t* pub_key) {
    _store->deleteBlobByKey(pub_key, PUB_KEY_SIZE); // delete from storage
  if (_serial->isConnected()) {
    out_frame[0] = PUSH_CODE_CONTACT_DELETED;
    memcpy(&out_frame[1], pub_key, PUB_KEY_SIZE);
    _serial->writeFrameToAll(out_frame, 1 + PUB_KEY_SIZE);
  }
}

void MyMesh::onContactsFull() {
  if (_serial->isConnected()) {
    out_frame[0] = PUSH_CODE_CONTACTS_FULL;
    _serial->writeFrameToAll(out_frame, 1);
  }
}

void MyMesh::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  if (_serial->isConnected()) {
    if (is_new) {
      writeContactRespFrame(PUSH_CODE_NEW_ADVERT, contact, true);
    } else {
      out_frame[0] = PUSH_CODE_ADVERT;
      memcpy(&out_frame[1], contact.id.pub_key, PUB_KEY_SIZE);
      _serial->writeFrameToAll(out_frame, 1 + PUB_KEY_SIZE);
    }
  } else {
#ifdef DISPLAY_CLASS
    if (_ui) _ui->notify(UIEventType::newContactMessage);
#endif
  }
#ifdef DISPLAY_CLASS
  // Mirror to the touch UI's Discovered store so the user can browse pending
  // adverts and manually add nodes to contacts[] (used when auto-add is off
  // or contacts[] is full). `is_new=true` here means the contact is NOT yet
  // in contacts[]; `is_new=false` means it's a refresh of an existing one.
  if (_ui) _ui->discoveredContact(contact, is_new, path_len);
#endif

  // add inbound-path to mem cache
  if (path && path_len <= sizeof(AdvertPath::path)) {  // check path is valid
    AdvertPath* p = advert_paths;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < ADVERT_PATH_TABLE_SIZE; i++) {   // check if already in table, otherwise evict oldest
      if (memcmp(advert_paths[i].pubkey_prefix, contact.id.pub_key, sizeof(AdvertPath::pubkey_prefix)) == 0) {
        p = &advert_paths[i];   // found
        break;
      }
      if (advert_paths[i].recv_timestamp < oldest) {
        oldest = advert_paths[i].recv_timestamp;
        p = &advert_paths[i];
      }
    }

    memcpy(p->pubkey_prefix, contact.id.pub_key, sizeof(p->pubkey_prefix));
    // contact.name is from an over-the-air advert and may fill its 32-byte field
    // with no NUL terminator; an unbounded strcpy would then run past p->name and
    // corrupt the advert_paths table (garbled "Found"/recently-heard entries).
    strncpy(p->name, contact.name, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';
    p->recv_timestamp = getRTCClock()->getCurrentTime();
    p->path_len = path_len;
    memcpy(p->path, path, p->path_len);
  }

  if (!is_new) dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY); // only schedule lazy write for contacts that are in contacts[]
}

static int sort_by_recent(const void *a, const void *b) {
  return ((AdvertPath *) b)->recv_timestamp - ((AdvertPath *) a)->recv_timestamp;
}

int MyMesh::getRecentlyHeard(AdvertPath dest[], int max_num) {
  if (max_num > ADVERT_PATH_TABLE_SIZE) max_num = ADVERT_PATH_TABLE_SIZE;
  qsort(advert_paths, ADVERT_PATH_TABLE_SIZE, sizeof(advert_paths[0]), sort_by_recent);

  for (int i = 0; i < max_num; i++) {
    dest[i] = advert_paths[i];
  }
  return max_num;
}

void MyMesh::onContactPathUpdated(const ContactInfo &contact) {
  out_frame[0] = PUSH_CODE_PATH_UPDATED;
  memcpy(&out_frame[1], contact.id.pub_key, PUB_KEY_SIZE);
  _serial->writeFrameToAll(out_frame, 1 + PUB_KEY_SIZE); // NOTE: app may not be connected

  dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
}

ContactInfo*  MyMesh::processAck(const uint8_t *data) {
#if defined(DISPLAY_CLASS)
  // Diag: log every processAck call so we can see whether the ACK matching
  // pipeline gets reached after the radio surface dispatches an ACK frame.
  if (_ui) {
    uint32_t in_ack = 0;
    memcpy(&in_ack, data, 4);
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "procACK %08lx", (unsigned long)in_ack);
    _ui->appendDiag(dbg);
  }
#endif
  // see if matches any in a table
  for (int i = 0; i < EXPECTED_ACK_TABLE_SIZE; i++) {
    if (memcmp(data, &expected_ack_table[i].ack, 4) == 0) { // got an ACK from recipient
      out_frame[0] = PUSH_CODE_SEND_CONFIRMED;
      memcpy(&out_frame[1], data, 4);
      uint32_t trip_time = _ms->getMillis() - expected_ack_table[i].msg_sent;
      memcpy(&out_frame[5], &trip_time, 4);
      _serial->writeFrameToAll(out_frame, 9);

#ifdef DISPLAY_CLASS
      // Tell the touch UI so the outgoing bubble flips to DELIVERED.
      if (_ui) {
        uint32_t ack4 = 0;
        memcpy(&ack4, data, 4);
        _ui->onMessageAcked(ack4);
      }
#endif

      // NOTE: the same ACK can be received multiple times!
      expected_ack_table[i].ack = 0; // clear expected hash, now that we have received ACK
      return expected_ack_table[i].contact;
    }
  }
  return checkConnectionsAck(data);
}

void MyMesh::queueMessage(const ContactInfo &from, uint8_t txt_type, mesh::Packet *pkt,
                          uint32_t sender_timestamp, const uint8_t *extra, int extra_len, const char *text) {
  int i = 0;
  out_frame[i++] = RESP_CODE_CONTACT_MSG_RECV_V3;
  out_frame[i++] = (int8_t)(pkt->getSNR() * 4);
  out_frame[i++] = 0; // reserved1
  out_frame[i++] = 0; // reserved2
  memcpy(&out_frame[i], from.id.pub_key, 6);
  i += 6; // just 6-byte prefix
  uint8_t path_len = out_frame[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  out_frame[i++] = txt_type;
  memcpy(&out_frame[i], &sender_timestamp, 4);
  i += 4;
  if (extra_len > 0) {
    memcpy(&out_frame[i], extra, extra_len);
    i += extra_len;
  }
  int tlen = strlen(text); // TODO: UTF-8 ??
  if (i + tlen > MAX_FRAME_SIZE) {
    tlen = MAX_FRAME_SIZE - i;
  }
  memcpy(&out_frame[i], text, tlen);
  i += tlen;
  uint32_t hist_seq = addToHistoryRing(out_frame, i);

  if (_serial->isConnected()) {
    if (_serial->writeFrameToAll(out_frame, i) == (size_t)i && hist_seq != 0 &&
        _serial->companionUnsolicitedPushesBroadcastToAll()) {
      advanceHistoryClientsAfterV3Broadcast(hist_seq);
    }
    uint8_t frame[1];
    frame[0] = PUSH_CODE_MSG_WAITING; // send push 'tickle'
    _serial->writeFrameToAll(frame, 1);
  }

#ifdef DISPLAY_CLASS
  // we only want to show text messages on display, not cli data
  bool should_display = txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_SIGNED_PLAIN;
  if (should_display && _ui) {
    /* notify BEFORE newMsgFromPub: UITask::newMsg keys on g_last_event to
     * decide channel-thread vs DM-thread. Previously this fired only when
     * serial was disconnected, which meant after a channel message arrived
     * over TCP/BLE g_last_event stayed at `channelMessage` and the next DM
     * was routed into the channel thread (or vice versa). */
    _ui->notify(UIEventType::contactMessage);
    // Pass RX metadata so the touch UI can surface it via the bubble's
    // long-press Info sheet. SNR comes off the packet itself (most accurate
    // per-message); RSSI is the radio's last-RSSI, which is current since
    // the packet handler runs inline with reception.
    const int8_t snr_q4 = (int8_t)(pkt->getSNR() * 4);
    const int8_t rssi   = (int8_t)(_radio->getLastRSSI());
    const bool   is_flood = pkt->isRouteFlood();
    uiStashRxMeta(pkt);   // capture route + scope for the per-message Info popup
    _ui->newMsgFromPubWithMeta(path_len, is_flood, from.id.pub_key, from.name,
                               text, history_count, snr_q4, rssi);
  }
  // CLI command replies don't belong in the chat thread but the touch UI
  // *does* want them — they're the response to whatever was typed into the
  // admin console. Surfaces via a dedicated hook so the console can append
  // the line without polluting the chat history.
  if (txt_type == TXT_TYPE_CLI_DATA && _ui) {
    _ui->onAdminCommandReply(from, text);
  }
#endif
}

bool MyMesh::filterRecvFloodPacket(mesh::Packet* packet) {
  // REVISIT: try to determine which Region (from transport_codes[1]) that Sender is indicating for replies/responses
  //    if unknown, fallback to finding Region from transport_codes[0], the 'scope' used by Sender
  return false;
}

bool MyMesh::allowPacketForward(const mesh::Packet* packet) {
  return _prefs.client_repeat != 0;
}

// Fingerprint + track an originated flood, but only for chat messages — DM text
// and group/channel text (the things the UI shows "repeats heard" for);
// adverts/acks/traces are ignored. fnv1a32/isMsgFloodType are defined above.
static inline uint32_t txtFloodFp(mesh::Packet* pkt) {
  if (!pkt || !isMsgFloodType(pkt->getPayloadType())) return 0;
  return fnv1a32(pkt->payload, pkt->payload_len);
}

void MyMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis) {
  uiTrackSentFp(txtFloodFp(pkt));
  uint8_t phs = (uint8_t)(_prefs.path_hash_mode + 1);
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, phs);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;  // REVISIT: set to 'home' Region, for sender/return region?
    sendFlood(pkt, codes, delay_millis, phs);
  }
}

void MyMesh::sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis) {
  uiTrackSentFp(txtFloodFp(pkt));
  // UNICAST floods (login, DM when out_path is unknown, status/telemetry reqs,
  // acks, path-returns) must be able to reach their specific target regardless of
  // our 'home' Region, so they are NOT tagged with the default region scope —
  // doing so region-locked logins/DMs and broke cross-region repeater/room login
  // (the old "TODO: dynamic send_scope depending on recipient/Region"). Only an
  // EXPLICIT per-send override (send_scope, set by the app's CMD_SET_FLOOD_SCOPE)
  // is honoured here; otherwise unscoped. Channel/group floods keep default_scope
  // — see the GroupChannel overload below — so public-channel containment is intact.
  if (send_unscoped || send_scope.isNull()) {
    sendFlood(pkt, delay_millis, _prefs.path_hash_mode + 1);
  } else {
    sendFloodScoped(send_scope, pkt, delay_millis);   // explicit override only
  }
}
void MyMesh::sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis) {
  uiTrackSentFp(txtFloodFp(pkt));
  // TODO: have per-channel send_scope
  if (send_unscoped) {
    sendFlood(pkt, delay_millis, _prefs.path_hash_mode + 1);  // app has explicitly requested un-scoped
  } else {
    TransportKey default_scope;
    memcpy(&default_scope.key, _prefs.default_scope_key, sizeof(default_scope.key));

    auto scope = send_scope.isNull() ? &default_scope : &send_scope;
    sendFloodScoped(*scope, pkt, delay_millis);   // the lower overload applies path_hash_mode
  }
}

void MyMesh::onMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                           const char *text) {
  markConnectionActive(from); // in case this is from a server, and we have a connection
  queueMessage(from, TXT_TYPE_PLAIN, pkt, sender_timestamp, NULL, 0, text);
}

void MyMesh::onCommandDataRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                               const char *text) {
  markConnectionActive(from); // in case this is from a server, and we have a connection
  queueMessage(from, TXT_TYPE_CLI_DATA, pkt, sender_timestamp, NULL, 0, text);
}

void MyMesh::onSignedMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                                 const uint8_t *sender_prefix, const char *text) {
  markConnectionActive(from);
  // from.sync_since change needs to be persisted
  dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
  queueMessage(from, TXT_TYPE_SIGNED_PLAIN, pkt, sender_timestamp, sender_prefix, 4, text);
}

void MyMesh::onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint32_t timestamp,
                                  const char *text) {
  int i = 0;
  out_frame[i++] = RESP_CODE_CHANNEL_MSG_RECV_V3;
  out_frame[i++] = (int8_t)(pkt->getSNR() * 4);
  out_frame[i++] = 0; // reserved1
  out_frame[i++] = 0; // reserved2

  int channel_idx_i = findChannelIdx(channel);
  if (channel_idx_i < 0 || channel_idx_i >= MAX_GROUP_CHANNELS) {
    // Keep on-wire channel index stable for clients that assume 0..MAX_GROUP_CHANNELS-1.
    channel_idx_i = 0;
  }
  uint8_t channel_idx = (uint8_t)channel_idx_i;
  out_frame[i++] = channel_idx;
  uint8_t path_len = out_frame[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;

  out_frame[i++] = TXT_TYPE_PLAIN;
  memcpy(&out_frame[i], &timestamp, 4);
  i += 4;
  int tlen = strlen(text); // TODO: UTF-8 ??
  if (i + tlen > MAX_FRAME_SIZE) {
    tlen = MAX_FRAME_SIZE - i;
  }
  memcpy(&out_frame[i], text, tlen);
  i += tlen;
  uint32_t hist_seq = addToHistoryRing(out_frame, i);
  if (_serial->isConnected()) {
    if (_serial->writeFrameToAll(out_frame, i) == (size_t)i && hist_seq != 0 &&
        _serial->companionUnsolicitedPushesBroadcastToAll()) {
      advanceHistoryClientsAfterV3Broadcast(hist_seq);
    }
    uint8_t frame[1];
    frame[0] = PUSH_CODE_MSG_WAITING; // send push 'tickle'
    _serial->writeFrameToAll(frame, 1);
  }
#ifdef DISPLAY_CLASS
  // Get the channel name from the channel index
  const char *channel_name = "Unknown";
  ChannelDetails channel_details;
  if (getChannel(channel_idx, channel_details)) {
    channel_name = channel_details.name;
  }
  /* notify BEFORE newMsgFromPub: UITask::newMsg keys on the last UIEventType
   * to decide whether the message lands in a channel thread or a DM thread.
   * Used to only fire when the serial client was disconnected, which meant
   * channel messages got appended as DMs whenever TCP/BLE was up. */
  if (_ui) _ui->notify(UIEventType::channelMessage);
  if (_ui) {
    const int8_t snr_q4 = (int8_t)(pkt->getSNR() * 4);
    const int8_t rssi   = (int8_t)(_radio->getLastRSSI());
    const bool   is_flood = pkt->isRouteFlood();
    uiStashRxMeta(pkt);   // capture route + scope for the per-message Info popup
    _ui->newMsgFromPubWithMeta(path_len, is_flood, nullptr, channel_name,
                               text, history_count, snr_q4, rssi);
  }
#endif
}

uint8_t MyMesh::onContactRequest(const ContactInfo &contact, uint32_t sender_timestamp, const uint8_t *data,
                                 uint8_t len, uint8_t *reply) {
  if (data[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t permissions = 0;
    uint8_t cp = contact.flags >> 1; // LSB used as 'favourite' bit (so only use upper bits)

    if (_prefs.telemetry_mode_base == TELEM_MODE_ALLOW_ALL) {
      permissions = TELEM_PERM_BASE;
    } else if (_prefs.telemetry_mode_base == TELEM_MODE_ALLOW_FLAGS) {
      permissions = cp & TELEM_PERM_BASE;
    }

    if (_prefs.telemetry_mode_loc == TELEM_MODE_ALLOW_ALL) {
      permissions |= TELEM_PERM_LOCATION;
    } else if (_prefs.telemetry_mode_loc == TELEM_MODE_ALLOW_FLAGS) {
      permissions |= cp & TELEM_PERM_LOCATION;
    }

    if (_prefs.telemetry_mode_env == TELEM_MODE_ALLOW_ALL) {
      permissions |= TELEM_PERM_ENVIRONMENT;
    } else if (_prefs.telemetry_mode_env == TELEM_MODE_ALLOW_FLAGS) {
      permissions |= cp & TELEM_PERM_ENVIRONMENT;
    }

    uint8_t perm_mask = ~(data[1]);    // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions
    permissions &= perm_mask;

    if (permissions & TELEM_PERM_BASE) { // only respond if base permission bit is set
      telemetry.reset();
      telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
      // query other sensors -- target specific
      sensors.querySensors(permissions, telemetry);

      memcpy(reply, &sender_timestamp,
             4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

      uint8_t tlen = telemetry.getSize();
      memcpy(&reply[4], telemetry.getBuffer(), tlen);
      return 4 + tlen;
    }
  }
  return 0; // unknown
}

void MyMesh::onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t len) {
  uint32_t tag;
  memcpy(&tag, data, 4);

  /* Touch-UI ping match: when the touch UI fired a status request via
   * sendStatusPingForUI, deliver the reply payload to it. Done before the
   * companion-serial pending_status branch so a UI-only ping doesn't write
   * a STATUS_RESPONSE frame to a serial client that didn't ask for it. */
#ifdef DISPLAY_CLASS
  // Match by *tag* (the 4-byte timestamp the responder echoes from our
  // request), not just by pubkey. The chained guest-login response uses
  // the repeater's own clock at data[0..3], so without a tag check it
  // would race ahead of the real STATUS/TELEMETRY reply and get
  // misrouted as if it were the answer. Tag is set when we send the REQ.
  if (_ui && _ui_pending_status && _ui_pending_tag != 0 && len > 4 &&
      memcmp(&_ui_pending_status, contact.id.pub_key, 4) == 0 &&
      tag == _ui_pending_tag) {
    UiReqKind kind = _ui_pending_kind;
    _ui_pending_status = 0;
    _ui_pending_kind = UiReqKind::None;
    _ui_pending_tag = 0;
    if (kind == UiReqKind::Telemetry) {
      // CayenneLPP payload — let the UI decode the LPP channels.
      _ui->onTelemetryReply(contact, &data[4], (size_t)(len - 4));
    } else {
      // STATUS or unknown — let the UI fall through its RepeaterStats /
      // JSON parser path.
      _ui->onPingReply(contact, &data[4], (size_t)(len - 4));
    }
    return;
  }
#endif

  if (pending_login && memcmp(&pending_login, contact.id.pub_key, 4) == 0) { // check for login response
    // yes, is response to pending sendLogin()
    pending_login = 0;

    int i = 0;
    bool ok = false;
    uint8_t perms = 0;
    if (memcmp(&data[4], "OK", 2) == 0) { // legacy Repeater login OK response
      out_frame[i++] = PUSH_CODE_LOGIN_SUCCESS;
      out_frame[i++] = 0; // legacy: is_admin = false
      memcpy(&out_frame[i], contact.id.pub_key, 6);
      i += 6;                                     // pub_key_prefix
      ok = true;
    } else if (data[4] == RESP_SERVER_LOGIN_OK) { // new login response
      uint16_t keep_alive_secs = ((uint16_t)data[5]) * 16;
      if (keep_alive_secs > 0) {
        startConnection(contact, keep_alive_secs);
      }
      out_frame[i++] = PUSH_CODE_LOGIN_SUCCESS;
      out_frame[i++] = data[6]; // permissions (eg. is_admin)
      memcpy(&out_frame[i], contact.id.pub_key, 6);
      i += 6; // pub_key_prefix
      memcpy(&out_frame[i], &tag, 4);
      i += 4; // NEW: include server timestamp
      out_frame[i++] = data[7]; // NEW (v7): ACL permissions
      out_frame[i++] = data[12]; // FIRMWARE_VER_LEVEL
      ok = true;
      perms = data[6];
    } else {
      out_frame[i++] = PUSH_CODE_LOGIN_FAIL;
      out_frame[i++] = 0; // reserved
      memcpy(&out_frame[i], contact.id.pub_key, 6);
      i += 6; // pub_key_prefix
    }
    _serial->writeFrame(out_frame, i);
    // Diagnostic (room-server login trace): the login response we matched +
    // whether we accepted it. resp byte: 0x4F 'O' = legacy "OK"; otherwise a
    // RESP_SERVER_LOGIN_OK / fail code. Pair with "[ROOM] login send".
    Serial.printf("[ROOM] login resp '%s' resp=0x%02x ok=%d perms=%d\n",
                  contact.name, data[4], (int)ok, (int)perms);
#ifdef DISPLAY_CLASS
    // Notify the touch UI so the admin console can flip from "logging in…"
    // to the prompt (success) or show "wrong password" (fail). Same data
    // the companion app sees via PUSH_CODE_LOGIN_SUCCESS/_FAIL.
    if (_ui) _ui->onAdminLoginResult(contact, ok, perms);
#endif
  } else if (len > 4 && // check for status response
             pending_status &&
             memcmp(&pending_status, contact.id.pub_key, 4) == 0 // legacy matching scheme
                                                                 // FUTURE: tag == pending_status
  ) {
    pending_status = 0;

    int i = 0;
    out_frame[i++] = PUSH_CODE_STATUS_RESPONSE;
    out_frame[i++] = 0; // reserved
    memcpy(&out_frame[i], contact.id.pub_key, 6);
    i += 6; // pub_key_prefix
    memcpy(&out_frame[i], &data[4], len - 4);
    i += (len - 4);
    _serial->writeFrame(out_frame, i);
  } else if (len > 4 && tag == pending_telemetry) {  // check for matching response tag
    pending_telemetry = 0;

    int i = 0;
    out_frame[i++] = PUSH_CODE_TELEMETRY_RESPONSE;
    out_frame[i++] = 0; // reserved
    memcpy(&out_frame[i], contact.id.pub_key, 6);
    i += 6; // pub_key_prefix
    memcpy(&out_frame[i], &data[4], len - 4);
    i += (len - 4);
    _serial->writeFrame(out_frame, i);
  } else if (len > 4 && tag == pending_req) {  // check for matching response tag
    pending_req = 0;

    int i = 0;
    out_frame[i++] = PUSH_CODE_BINARY_RESPONSE;
    out_frame[i++] = 0; // reserved
    memcpy(&out_frame[i], &tag, 4);   // app needs to match this to RESP_CODE_SENT.tag
    i += 4;
    memcpy(&out_frame[i], &data[4], len - 4);
    i += (len - 4);
    _serial->writeFrame(out_frame, i);
  }
}

bool MyMesh::onContactPathRecv(ContactInfo& contact, uint8_t* in_path, uint8_t in_path_len, uint8_t* out_path, uint8_t out_path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) {
  if (extra_type == PAYLOAD_TYPE_RESPONSE && extra_len > 4) {
    uint32_t tag;
    memcpy(&tag, extra, 4);

    if (tag == pending_discovery) {  // check for matching response tag)
      pending_discovery = 0;

      if (in_path_len > MAX_PATH_SIZE || out_path_len > MAX_PATH_SIZE) {
        MESH_DEBUG_PRINTLN("onContactPathRecv, invalid path sizes: %d, %d", in_path_len, out_path_len);
      } else {
        int i = 0;
        out_frame[i++] = PUSH_CODE_PATH_DISCOVERY_RESPONSE;
        out_frame[i++] = 0; // reserved
        memcpy(&out_frame[i], contact.id.pub_key, 6);
        i += 6; // pub_key_prefix
        out_frame[i++] = out_path_len;
        memcpy(&out_frame[i], out_path, out_path_len);
        i += out_path_len;
        out_frame[i++] = in_path_len;
        memcpy(&out_frame[i], in_path, in_path_len);
        i += in_path_len;
        // NOTE: telemetry data in 'extra' is discarded at present

        _serial->writeFrame(out_frame, i);
      }
      return false;  // DON'T send reciprocal path!
    }
  }
  // let base class handle received path and data
  return BaseChatMesh::onContactPathRecv(contact, in_path, in_path_len, out_path, out_path_len, extra_type, extra, extra_len);
}

void MyMesh::onControlDataRecv(mesh::Packet *packet) {
  if (packet->payload_len + 4 > sizeof(out_frame)) {
    MESH_DEBUG_PRINTLN("onControlDataRecv(), payload_len too long: %d", packet->payload_len);
    return;
  }
  int i = 0;
  out_frame[i++] = PUSH_CODE_CONTROL_DATA;
  out_frame[i++] = (int8_t)(_radio->getLastSNR() * 4);
  out_frame[i++] = (int8_t)(_radio->getLastRSSI());
  out_frame[i++] = packet->path_len;
  memcpy(&out_frame[i], packet->payload, packet->payload_len);
  i += packet->payload_len;

  if (_serial->isConnected()) {
    _serial->writeFrame(out_frame, i);
  } else {
    MESH_DEBUG_PRINTLN("onControlDataRecv(), data received while app offline");
  }
}

void MyMesh::onRawDataRecv(mesh::Packet *packet) {
  if (packet->payload_len + 4 > sizeof(out_frame)) {
    MESH_DEBUG_PRINTLN("onRawDataRecv(), payload_len too long: %d", packet->payload_len);
    return;
  }
  int i = 0;
  out_frame[i++] = PUSH_CODE_RAW_DATA;
  out_frame[i++] = (int8_t)(_radio->getLastSNR() * 4);
  out_frame[i++] = (int8_t)(_radio->getLastRSSI());
  out_frame[i++] = 0xFF; // reserved (possibly path_len in future)
  memcpy(&out_frame[i], packet->payload, packet->payload_len);
  i += packet->payload_len;

  if (_serial->isConnected()) {
    _serial->writeFrame(out_frame, i);
  } else {
    MESH_DEBUG_PRINTLN("onRawDataRecv(), data received while app offline");
  }
}

void MyMesh::onTraceRecv(mesh::Packet *packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                         const uint8_t *path_snrs, const uint8_t *path_hashes, uint8_t path_len) {
  uint8_t path_sz = flags & 0x03;  // NEW v1.11+
#if defined(DISPLAY_CLASS)
  // If this trace's tag matches the most recent UI-initiated ping, surface
  // the bidirectional SNRs directly to the touch UI instead of (or in
  // addition to) sending the companion-protocol push frame. path_len is
  // the *byte* length of path_hashes; the number of SNR readings is
  // (path_len >> path_sz). For a 0-hop ping the path has a single entry
  // (the neighbor's hash) and we get a single SNR: their RX of us. Our RX
  // of their retransmission comes from packet->getSNR().
  if (_ui && tag != 0 && tag == _ui_trace_ping_tag) {
    _ui_trace_ping_tag = 0;
    const uint8_t snr_count = (path_sz >= 4) ? 0 : (uint8_t)(path_len >> path_sz);
    int8_t their_snr = (snr_count > 0) ? (int8_t)path_snrs[0] : (int8_t)0;
    int8_t our_snr = (int8_t)(packet->getSNR() * 4);
    const int8_t* extra = (snr_count > 1) ? (const int8_t*)&path_snrs[1] : nullptr;
    uint8_t extra_hops = (snr_count > 1) ? (uint8_t)(snr_count - 1) : 0;
    _ui->onTracePingResult(tag, their_snr, our_snr, extra_hops, extra);
    // Don't also push to companion: a UI ping shouldn't leak as if the
    // companion app asked for it. (Companion-initiated traces use a tag
    // that won't collide because we generate _ui_trace_ping_tag with the
    // RNG and clear it after one match.)
    return;
  }
#endif
  if (12 + path_len + (path_len >> path_sz) + 1 > sizeof(out_frame)) {
    MESH_DEBUG_PRINTLN("onTraceRecv(), path_len is too long: %d", (uint32_t)path_len);
    return;
  }
  int i = 0;
  out_frame[i++] = PUSH_CODE_TRACE_DATA;
  out_frame[i++] = 0; // reserved
  out_frame[i++] = path_len;
  out_frame[i++] = flags;
  memcpy(&out_frame[i], &tag, 4);
  i += 4;
  memcpy(&out_frame[i], &auth_code, 4);
  i += 4;
  memcpy(&out_frame[i], path_hashes, path_len);
  i += path_len;

  memcpy(&out_frame[i], path_snrs, path_len >> path_sz);
  i += path_len >> path_sz;
  out_frame[i++] = (int8_t)(packet->getSNR() * 4); // extra/final SNR (to this node)

  if (_serial->isConnected()) {
    _serial->writeFrame(out_frame, i);
  } else {
    MESH_DEBUG_PRINTLN("onTraceRecv(), data received while app offline");
  }
}

uint32_t MyMesh::calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const {
  return SEND_TIMEOUT_BASE_MILLIS + (FLOOD_SEND_TIMEOUT_FACTOR * pkt_airtime_millis);
}
uint32_t MyMesh::calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const {
  return SEND_TIMEOUT_BASE_MILLIS +
         ((pkt_airtime_millis * DIRECT_SEND_PERHOP_FACTOR + DIRECT_SEND_PERHOP_EXTRA_MILLIS) *
          (path_len + 1));
}

void MyMesh::onSendTimeout() {}

MyMesh::MyMesh(mesh::Radio &radio, mesh::RNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables, DataStore& store, AbstractUITask* ui)
    : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(16), tables),
      _serial(NULL), telemetry(MAX_PACKET_PAYLOAD - 4), _store(&store), _ui(ui) {
  _iter_started = false;
  _contact_send_index = 0;
  _contact_list_reply_target = -1;
  _cli_rescue = false;
  offline_queue_len = 0;
  history_count = 0;
  history_head = 0;
  history_next_seq = 0;
  history_num_clients = 0;
  proto_num_clients = 0;
  app_target_ver = 0;
  clearPendingReqs();
  _ui_pending_status = 0;
  next_ack_idx = 0;
  sign_data = NULL;
  dirty_contacts_expiry = 0;
  memset(advert_paths, 0, sizeof(advert_paths));
  memset(send_scope.key, 0, sizeof(send_scope.key));
  send_unscoped = false;

  // defaults
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor = 1.0;
  strcpy(_prefs.node_name, "NONAME");
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.gps_enabled = 0;       // GPS disabled by default
  _prefs.gps_interval = 0;      // No automatic GPS updates by default
  //_prefs.rx_delay_base = 10.0f;  enable once new algo fixed
#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN ? 1 : 0;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default
#endif
#endif
}

void MyMesh::applyRadioFromPrefs() {
  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);
#if defined(USE_SX1262) || defined(USE_SX1268)
  _prefs.rx_boosted_gain = _prefs.rx_boosted_gain ? 1 : 0;
  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain != 0);
  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",
                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");
#endif
}

void MyMesh::setDefaultFloodScope(const char* region_name) {
  // Trim leading/trailing whitespace — a stray space changes the hash and would
  // derive the wrong scope key (the node would never match the region).
  char tag[40] = {0};
  if (region_name) {
    while (*region_name == ' ' || *region_name == '\t') region_name++;
    size_t n = strlen(region_name);
    while (n && (region_name[n-1] == ' '  || region_name[n-1] == '\t' ||
                 region_name[n-1] == '\n' || region_name[n-1] == '\r')) n--;
    // Normalise to a leading '#': a public hashtag region's key is SHA256("#name").
    size_t o = 0;
    if (n && region_name[0] != '#' && o < sizeof(tag)-1) tag[o++] = '#';
    for (size_t i = 0; i < n && o < sizeof(tag)-1; ++i) tag[o++] = region_name[i];
    tag[o] = '\0';
  }
  if (tag[0] == '\0' || (tag[0] == '#' && tag[1] == '\0')) {
    memset(_prefs.default_scope_key, 0, sizeof(_prefs.default_scope_key));   // unscoped
  } else {
    SHA256 sha;
    sha.update(tag, strlen(tag));
    sha.finalize(_prefs.default_scope_key, sizeof(_prefs.default_scope_key));
    send_unscoped = false;   // make sure the scope is actually applied on send
  }
  savePrefs();
}

bool MyMesh::pushChannelScope(const char* region_name) {
  // Mirror setDefaultFloodScope's trim + "#name" normalisation, but derive into
  // the transient send_scope so it applies only to the next channel send.
  char tag[40] = {0};
  if (region_name) {
    while (*region_name == ' ' || *region_name == '\t') region_name++;
    size_t n = strlen(region_name);
    while (n && (region_name[n-1] == ' '  || region_name[n-1] == '\t' ||
                 region_name[n-1] == '\n' || region_name[n-1] == '\r')) n--;
    size_t o = 0;
    if (n && region_name[0] != '#' && o < sizeof(tag)-1) tag[o++] = '#';
    for (size_t i = 0; i < n && o < sizeof(tag)-1; ++i) tag[o++] = region_name[i];
    tag[o] = '\0';
  }
  if (tag[0] == '\0' || (tag[0] == '#' && tag[1] == '\0')) return false;   // no override
  memcpy(&_chan_scope_saved, &send_scope, sizeof(send_scope));
  _chan_scope_saved_unscoped = send_unscoped;
  _chan_scope_pushed = true;
  SHA256 sha;
  sha.update(tag, strlen(tag));
  sha.finalize(send_scope.key, sizeof(send_scope.key));
  send_unscoped = false;
  return true;
}

void MyMesh::popChannelScope() {
  if (!_chan_scope_pushed) return;
  memcpy(&send_scope, &_chan_scope_saved, sizeof(send_scope));
  send_unscoped = _chan_scope_saved_unscoped;
  _chan_scope_pushed = false;
}

void MyMesh::begin(bool has_display) {
  BaseChatMesh::begin();

  if (!_store->loadMainIdentity(self_id)) {
    self_id = radio_new_identity(); // create new random identity
    int count = 0;
    while (count < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF)) { // reserved id hashes
      self_id = radio_new_identity();
      count++;
    }
    _store->saveMainIdentity(self_id);
  }

// if name is provided as a build flag, use that as default node name instead
#ifdef ADVERT_NAME
  strcpy(_prefs.node_name, ADVERT_NAME);
#else
  // use hex of first 4 bytes of identity public key as default node name
  char pub_key_hex[10];
  mesh::Utils::toHex(pub_key_hex, self_id.pub_key, 4);
  strcpy(_prefs.node_name, pub_key_hex);
#endif

  // load persisted prefs
  _store->loadPrefs(_prefs, sensors.node_lat, sensors.node_lon);

  // sanitise bad pref values
  _prefs.rx_delay_base = constrain(_prefs.rx_delay_base, 0, 20.0f);
  _prefs.airtime_factor = constrain(_prefs.airtime_factor, 0, 9.0f);
  _prefs.freq = constrain(_prefs.freq, 400.0f, 2500.0f);
  _prefs.bw = constrain(_prefs.bw, 7.8f, 500.0f);
  _prefs.sf = constrain(_prefs.sf, 5, 12);
  _prefs.cr = constrain(_prefs.cr, 5, 8);
  _prefs.tx_power_dbm = constrain(_prefs.tx_power_dbm, -9, MAX_LORA_TX_POWER);
  _prefs.gps_enabled = constrain(_prefs.gps_enabled, 0, 1);  // Ensure boolean 0 or 1
  _prefs.gps_interval = constrain(_prefs.gps_interval, 0, 86400);  // Max 24 hours
  _prefs.path_hash_mode = constrain(_prefs.path_hash_mode, 0, 2);
  if (_prefs.autoadd_max_hops > 64) {
    _prefs.autoadd_max_hops = 64;
  }

#ifdef BLE_PIN_CODE // 123456 by default
  if (_prefs.ble_pin == 0) {
#ifdef DISPLAY_CLASS
    if (has_display && BLE_PIN_CODE == 123456) {
      StdRNG rng;
      _active_ble_pin = rng.nextInt(100000, 999999); // random pin, generated ONCE
      // Persist it so it stays the SAME across reboots — it used to be re-rolled
      // every boot (ble_pin stayed 0), so a paired phone's saved PIN stopped
      // matching and pairing broke (user report: "BT pin resets sometimes").
      _prefs.ble_pin = _active_ble_pin;
      savePrefs();
    } else {
      _active_ble_pin = BLE_PIN_CODE; // otherwise static pin
    }
#else
    _active_ble_pin = BLE_PIN_CODE; // otherwise static pin
#endif
  } else {
    _active_ble_pin = _prefs.ble_pin;
  }
#else
  _active_ble_pin = 0;
#endif

  resetContacts();
  _store->loadContacts(this);
  bootstrapRTCfromContacts();
  addChannel("Public", PUBLIC_GROUP_PSK); // pre-configure Andy's public channel
  _store->loadChannels(this);

  applyRadioFromPrefs();   // freq/bw/sf/cr + TX power + RX-boost (shared with the live UI apply)
#if defined(DISPLAY_CLASS)
  // Boot diag: identity prefix + radio config so we can confirm the touch
  // firmware's pubkey is stable across flashes (replies are addressed to
  // the first byte of our pub_key, so any drift = no inbound) and that
  // freq/SF/BW match the stock firmware we're comparing against.
  if (_ui) {
    char dbg[80];
    snprintf(dbg, sizeof(dbg),
             "ID %02x%02x%02x%02x %.3fMHz sf%u bw%.1f",
             (unsigned)self_id.pub_key[0], (unsigned)self_id.pub_key[1],
             (unsigned)self_id.pub_key[2], (unsigned)self_id.pub_key[3],
             (double)_prefs.freq, (unsigned)_prefs.sf, (double)_prefs.bw);
    _ui->appendDiag(dbg);
  }
#endif

#if defined(ENABLE_ADVERT_ON_BOOT) && ENABLE_ADVERT_ON_BOOT == 1
  // Schedule a flood advert ~6s after boot so peers with auto-add ON learn
  // our current pubkey. Critical for touch firmware where SPIFFS may have
  // been wiped during a flash, leaving prior contacts with a stale pubkey
  // and silently dropping our DMs.
  _boot_advert_due_ms = _ms->getMillis() + 6000UL;
  _boot_advert_done   = false;
#endif
}

const char *MyMesh::getNodeName() {
  return _prefs.node_name;
}
NodePrefs *MyMesh::getNodePrefs() {
  return &_prefs;
}
uint32_t MyMesh::getBLEPin() {
  return _active_ble_pin;
}
// User-chosen pairing code from the touch BLE settings page. Validated to a
// 6-digit value, persisted to _prefs.ble_pin, and applied at the next boot
// (begin() seeds _active_ble_pin from it) — same contract as the companion
// CMD_SET_DEVICE_PIN. Returns false on an out-of-range PIN.
bool MyMesh::setBLEPin(uint32_t pin) {
  // Any 6-digit BLE passkey is valid, INCLUDING ones that start with 0 (e.g.
  // "012345" == 12345) — the UI validates the 6-digit string and displays it
  // zero-padded. Only reject 0 (the "use default/random" sentinel) and values
  // that don't fit in 6 digits.
  if (pin == 0 || pin > 999999) return false;
  _prefs.ble_pin = pin;
  savePrefs();
  return true;
}

struct FreqRange {
  uint32_t lower_freq, upper_freq;
};

static FreqRange repeat_freq_ranges[] = {
  #ifdef ALLOWED_REPEAT_FREQ_RANGE
  ALLOWED_REPEAT_FREQ_RANGE
  #else
  { 433000, 433000 },
  { 869495, 869495 },
  { 918000, 918000 }
  #endif
};

bool MyMesh::isValidClientRepeatFreq(uint32_t f) const {
  for (int i = 0; i < sizeof(repeat_freq_ranges)/sizeof(repeat_freq_ranges[0]); i++) {
    auto r = &repeat_freq_ranges[i];
    if (f >= r->lower_freq && f <= r->upper_freq) return true;
  }
  return false;
}

void MyMesh::startInterface(BaseSerialInterface &serial) {
  _serial = &serial;
  serial.enable();
}

void MyMesh::handleCmdFrame(size_t len) {
  if (cmd_frame[0] == CMD_DEVICE_QUERY && len >= 2) { // sent when app establishes connection
    app_target_ver = cmd_frame[1];                    // which version of protocol does app understand
    char client_id[MAX_CLIENT_ID_LEN + 1];
    _serial->getCurrentClientId(client_id, sizeof(client_id));
    setClientTargetVer(client_id, cmd_frame[1]);      // version per connected client

    int i = 0;
    out_frame[i++] = RESP_CODE_DEVICE_INFO;
    out_frame[i++] = FIRMWARE_VER_CODE;
    out_frame[i++] = MAX_CONTACTS / 2;   // v3+
    out_frame[i++] = MAX_GROUP_CHANNELS; // v3+
    memcpy(&out_frame[i], &_prefs.ble_pin, 4);
    i += 4;
    memset(&out_frame[i], 0, 12);
    StrHelper::strzcpy((char *)&out_frame[i], FIRMWARE_BUILD_DATE, 12);
    i += 12;
    StrHelper::strzcpy((char *)&out_frame[i], board.getManufacturerName(), 40);
    i += 40;
    StrHelper::strzcpy((char *)&out_frame[i], FIRMWARE_VERSION, 20);
    i += 20;
    out_frame[i++] = _prefs.client_repeat;   // v9+
    out_frame[i++] = _prefs.path_hash_mode;  // v10+ (matches upstream 1.14 companion protocol)
    _serial->writeFrame(out_frame, i);
  } else if (cmd_frame[0] == CMD_APP_START &&
             len >= 8) { // sent when app establishes connection, respond with node ID
    // Optional client_id: byte 1 = length (0 = none), bytes 2..1+len = client_id. Then app_name.
    char* app_name;
    if (len >= 2 && cmd_frame[1] > 0 && len >= 2 + (size_t)cmd_frame[1]) {
      uint8_t cid_len = cmd_frame[1];
      if (cid_len > MAX_CLIENT_ID_LEN) cid_len = MAX_CLIENT_ID_LEN;
      char cid_buf[MAX_CLIENT_ID_LEN + 1];
      memcpy(cid_buf, &cmd_frame[2], cid_len);
      cid_buf[cid_len] = '\0';
      _serial->setCurrentClientId(cid_buf);
      app_name = (char*)&cmd_frame[2 + cmd_frame[1]];
    } else {
      app_name = (char*)&cmd_frame[8];
    }
    cmd_frame[len] = 0; // make app_name null terminated
    MESH_DEBUG_PRINTLN("App %s connected", app_name);
    char client_id[MAX_CLIENT_ID_LEN + 1];
    _serial->getCurrentClientId(client_id, sizeof(client_id));
    setClientAppName(client_id, app_name);

    _iter_started = false; // stop any left-over ContactsIterator
    int i = 0;
    out_frame[i++] = RESP_CODE_SELF_INFO;
    out_frame[i++] = ADV_TYPE_CHAT; // what this node Advert identifies as (maybe node's pronouns too?? :-)
    out_frame[i++] = _prefs.tx_power_dbm;
    out_frame[i++] = MAX_LORA_TX_POWER;
    memcpy(&out_frame[i], self_id.pub_key, PUB_KEY_SIZE);
    i += PUB_KEY_SIZE;

    int32_t lat, lon;
    lat = (sensors.node_lat * 1000000.0);
    lon = (sensors.node_lon * 1000000.0);
    memcpy(&out_frame[i], &lat, 4);
    i += 4;
    memcpy(&out_frame[i], &lon, 4);
    i += 4;
    out_frame[i++] = _prefs.multi_acks; // new v7+
    out_frame[i++] = _prefs.advert_loc_policy;
    out_frame[i++] = (_prefs.telemetry_mode_env << 4) | (_prefs.telemetry_mode_loc << 2) |
                     (_prefs.telemetry_mode_base); // v5+
    out_frame[i++] = _prefs.manual_add_contacts;

    uint32_t freq = _prefs.freq * 1000;
    memcpy(&out_frame[i], &freq, 4);
    i += 4;
    uint32_t bw = _prefs.bw * 1000;
    memcpy(&out_frame[i], &bw, 4);
    i += 4;
    out_frame[i++] = _prefs.sf;
    out_frame[i++] = _prefs.cr;

    int tlen = strlen(_prefs.node_name); // revisit: UTF_8 ??
    memcpy(&out_frame[i], _prefs.node_name, tlen);
    i += tlen;
    _serial->writeFrame(out_frame, i);
  } else if (cmd_frame[0] == CMD_SEND_TXT_MSG && len >= 14) {
    int i = 1;
    uint8_t txt_type = cmd_frame[i++];
    uint8_t attempt = cmd_frame[i++];
    uint32_t msg_timestamp;
    memcpy(&msg_timestamp, &cmd_frame[i], 4);
    i += 4;
    uint8_t *pub_key_prefix = &cmd_frame[i];
    i += 6;
    if (isMeshcomodRecipient(pub_key_prefix)) {
      char *text = (char *)&cmd_frame[i];
      int tlen = len - i;
      uint32_t expected_ack = msg_timestamp ? msg_timestamp : getRTCClock()->getCurrentTimeUnique();
      uint32_t est_timeout = 1;
      out_frame[0] = RESP_CODE_SENT;
      out_frame[1] = 0; // local handling, not flood
      memcpy(&out_frame[2], &expected_ack, 4);
      memcpy(&out_frame[6], &est_timeout, 4);
      _serial->writeFrame(out_frame, 10);
      // Immediately confirm local command "delivery" so companion UI doesn't keep retrying.
      uint8_t confirmed[9];
      confirmed[0] = PUSH_CODE_SEND_CONFIRMED;
      uint32_t trip_time = 0;
      memcpy(&confirmed[1], &expected_ack, 4);
      memcpy(&confirmed[5], &trip_time, 4);
      _serial->writeFrame(confirmed, sizeof(confirmed));
      if (tlen > 0) {
        text[tlen] = 0;
        if (!isMeshcomodDuplicate(msg_timestamp, text)) {
          rememberMeshcomodCommand(msg_timestamp, text);
          handleMeshcomodCommand(text, tlen);
        }
      } else {
        pushMeshcomodReply("usage: wifi help");
      }
    } else {
    ContactInfo *recipient = lookupContactByPubKey(pub_key_prefix, 6);
    if (recipient && (txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_CLI_DATA)) {
      char *text = (char *)&cmd_frame[i];
      int tlen = len - i;
      uint32_t est_timeout;
      text[tlen] = 0; // ensure null
      int result;
      uint32_t expected_ack;
      bool skip_radio_send = false;
      if (txt_type == TXT_TYPE_PLAIN) {
        uint32_t body_crc = 0;
        mesh::Utils::sha256((uint8_t*)&body_crc, 4, (const uint8_t*)text, tlen);
        uint32_t now_ms = millis();
        if (msg_timestamp != 0 &&
            msg_timestamp == s_last_cmd_txt_ts &&
            memcmp(pub_key_prefix, s_last_cmd_txt_pub6, sizeof(s_last_cmd_txt_pub6)) == 0 &&
            body_crc == s_last_cmd_txt_body_crc &&
            (uint32_t)(now_ms - s_last_cmd_txt_seen_ms) < 30000UL) {
          // Transport/client retry of same command frame: ack locally but avoid re-transmitting stale packet.
          skip_radio_send = true;
          expected_ack = s_last_cmd_txt_ack;
          est_timeout = s_last_cmd_txt_est_timeout;
          result = MSG_SEND_SENT_FLOOD;
        } else {
          s_last_cmd_txt_ts = msg_timestamp;
          memcpy(s_last_cmd_txt_pub6, pub_key_prefix, sizeof(s_last_cmd_txt_pub6));
          s_last_cmd_txt_body_crc = body_crc;
          s_last_cmd_txt_seen_ms = now_ms;
        }
      }
      if (txt_type == TXT_TYPE_CLI_DATA) {
        msg_timestamp = getRTCClock()->getCurrentTimeUnique(); // Use node's RTC instead of app timestamp to avoid tripping replay protection
        TxtTxDebugInfo dbg{};
        result = sendCommandData(*recipient, msg_timestamp, attempt, text, est_timeout, nullptr, &dbg);
        expected_ack = 0; // no Ack expected
        if (_ui) {
          char line[160];
          snprintf(line, sizeof(line), "TX CMD src=CMD_SEND_TXT_MSG kind=CLI ts=%lu att=%u r=%d h=%08lX core_ts=%lu core_att=%u",
                   static_cast<unsigned long>(msg_timestamp),
                   static_cast<unsigned>(attempt),
                   result,
                   static_cast<unsigned long>(dbg.packet_hash4),
                   static_cast<unsigned long>(dbg.uniq_ts),
                   static_cast<unsigned>(dbg.uniq_attempt));
          _ui->appendDiag(line);
        }
      } else if (!skip_radio_send) {
        // Force node-side unique timestamp for plain sends as well.
        // Some clients may resend/reuse app timestamps, which can cause repeated packet hashes
        // and replay-like suppression on receivers.
        msg_timestamp = getRTCClock()->getCurrentTimeUnique();
        uint32_t tx_hash4 = 0;
        TxtTxDebugInfo dbg{};
        result = sendMessage(*recipient, msg_timestamp, attempt, text, expected_ack, est_timeout, &tx_hash4, &dbg);
        if (_ui) {
          char line[192];
          snprintf(line, sizeof(line), "TX CMD src=CMD_SEND_TXT_MSG kind=PLAIN ts=%lu att=%u r=%d ack=%lu h=%08lX core_ts=%lu core_att=%u n=%u",
                   static_cast<unsigned long>(msg_timestamp), static_cast<unsigned>(attempt), result,
                   static_cast<unsigned long>(expected_ack),
                   static_cast<unsigned long>(tx_hash4),
                   static_cast<unsigned long>(dbg.uniq_ts),
                   static_cast<unsigned>(dbg.uniq_attempt),
                   static_cast<unsigned>(dbg.nonce));
          _ui->appendDiag(line);
        }
      }
      // TODO: add expected ACK to table
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        if (txt_type == TXT_TYPE_PLAIN && !skip_radio_send) {
          s_last_cmd_txt_ack = expected_ack;
          s_last_cmd_txt_est_timeout = est_timeout;
        }
        if (expected_ack) {
          expected_ack_table[next_ack_idx].msg_sent = _ms->getMillis(); // add to circular table
          expected_ack_table[next_ack_idx].ack = expected_ack;
          expected_ack_table[next_ack_idx].contact = recipient;
          next_ack_idx = (next_ack_idx + 1) % EXPECTED_ACK_TABLE_SIZE;
        }

        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &expected_ack, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial->writeFrame(out_frame, 10);

        // Do not synthesize a private-message "recv" frame from self. That frame has no recipient
        // context and can be rendered by clients as a chat with self.
      }
    } else {
      writeErrFrame(recipient == NULL
                        ? ERR_CODE_NOT_FOUND
                        : ERR_CODE_UNSUPPORTED_CMD); // unknown recipient, or unsupported TXT_TYPE_*
    }
    }
  } else if (cmd_frame[0] == CMD_SEND_CHANNEL_TXT_MSG) { // send GroupChannel msg
    int i = 1;
    uint8_t txt_type = cmd_frame[i++]; // should be TXT_TYPE_PLAIN
    uint8_t channel_idx = cmd_frame[i++];
    uint32_t msg_timestamp;
    memcpy(&msg_timestamp, &cmd_frame[i], 4);
    i += 4;
    const char *text = (char *)&cmd_frame[i];

    if (txt_type != TXT_TYPE_PLAIN) {
      writeErrFrame(ERR_CODE_UNSUPPORTED_CMD);
    } else {
      ChannelDetails channel;
      bool success = getChannel(channel_idx, channel);
      if (success && sendGroupMessage(msg_timestamp, channel.channel, _prefs.node_name, text, len - i)) {
        writeOKFrame();
        // Sent channel messages are not added to shared history / broadcast: channel_idx and
        // frame format are device-specific and clients (e.g. HA) without channel support can
        // misparse or show "text as sender"; also avoids failed-to-sync when versions differ.
      } else {
        writeErrFrame(ERR_CODE_NOT_FOUND); // bad channel_idx
      }
    }
  } else if (cmd_frame[0] == CMD_SEND_CHANNEL_DATA && len > 5) { // send GroupChannel datagram
    if (len < 4) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
      return;
    }
    int i = 1;
    uint8_t channel_idx = cmd_frame[i++];
    uint8_t path_len = cmd_frame[i++];

    // validate path len, allowing 0xFF for flood
    if (!mesh::Packet::isValidPathLen(path_len) && path_len != OUT_PATH_UNKNOWN) {
      MESH_DEBUG_PRINTLN("CMD_SEND_CHANNEL_DATA invalid path size: %d", path_len);
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
      return;
    }

    // parse provided path if not flood
    uint8_t path[MAX_PATH_SIZE];
    if (path_len != OUT_PATH_UNKNOWN) {
      i += mesh::Packet::writePath(path, &cmd_frame[i], path_len);
    }

    uint16_t data_type = ((uint16_t)cmd_frame[i]) | (((uint16_t)cmd_frame[i + 1]) << 8);
    i += 2;
    const uint8_t *payload = &cmd_frame[i];
    int payload_len = (len > (size_t)i) ? (int)(len - i) : 0;

    ChannelDetails channel;
    if (!getChannel(channel_idx, channel)) {
      writeErrFrame(ERR_CODE_NOT_FOUND); // bad channel_idx
    } else if (data_type == DATA_TYPE_RESERVED) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else if (payload_len > MAX_CHANNEL_DATA_LENGTH) {
      MESH_DEBUG_PRINTLN("CMD_SEND_CHANNEL_DATA payload too long: %d > %d", payload_len, MAX_CHANNEL_DATA_LENGTH);
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else if (sendGroupData(channel.channel, path, path_len, data_type, payload, payload_len)) {
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_TABLE_FULL);
    }
  } else if (cmd_frame[0] == CMD_GET_CONTACTS) { // get Contact list
    if (_iter_started) {
      writeErrFrame(ERR_CODE_BAD_STATE); // iterator is currently busy
    } else {
      if (len >= 5) { // has optional 'since' param
        memcpy(&_iter_filter_since, &cmd_frame[1], 4);
      } else {
        _iter_filter_since = 0;
      }

      uint8_t reply[5];
      reply[0] = RESP_CODE_CONTACTS_START;
      uint32_t count = getNumContacts(); // total, NOT filtered count
      memcpy(&reply[1], &count, 4);
      // Save reply target so CONTACT/END go to same client (WS/TCP) even if next checkRecvFrame overwrites it
      _contact_list_reply_target = _serial->getReplyTarget();
      size_t start_ret = _serial->writeFrame(reply, 5);
      _contact_send_index = 0;
      // No debug prints here: companion stream must be binary-only (no ASCII in same transport as protocol frames)

      // start iterator
      _iter = startContactsIterator();
      _iter_started = true;
      _most_recent_lastmod = 0;
    }
  } else if (cmd_frame[0] == CMD_SET_ADVERT_NAME && len >= 2) {
    int nlen = len - 1;
    if (nlen > sizeof(_prefs.node_name) - 1) nlen = sizeof(_prefs.node_name) - 1; // max len
    memcpy(_prefs.node_name, &cmd_frame[1], nlen);
    _prefs.node_name[nlen] = 0; // null terminator
    savePrefs();
    writeOKFrame();
  } else if (cmd_frame[0] == CMD_SET_ADVERT_LATLON && len >= 9) {
    int32_t lat, lon, alt = 0;
    memcpy(&lat, &cmd_frame[1], 4);
    memcpy(&lon, &cmd_frame[5], 4);
    if (len >= 13) {
      memcpy(&alt, &cmd_frame[9], 4); // for FUTURE support
    }
    if (lat <= 90 * 1E6 && lat >= -90 * 1E6 && lon <= 180 * 1E6 && lon >= -180 * 1E6) {
      sensors.node_lat = ((double)lat) / 1000000.0;
      sensors.node_lon = ((double)lon) / 1000000.0;
      savePrefs();
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG); // invalid geo coordinate
    }
  } else if (cmd_frame[0] == CMD_GET_DEVICE_TIME) {
    uint8_t reply[5];
    reply[0] = RESP_CODE_CURR_TIME;
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply[1], &now, 4);
    _serial->writeFrame(reply, 5);
  } else if (cmd_frame[0] == CMD_SET_DEVICE_TIME && len >= 5) {
    uint32_t secs;
    memcpy(&secs, &cmd_frame[1], 4);
    uint32_t curr = getRTCClock()->getCurrentTime();
    if (secs >= curr) {
      getRTCClock()->setCurrentTime(secs);
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }
  } else if (cmd_frame[0] == CMD_SEND_SELF_ADVERT) {
    mesh::Packet* pkt;
    if (_prefs.advert_loc_policy == ADVERT_LOC_NONE) {
      pkt = createSelfAdvert(_prefs.node_name);
    } else {
      pkt = createSelfAdvert(_prefs.node_name, sensors.node_lat, sensors.node_lon);
    }
    if (pkt) {
      if (len >= 2 && cmd_frame[1] == 1) { // optional param (1 = flood, 0 = zero hop)
        TransportKey default_scope;
        memcpy(&default_scope.key, _prefs.default_scope_key, sizeof(default_scope.key));
        sendFloodScoped(default_scope, pkt, 0);
      } else {
        sendZeroHop(pkt);
      }
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_TABLE_FULL);
    }
  } else if (cmd_frame[0] == CMD_RESET_PATH && len >= 1 + 32) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      recipient->out_path_len = -1;
      // recipient->lastmod = ??   shouldn't be needed, app already has this version of contact
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND); // unknown contact
    }
  } else if (cmd_frame[0] == CMD_ADD_UPDATE_CONTACT && len >= 1 + 32 + 2 + 1) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    uint32_t last_mod = getRTCClock()->getCurrentTime();  // fallback value if not present in cmd_frame
    if (recipient) {
      updateContactFromFrame(*recipient, last_mod, cmd_frame, len);
      recipient->lastmod = last_mod;
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
      writeOKFrame();
    } else {
      ContactInfo contact;
      updateContactFromFrame(contact, last_mod, cmd_frame, len);
      contact.lastmod = last_mod;
      contact.sync_since = 0;
      if (addContact(contact)) {
        dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
#ifdef DISPLAY_CLASS
        // Tell the touch UI to refresh its thread list immediately so the new
        // contact's DM shows up in Chats without waiting for the periodic poll.
        if (_ui) _ui->onThreadsChanged();
#endif
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      }
    }
  } else if (cmd_frame[0] == CMD_REMOVE_CONTACT) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient && removeContact(*recipient)) {
      _store->deleteBlobByKey(pub_key, PUB_KEY_SIZE);
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
#ifdef DISPLAY_CLASS
      // Removing a contact drops its DM thread from the Chats list — ping
      // the UI so it picks the change up immediately.
      if (_ui) _ui->onThreadsChanged();
#endif
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND); // not found, or unable to remove
    }
  } else if (cmd_frame[0] == CMD_SHARE_CONTACT) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      if (shareContactZeroHop(*recipient)) {
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL); // unable to send
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }
  } else if (cmd_frame[0] == CMD_GET_CONTACT_BY_KEY) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *contact = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (contact) {
      writeContactRespFrame(RESP_CODE_CONTACT, *contact);
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND); // not found
    }
  } else if (cmd_frame[0] == CMD_EXPORT_CONTACT) {
    if (len < 1 + PUB_KEY_SIZE) {
      // export SELF
      mesh::Packet* pkt;
      if (_prefs.advert_loc_policy == ADVERT_LOC_NONE) {
        pkt = createSelfAdvert(_prefs.node_name);
      } else {
        pkt = createSelfAdvert(_prefs.node_name, sensors.node_lat, sensors.node_lon);
      }
      if (pkt) {
        pkt->header |= ROUTE_TYPE_FLOOD; // would normally be sent in this mode

        out_frame[0] = RESP_CODE_EXPORT_CONTACT;
        uint8_t out_len = pkt->writeTo(&out_frame[1]);
        releasePacket(pkt); // undo the obtainNewPacket()
        _serial->writeFrame(out_frame, out_len + 1);
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL); // Error
      }
    } else {
      uint8_t *pub_key = &cmd_frame[1];
      ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
      uint8_t out_len;
      if (recipient && (out_len = exportContact(*recipient, &out_frame[1])) > 0) {
        out_frame[0] = RESP_CODE_EXPORT_CONTACT;
        _serial->writeFrame(out_frame, out_len + 1);
      } else {
        writeErrFrame(ERR_CODE_NOT_FOUND); // not found
      }
    }
  } else if (cmd_frame[0] == CMD_IMPORT_CONTACT && len > 2 + 32 + 64) {
    if (importContact(&cmd_frame[1], len - 1)) {
#ifdef DISPLAY_CLASS
      // Imported contact = a new DM thread. Ping the UI so it picks it up
      // immediately instead of after the next 4 s mesh-refresh tick.
      if (_ui) _ui->onThreadsChanged();
#endif
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }
  } else if (cmd_frame[0] == CMD_SYNC_SINCE) {
    if (len >= 5) {
      uint32_t T;
      memcpy(&T, &cmd_frame[1], 4);
      sendSyncSinceDelta(T);
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }
  } else if (cmd_frame[0] == CMD_SYNC_NEXT_MESSAGE) {
    char client_id[MAX_CLIENT_ID_LEN + 1];
    _serial->getCurrentClientId(client_id, sizeof(client_id));
#if COMPANION_SYNC_DEBUG
    g_sync_dbg.req++;
#endif
    uint32_t sent_seq = 0;
    int out_len = getNextFromHistoryForClient(client_id, out_frame, &sent_seq, false);
    const bool had_history_frame = (out_len > 0);
#if COMPANION_SYNC_DEBUG
    if (had_history_frame) g_sync_dbg.had_frame++;
    else g_sync_dbg.no_more++;
#endif
    uint8_t send_buf[MAX_FRAME_SIZE];
    const uint8_t* send_ptr = out_frame;
    if (out_len <= 0) {
      out_frame[0] = RESP_CODE_NO_MORE_MESSAGES;
      out_len = 1;
    } else {
      int adapted_len = adaptHistoryFrameForClient(client_id, out_frame, out_len, send_buf);
      if (adapted_len > 0) {
        send_ptr = send_buf;
        out_len = adapted_len;
      }
    }
    size_t to_send = (size_t)out_len;
    // Retry write so transient full buffers (TCP/BLE) don't cause client timeout
    const int max_retries = 10;
    bool sent_ok = false;
    for (int r = 0; r < max_retries; r++) {
      if (_serial->writeFrame(send_ptr, to_send) == to_send) {
        sent_ok = true;
#if COMPANION_SYNC_DEBUG
        g_sync_dbg.write_ok++;
        g_sync_dbg.retries += (uint32_t)r;
        SYNC_DEBUG_PRINTLN("resp client=%s had=%d code=%u len=%u seq=%lu retry=%d",
                           client_id, had_history_frame ? 1 : 0,
                           (unsigned)send_ptr[0], (unsigned)to_send,
                           (unsigned long)sent_seq, r);
        if (r > 0) {
          SYNC_DEBUG_PRINTLN("retry_ok client=%s retries=%d had=%d len=%u seq=%lu",
                             client_id, r, had_history_frame ? 1 : 0, (unsigned)to_send,
                             (unsigned long)sent_seq);
        }
        if ((g_sync_dbg.req % 100u) == 0u) {
          SYNC_DEBUG_PRINTLN("summary req=%lu had=%lu no_more=%lu ok=%lu fail=%lu retries=%lu",
                             (unsigned long)g_sync_dbg.req, (unsigned long)g_sync_dbg.had_frame,
                             (unsigned long)g_sync_dbg.no_more, (unsigned long)g_sync_dbg.write_ok,
                             (unsigned long)g_sync_dbg.write_fail, (unsigned long)g_sync_dbg.retries);
        }
#endif
        if (had_history_frame) commitHistoryForClient(client_id, sent_seq);
#ifdef DISPLAY_CLASS
        if (_ui && had_history_frame) _ui->msgRead(history_count);
#endif
        break;
      }
      if (r < max_retries - 1) delay(25);
    }
#if COMPANION_SYNC_DEBUG
    if (!sent_ok) {
      g_sync_dbg.write_fail++;
      SYNC_DEBUG_PRINTLN("write_fail client=%s had=%d len=%u seq=%lu req=%lu",
                         client_id, had_history_frame ? 1 : 0, (unsigned)to_send, (unsigned long)sent_seq,
                         (unsigned long)g_sync_dbg.req);
    }
#endif
  } else if (cmd_frame[0] == CMD_SET_RADIO_PARAMS) {
    int i = 1;
    uint32_t freq;
    memcpy(&freq, &cmd_frame[i], 4);
    i += 4;
    uint32_t bw;
    memcpy(&bw, &cmd_frame[i], 4);
    i += 4;
    uint8_t sf = cmd_frame[i++];
    uint8_t cr = cmd_frame[i++];
    uint8_t repeat = 0;  // default - false
    if (len > i) {
      repeat = cmd_frame[i++];   // FIRMWARE_VER_CODE  9+
    }

    if (repeat && !isValidClientRepeatFreq(freq)) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else if (freq >= 300000 && freq <= 2500000 && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8 && bw >= 7000 &&
        bw <= 500000) {
      _prefs.sf = sf;
      _prefs.cr = cr;
      _prefs.freq = (float)freq / 1000.0;
      _prefs.bw = (float)bw / 1000.0;
      _prefs.client_repeat = repeat;
      savePrefs();

      radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
#if defined(USE_SX1262) || defined(USE_SX1268) || defined(SX126X_RX_BOOSTED_GAIN)
      radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain != 0);
#endif
      MESH_DEBUG_PRINTLN("OK: CMD_SET_RADIO_PARAMS: f=%d, bw=%d, sf=%d, cr=%d", freq, bw, (uint32_t)sf,
                         (uint32_t)cr);

      writeOKFrame();
    } else {
      MESH_DEBUG_PRINTLN("Error: CMD_SET_RADIO_PARAMS: f=%d, bw=%d, sf=%d, cr=%d", freq, bw, (uint32_t)sf,
                         (uint32_t)cr);
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }
  } else if (cmd_frame[0] == CMD_SET_RADIO_TX_POWER) {
    int8_t power = (int8_t)cmd_frame[1];
    if (power < -9 || power > MAX_LORA_TX_POWER) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else {
      _prefs.tx_power_dbm = power;
      savePrefs();
      radio_driver.setTxPower(_prefs.tx_power_dbm);
      writeOKFrame();
    }
  } else if (cmd_frame[0] == CMD_SET_TUNING_PARAMS) {
    int i = 1;
    uint32_t rx, af;
    memcpy(&rx, &cmd_frame[i], 4);
    i += 4;
    memcpy(&af, &cmd_frame[i], 4);
    i += 4;
    _prefs.rx_delay_base = ((float)rx) / 1000.0f;
    _prefs.airtime_factor = ((float)af) / 1000.0f;
    savePrefs();
    writeOKFrame();
  } else if (cmd_frame[0] == CMD_GET_TUNING_PARAMS) {
    uint32_t rx = _prefs.rx_delay_base * 1000, af = _prefs.airtime_factor * 1000;
    int i = 0;
    out_frame[i++] = RESP_CODE_TUNING_PARAMS;
    memcpy(&out_frame[i], &rx, 4); i += 4;
    memcpy(&out_frame[i], &af, 4); i += 4;
    _serial->writeFrame(out_frame, i);
  } else if (cmd_frame[0] == CMD_SET_OTHER_PARAMS) {
    _prefs.manual_add_contacts = cmd_frame[1];
    if (len >= 3) {
      _prefs.telemetry_mode_base = cmd_frame[2] & 0x03; // v5+
      _prefs.telemetry_mode_loc = (cmd_frame[2] >> 2) & 0x03;
      _prefs.telemetry_mode_env = (cmd_frame[2] >> 4) & 0x03;

      if (len >= 4) {
        _prefs.advert_loc_policy = cmd_frame[3];
        if (len >= 5) {
          _prefs.multi_acks = cmd_frame[4];
        }
      }
    }
    savePrefs();
    writeOKFrame();
  } else if (cmd_frame[0] == CMD_SET_PATH_HASH_MODE && cmd_frame[1] == 0 && len >= 3) {
    if (cmd_frame[2] >= 3) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else {
      _prefs.path_hash_mode = cmd_frame[2];
      savePrefs();
      writeOKFrame();
    }
  } else if (cmd_frame[0] == CMD_REBOOT && memcmp(&cmd_frame[1], "reboot", 6) == 0) {
    if (dirty_contacts_expiry) { // is there are pending dirty contacts write needed?
      saveContacts();
    }
    board.reboot();
  } else if (cmd_frame[0] == CMD_GET_BATT_AND_STORAGE) {
    uint8_t reply[11];
    int i = 0;
    reply[i++] = RESP_CODE_BATT_AND_STORAGE;
    uint16_t battery_millivolts = board.getBattMilliVolts();
    uint32_t used = _store->getStorageUsedKb();
    uint32_t total = _store->getStorageTotalKb();
    memcpy(&reply[i], &battery_millivolts, 2); i += 2;
    memcpy(&reply[i], &used, 4); i += 4;
    memcpy(&reply[i], &total, 4); i += 4;
    _serial->writeFrame(reply, i);
  } else if (cmd_frame[0] == CMD_EXPORT_PRIVATE_KEY) {
#if ENABLE_PRIVATE_KEY_EXPORT
    uint8_t reply[65];
    reply[0] = RESP_CODE_PRIVATE_KEY;
    self_id.writeTo(&reply[1], 64);
    _serial->writeFrame(reply, 65);
#else
    writeDisabledFrame();
#endif
  } else if (cmd_frame[0] == CMD_IMPORT_PRIVATE_KEY && len >= 65) {
#if ENABLE_PRIVATE_KEY_IMPORT
    if (!mesh::LocalIdentity::validatePrivateKey(&cmd_frame[1])) {
        writeErrFrame(ERR_CODE_ILLEGAL_ARG); // invalid key
    } else {
        mesh::LocalIdentity identity;
        identity.readFrom(&cmd_frame[1], 64);
        if (_store->saveMainIdentity(identity)) {
          self_id = identity;
          writeOKFrame();
          // re-load contacts, to invalidate ecdh shared_secrets
          resetContacts();
          _store->loadContacts(this);
        } else {
          writeErrFrame(ERR_CODE_FILE_IO_ERROR);
        }
    }
#else
    writeDisabledFrame();
#endif
  } else if (cmd_frame[0] == CMD_SEND_RAW_DATA && len >= 6) {
    int i = 1;
    int8_t path_len = cmd_frame[i++];
    if (path_len >= 0 && i + path_len + 4 <= len) { // minimum 4 byte payload
      uint8_t *path = &cmd_frame[i];
      i += path_len;
      // Companion OTA panel currently sends `ota url ...` using CMD_SEND_RAW_DATA with empty path.
      // Treat zero-path ASCII `ota ...` as a local meshcomod command.
      if (path_len == 0 && i < (int)len) {
        char local_cmd[220];
        int payload_len = (int)len - i;
        if (payload_len >= (int)sizeof(local_cmd)) payload_len = (int)sizeof(local_cmd) - 1;
        memcpy(local_cmd, &cmd_frame[i], (size_t)payload_len);
        local_cmd[payload_len] = '\0';
        // Drop trailing NULs/whitespace from transport payload.
        while (payload_len > 0 &&
               (local_cmd[payload_len - 1] == '\0' || local_cmd[payload_len - 1] == '\r' ||
                local_cmd[payload_len - 1] == '\n' || local_cmd[payload_len - 1] == ' ' ||
                local_cmd[payload_len - 1] == '\t')) {
          local_cmd[--payload_len] = '\0';
        }
        const char* cp = local_cmd;
        while (*cp == ' ' || *cp == '\t') cp++;
        if (strncasecmp(cp, "ota", 3) == 0 && (cp[3] == '\0' || cp[3] == ' ' || cp[3] == '\t')) {
          // Acknowledge before synchronous OTA (same pattern as meshcomod TXT: SENT + confirmed first).
          int j = 0;
          out_frame[j++] = PUSH_CODE_BINARY_RESPONSE;
          out_frame[j++] = 0;
          uint32_t tag = 0;
          memcpy(&out_frame[j], &tag, 4);
          j += 4;
          const char *line = "OTA command accepted";
          int ll = (int)strlen(line);
          if (j + ll > MAX_FRAME_SIZE) ll = MAX_FRAME_SIZE - j;
          memcpy(&out_frame[j], line, (size_t)ll);
          j += ll;
          _serial->writeFrame(out_frame, j);
          writeOKFrame();
          handleMeshcomodCommand(cp, (int)strlen(cp));
          return;
        }
      }
      auto pkt = createRawData(&cmd_frame[i], len - i);
      if (pkt) {
        sendDirect(pkt, path, path_len);
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      }
    } else {
      writeErrFrame(ERR_CODE_UNSUPPORTED_CMD); // flood, not supported (yet)
    }
  } else if (cmd_frame[0] == CMD_SEND_LOGIN && len >= 1 + PUB_KEY_SIZE) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    char *password = (char *)&cmd_frame[1 + PUB_KEY_SIZE];
    cmd_frame[len] = 0; // ensure null terminator in password
    if (recipient) {
      uint32_t est_timeout;
      int result = sendLogin(*recipient, password, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        memcpy(&pending_login, recipient->id.pub_key, 4); // match this to onContactResponse()
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &pending_login, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial->writeFrame(out_frame, 10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND); // contact not found
    }
  } else if (cmd_frame[0] == CMD_SEND_ANON_REQ && len > 1 + PUB_KEY_SIZE) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    ContactInfo anon;
    if (recipient == NULL) { // FIRMWARE_VER_CODE 13+,  allow non-contact requests
      memset(&anon, 0, sizeof(anon));
      memcpy(anon.id.pub_key, pub_key, PUB_KEY_SIZE);
      anon.out_path_len = 0;   // default to zero-hop direct
      anon.type = ADV_TYPE_NONE;  // unknown

      if (addContact(anon)) recipient = &anon;
    }
    uint8_t *data = &cmd_frame[1 + PUB_KEY_SIZE];
    if (recipient) {
      uint32_t tag, est_timeout;
      int result = sendAnonReq(*recipient, data, len - (1 + PUB_KEY_SIZE), tag, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        pending_req = tag; // match this to onContactResponse()
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial->writeFrame(out_frame, 10);
      }
    } else {
      writeErrFrame(ERR_CODE_TABLE_FULL); // contacts full
    }
  } else if (cmd_frame[0] == CMD_SEND_STATUS_REQ && len >= 1 + PUB_KEY_SIZE) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      uint32_t tag, est_timeout;
      int result = sendRequest(*recipient, REQ_TYPE_GET_STATUS, tag, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        // FUTURE:  pending_status = tag;  // match this in onContactResponse()
        memcpy(&pending_status, recipient->id.pub_key, 4); // legacy matching scheme
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial->writeFrame(out_frame, 10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND); // contact not found
    }
  } else if (cmd_frame[0] == CMD_SEND_PATH_DISCOVERY_REQ && cmd_frame[1] == 0 && len >= 2 + PUB_KEY_SIZE) {
    uint8_t *pub_key = &cmd_frame[2];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      uint32_t tag, est_timeout;
      // 'Path Discovery' is just a special case of flood + Telemetry req
      uint8_t req_data[9];
      req_data[0] = REQ_TYPE_GET_TELEMETRY_DATA;
      req_data[1] = ~(TELEM_PERM_BASE);  // NEW: inverse permissions mask (ie. we only want BASE telemetry)
      memset(&req_data[2], 0, 3);  // reserved
      getRNG()->random(&req_data[5], 4);   // random blob to help make packet-hash unique
      auto save = recipient->out_path_len;    // temporarily force sendRequest() to flood
      recipient->out_path_len = -1;
      int result = sendRequest(*recipient, req_data, sizeof(req_data), tag, est_timeout);
      recipient->out_path_len = save;
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        pending_discovery = tag; // match this in onContactResponse()
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial->writeFrame(out_frame, 10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND); // contact not found
    }
  } else if (cmd_frame[0] == CMD_SEND_TELEMETRY_REQ && len >= 4 + PUB_KEY_SIZE) {  // can deprecate, in favour of CMD_SEND_BINARY_REQ
    uint8_t *pub_key = &cmd_frame[4];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      uint32_t tag, est_timeout;
      int result = sendRequest(*recipient, REQ_TYPE_GET_TELEMETRY_DATA, tag, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        pending_telemetry = tag; // match this in onContactResponse()
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial->writeFrame(out_frame, 10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND); // contact not found
    }
  } else if (cmd_frame[0] == CMD_SEND_TELEMETRY_REQ && len == 4) {  // 'self' telemetry request
    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
    // query other sensors -- target specific
    sensors.querySensors(0xFF, telemetry);

    int i = 0;
    out_frame[i++] = PUSH_CODE_TELEMETRY_RESPONSE;
    out_frame[i++] = 0; // reserved
    memcpy(&out_frame[i], self_id.pub_key, 6);
    i += 6; // pub_key_prefix
    uint8_t tlen = telemetry.getSize();
    memcpy(&out_frame[i], telemetry.getBuffer(), tlen);
    i += tlen;
    _serial->writeFrame(out_frame, i);
  } else if (cmd_frame[0] == CMD_SEND_BINARY_REQ && len >= 2 + PUB_KEY_SIZE) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *recipient = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      uint8_t *req_data = &cmd_frame[1 + PUB_KEY_SIZE];
      uint32_t tag, est_timeout;
      int result = sendRequest(*recipient, req_data, len - (1 + PUB_KEY_SIZE), tag, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        pending_req = tag; // match this in onContactResponse()
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial->writeFrame(out_frame, 10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND); // contact not found
    }
  } else if (cmd_frame[0] == CMD_HAS_CONNECTION && len >= 1 + PUB_KEY_SIZE) {
    uint8_t *pub_key = &cmd_frame[1];
    if (hasConnectionTo(pub_key)) {
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }
  } else if (cmd_frame[0] == CMD_LOGOUT && len >= 1 + PUB_KEY_SIZE) {
    uint8_t *pub_key = &cmd_frame[1];
    stopConnection(pub_key);
    writeOKFrame();
  } else if (cmd_frame[0] == CMD_GET_CHANNEL && len >= 2) {
    uint8_t channel_idx = cmd_frame[1];
    ChannelDetails channel;
    if (getChannel(channel_idx, channel)) {
      int i = 0;
      out_frame[i++] = RESP_CODE_CHANNEL_INFO;
      out_frame[i++] = channel_idx;
      StrHelper::strzcpy((char *)&out_frame[i], channel.name, 32);
      i += 32;
      memcpy(&out_frame[i], channel.channel.secret, 16);
      i += 16; // NOTE: only 128-bit supported
      _serial->writeFrame(out_frame, i);
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }
  } else if (cmd_frame[0] == CMD_SET_CHANNEL && len >= 2 + 32 + 32) {
    writeErrFrame(ERR_CODE_UNSUPPORTED_CMD); // not supported (yet)
  } else if (cmd_frame[0] == CMD_SET_CHANNEL && len >= 2 + 32 + 16) {
    uint8_t channel_idx = cmd_frame[1];
    ChannelDetails channel;
    StrHelper::strncpy(channel.name, (char *)&cmd_frame[2], 32);
    memset(channel.channel.secret, 0, sizeof(channel.channel.secret));
    memcpy(channel.channel.secret, &cmd_frame[2 + 32], 16); // NOTE: only 128-bit supported
    if (setChannel(channel_idx, channel)) {
      saveChannels();
#ifdef DISPLAY_CLASS
      /* Tell the touch UI to refresh its thread list immediately so the new
       * channel shows up without waiting for the periodic refresh. */
      if (_ui) _ui->onThreadsChanged();
#endif
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND); // bad channel_idx
    }
  } else if (cmd_frame[0] == CMD_SIGN_START) {
    out_frame[0] = RESP_CODE_SIGN_START;
    out_frame[1] = 0; // reserved
    uint32_t len = MAX_SIGN_DATA_LEN;
    memcpy(&out_frame[2], &len, 4);
    _serial->writeFrame(out_frame, 6);

    if (sign_data) {
      free(sign_data);
    }
    sign_data = (uint8_t *)malloc(MAX_SIGN_DATA_LEN);
    sign_data_len = 0;
  } else if (cmd_frame[0] == CMD_SIGN_DATA && len > 1) {
    if (sign_data == NULL || sign_data_len + (len - 1) > MAX_SIGN_DATA_LEN) {
      writeErrFrame(sign_data == NULL ? ERR_CODE_BAD_STATE : ERR_CODE_TABLE_FULL); // error: too long
    } else {
      memcpy(&sign_data[sign_data_len], &cmd_frame[1], len - 1);
      sign_data_len += (len - 1);
      writeOKFrame();
    }
  } else if (cmd_frame[0] == CMD_SIGN_FINISH) {
    if (sign_data) {
      self_id.sign(&out_frame[1], sign_data, sign_data_len);

      free(sign_data); // don't need sign_data now
      sign_data = NULL;

      out_frame[0] = RESP_CODE_SIGNATURE;
      _serial->writeFrame(out_frame, 1 + SIGNATURE_SIZE);
    } else {
      writeErrFrame(ERR_CODE_BAD_STATE);
    }
  } else if (cmd_frame[0] == CMD_SEND_TRACE_PATH && len > 10 && len - 10 < MAX_PACKET_PAYLOAD-5) {
    uint8_t path_len = len - 10;
    uint8_t flags = cmd_frame[9];
    uint8_t path_sz = flags & 0x03;  // NEW v1.11+ 
    if ((path_len >> path_sz) > MAX_PATH_SIZE || (path_len % (1 << path_sz)) != 0) { // make sure is multiple of path_sz
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else {
      uint32_t tag, auth;
      memcpy(&tag, &cmd_frame[1], 4);
      memcpy(&auth, &cmd_frame[5], 4);
      auto pkt = createTrace(tag, auth, flags);
      if (pkt) {
        sendDirect(pkt, &cmd_frame[10], path_len);

        uint32_t t = _radio->getEstAirtimeFor(pkt->payload_len + pkt->path_len + 2);
        uint32_t est_timeout = calcDirectTimeoutMillisFor(t, path_len >> path_sz);

        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial->writeFrame(out_frame, 10);
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      }
    }
  } else if (cmd_frame[0] == CMD_SET_DEVICE_PIN && len >= 5) {

    // get pin from command frame
    uint32_t pin;
    memcpy(&pin, &cmd_frame[1], 4);

    // ensure pin is zero, or a valid 6 digit pin
    if (pin == 0 || (pin >= 100000 && pin <= 999999)) {
      _prefs.ble_pin = pin;
      savePrefs();
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }
  } else if (cmd_frame[0] == CMD_GET_CUSTOM_VARS) {
    out_frame[0] = RESP_CODE_CUSTOM_VARS;
    char *dp = (char *)&out_frame[1];
    for (int i = 0; i < sensors.getNumSettings() && dp - (char *)&out_frame[1] < 140; i++) {
      if (i > 0) {
        *dp++ = ',';
      }
      strcpy(dp, sensors.getSettingName(i));
      dp = strchr(dp, 0);
      *dp++ = ':';
      strcpy(dp, sensors.getSettingValue(i));
      dp = strchr(dp, 0);
    }
    _serial->writeFrame(out_frame, dp - (char *)out_frame);
  } else if (cmd_frame[0] == CMD_SET_CUSTOM_VAR && len >= 4) {
    cmd_frame[len] = 0;
    char *sp = (char *)&cmd_frame[1];
    char *np = strchr(sp, ':'); // look for separator char
    if (np) {
      *np++ = 0; // modify 'cmd_frame', replace ':' with null
      bool success = sensors.setSettingValue(sp, np);
      if (success) {
        #if ENV_INCLUDE_GPS == 1
        // Update node preferences for GPS settings
        if (strcmp(sp, "gps") == 0) {
          _prefs.gps_enabled = (np[0] == '1') ? 1 : 0;
          savePrefs();
        } else if (strcmp(sp, "gps_interval") == 0) {
          uint32_t interval_seconds = atoi(np);
          _prefs.gps_interval = constrain(interval_seconds, 0, 86400);
          savePrefs();
        }
        #endif
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_ILLEGAL_ARG);
      }
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }
  } else if (cmd_frame[0] == CMD_GET_ADVERT_PATH && len >= PUB_KEY_SIZE+2) {
    // FUTURE use:  uint8_t reserved = cmd_frame[1];
    uint8_t *pub_key = &cmd_frame[2];
    AdvertPath* found = NULL;
    for (int i = 0; i < ADVERT_PATH_TABLE_SIZE; i++) {
      auto p = &advert_paths[i];
      if (memcmp(p->pubkey_prefix, pub_key, sizeof(p->pubkey_prefix)) == 0) {
        found = p;
        break;
      }
    }
    if (found) {
      out_frame[0] = RESP_CODE_ADVERT_PATH;
      memcpy(&out_frame[1], &found->recv_timestamp, 4);
      out_frame[5] = found->path_len;
      memcpy(&out_frame[6], found->path, found->path_len);
      _serial->writeFrame(out_frame, 6 + found->path_len);
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }
  } else if (cmd_frame[0] == CMD_GET_STATS && len >= 2) {
    uint8_t stats_type = cmd_frame[1];
    if (stats_type == STATS_TYPE_CORE) {
      int i = 0;
      out_frame[i++] = RESP_CODE_STATS;
      out_frame[i++] = STATS_TYPE_CORE;
      uint16_t battery_mv = board.getBattMilliVolts();
      uint32_t uptime_secs = _ms->getMillis() / 1000;
      uint8_t queue_len = (uint8_t)_mgr->getOutboundTotal();
      memcpy(&out_frame[i], &battery_mv, 2); i += 2;
      memcpy(&out_frame[i], &uptime_secs, 4); i += 4;
      memcpy(&out_frame[i], &_err_flags, 2); i += 2;
      out_frame[i++] = queue_len;
      _serial->writeFrame(out_frame, i);
    } else if (stats_type == STATS_TYPE_RADIO) {
      int i = 0;
      out_frame[i++] = RESP_CODE_STATS;
      out_frame[i++] = STATS_TYPE_RADIO;
      int16_t noise_floor = (int16_t)_radio->getNoiseFloor();
      int8_t last_rssi = (int8_t)radio_driver.getLastRSSI();
      int8_t last_snr = (int8_t)(radio_driver.getLastSNR() * 4); // scaled by 4 for 0.25 dB precision
      uint32_t tx_air_secs = getTotalAirTime() / 1000;
      uint32_t rx_air_secs = getReceiveAirTime() / 1000;
      memcpy(&out_frame[i], &noise_floor, 2); i += 2;
      out_frame[i++] = last_rssi;
      out_frame[i++] = last_snr;
      memcpy(&out_frame[i], &tx_air_secs, 4); i += 4;
      memcpy(&out_frame[i], &rx_air_secs, 4); i += 4;
      _serial->writeFrame(out_frame, i);
    } else if (stats_type == STATS_TYPE_PACKETS) {
      int i = 0;
      out_frame[i++] = RESP_CODE_STATS;
      out_frame[i++] = STATS_TYPE_PACKETS;
      uint32_t recv = radio_driver.getPacketsRecv();
      uint32_t sent = radio_driver.getPacketsSent();
      uint32_t n_sent_flood = getNumSentFlood();
      uint32_t n_sent_direct = getNumSentDirect();
      uint32_t n_recv_flood = getNumRecvFlood();
      uint32_t n_recv_direct = getNumRecvDirect();
      uint32_t n_recv_errors = radio_driver.getPacketsRecvErrors();
      memcpy(&out_frame[i], &recv, 4); i += 4;
      memcpy(&out_frame[i], &sent, 4); i += 4;
      memcpy(&out_frame[i], &n_sent_flood, 4); i += 4;
      memcpy(&out_frame[i], &n_sent_direct, 4); i += 4;
      memcpy(&out_frame[i], &n_recv_flood, 4); i += 4;
      memcpy(&out_frame[i], &n_recv_direct, 4); i += 4;
      memcpy(&out_frame[i], &n_recv_errors, 4); i += 4;
      _serial->writeFrame(out_frame, i);
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG); // invalid stats sub-type
    }
  } else if (cmd_frame[0] == CMD_FACTORY_RESET && memcmp(&cmd_frame[1], "reset", 5) == 0) {
    if (_serial) {
      MESH_DEBUG_PRINTLN("Factory reset: disabling serial interface to prevent reconnects (BLE/WiFi)");
      _serial->disable(); // Phone app disconnects before we can send OK frame so it's safe here
    }
    bool success = _store->formatFileSystem();
    if (success) {
      writeOKFrame();
      delay(1000);
      board.reboot();  // doesn't return
    } else {
      writeErrFrame(ERR_CODE_FILE_IO_ERROR);
    }
  } else if (cmd_frame[0] == CMD_SET_FLOOD_SCOPE && len >= 2 && cmd_frame[1] == 0) {
    if (len >= 2 + 16) {
      memcpy(send_scope.key, &cmd_frame[2], sizeof(send_scope.key));  // set scope override TransportKey
    } else {
      memset(send_scope.key, 0, sizeof(send_scope.key));  // reset scope override
    }
    send_unscoped = false;
    writeOKFrame();
  } else if (cmd_frame[0] == CMD_SET_FLOOD_SCOPE && len >= 2 && cmd_frame[1] == 1) {  // ver 12+ (1.16 send_unscoped; fork keeps its cmd name/number 54)
    send_unscoped = true;
    writeOKFrame();
  } else if (cmd_frame[0] == CMD_SET_DEFAULT_FLOOD_SCOPE && len >= 1) {
    // MeshCore 1.16: persistent default region scope (companion-v1.16.0.3 / issue #31).
    // Payload [63, name(31), key(16)]; an empty payload ([63]) clears it. Without this
    // handler the official MeshCore app's "default scope" setting got no reply
    // ("no_event_received"). The flood path already applies _prefs.default_scope_key.
    if (len >= 1 + 31 + 16) {
      int n = strlen((char *)&cmd_frame[1]);
      if (n > 0 && n < 31) {
        strcpy(_prefs.default_scope_name, (char *)&cmd_frame[1]);
        memcpy(_prefs.default_scope_key, &cmd_frame[1 + 31], 16);
        savePrefs();
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_ILLEGAL_ARG);
      }
    } else {
      memset(_prefs.default_scope_name, 0, sizeof(_prefs.default_scope_name));  // null = unscoped
      memset(_prefs.default_scope_key, 0, sizeof(_prefs.default_scope_key));
      savePrefs();
      writeOKFrame();
    }
  } else if (cmd_frame[0] == CMD_GET_DEFAULT_FLOOD_SCOPE) {
    out_frame[0] = RESP_CODE_DEFAULT_FLOOD_SCOPE;
    if (strlen(_prefs.default_scope_name) > 0) {
      memcpy(&out_frame[1], _prefs.default_scope_name, 31);
      memcpy(&out_frame[1 + 31], _prefs.default_scope_key, 16);
      _serial->writeFrame(out_frame, 1 + 31 + 16);
    } else {
      _serial->writeFrame(out_frame, 1);   // no name/key = null
    }
  } else if (cmd_frame[0] == CMD_SEND_CONTROL_DATA && len >= 2 && (cmd_frame[1] & 0x80) != 0) {
    auto resp = createControlData(&cmd_frame[1], len - 1);
    if (resp) {
      sendZeroHop(resp);
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_TABLE_FULL);
    }
  } else if (cmd_frame[0] == CMD_SET_AUTOADD_CONFIG) {
    _prefs.autoadd_config = cmd_frame[1];
    if (len >= 3) {
      uint8_t mh = cmd_frame[2];
      _prefs.autoadd_max_hops = mh > 64 ? 64 : mh;
    }
    savePrefs();
    writeOKFrame();  
  } else if (cmd_frame[0] == CMD_GET_AUTOADD_CONFIG) {
    int i = 0;
    out_frame[i++] = RESP_CODE_AUTOADD_CONFIG;
    out_frame[i++] = _prefs.autoadd_config;
    out_frame[i++] = _prefs.autoadd_max_hops;
    _serial->writeFrame(out_frame, i);
  } else if (cmd_frame[0] == CMD_GET_ALLOWED_REPEAT_FREQ) {
    int i = 0;
    out_frame[i++] = RESP_ALLOWED_REPEAT_FREQ;
    for (int k = 0; k < sizeof(repeat_freq_ranges)/sizeof(repeat_freq_ranges[0]) && i + 8 < sizeof(out_frame); k++) {
      auto r = &repeat_freq_ranges[k];
      memcpy(&out_frame[i], &r->lower_freq, 4); i += 4;
      memcpy(&out_frame[i], &r->upper_freq, 4); i += 4;
    }
    _serial->writeFrame(out_frame, i);
  } else if (cmd_frame[0] == CMD_SEND_RAW_PACKET && len >= 4) {
    auto pkt = obtainNewPacket();
    if (pkt) {
      uint8_t priority = cmd_frame[1];
      if (tryParsePacket(pkt, &cmd_frame[2], len - 2)) {
        sendPacket(pkt, priority, 0);
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_ILLEGAL_ARG);
      }
    } else {
      writeErrFrame(ERR_CODE_TABLE_FULL);
    }
  } else {
    writeErrFrame(ERR_CODE_UNSUPPORTED_CMD);
    MESH_DEBUG_PRINTLN("ERROR: unknown command: %02X", cmd_frame[0]);
  }
}

static bool save_filter(const ContactInfo& c) {
  return c.type != ADV_TYPE_NONE;   // don't save the transient/anon entries
}

void MyMesh::saveContacts() {
  _store->saveContacts(this, save_filter);
}

void MyMesh::enterCLIRescue() {
  _cli_rescue = true;
  cli_command[0] = 0;
  Serial.println("========= CLI Rescue =========");
}

void MyMesh::checkCLIRescueCmd() {
  int len = strlen(cli_command);
  bool line_complete = false;
  while (Serial.available() && len < sizeof(cli_command)-1) {
    if (Serial.peek() == '<') {
      cli_command[0] = 0;
      return;
    }
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      line_complete = true;
      Serial.print(c);  // echo
      break;
    } else {
      cli_command[len++] = c;
      cli_command[len] = 0;
      Serial.print(c);  // echo
    }
  }
  if (len == sizeof(cli_command)-1) {  // command buffer full
    line_complete = true;
  }

  if (line_complete && len > 0) {  // received complete line

    if (memcmp(cli_command, "set ", 4) == 0) {
      const char* config = &cli_command[4];
      if (memcmp(config, "pin ", 4) == 0) {
        _prefs.ble_pin = atoi(&config[4]);
        savePrefs();
        Serial.printf("  > pin is now %06d\n", _prefs.ble_pin);
      } else if (memcmp(config, "wifi.ssid ", 10) == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
        const char* ssid = &config[10];
        if (wifiConfigSetSsid((char*)ssid)) {
          Serial.println("  > OK: wifi ssid set");
        } else {
          Serial.println("  Error: invalid/too long SSID");
        }
#else
        Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
        Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
      } else if (memcmp(config, "wifi.pwd ", 9) == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
        const char* pwd = &config[9];
        if (wifiConfigSetPwd((char*)pwd)) {
          Serial.println("  > OK: wifi password set");
        } else {
          Serial.println("  Error: password too long");
        }
#else
        Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
        Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
      } else if (memcmp(config, "wifi.radio ", 11) == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
        int v = atoi(&config[11]);
        wifiConfigSetRadioEnabled(v != 0);
        Serial.println(v ? "  > OK: wifi radio on" : "  > OK: wifi radio off");
#else
        Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
        Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
      } else if (strcmp(config, "wifi.apply") == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
        if (!wifiConfigGetRadioEnabled()) {
          Serial.println("  Error: wifi radio off; set wifi.radio 1 first");
        } else if (!wifiConfigHasRuntime()) {
          Serial.println("  Error: no runtime credentials set");
        } else {
          wifiConfigApply();
          Serial.println("  > OK: reconnecting WiFi");
        }
#else
        Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
        Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
      } else if (strcmp(config, "wifi.clear") == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
        wifiConfigClear();
        Serial.println("  > OK: wifi credentials cleared");
#else
        Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
        Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
      } else {
        Serial.printf("  Error: unknown config: %s\n", config);
      }
    } else if (strcmp(cli_command, "get wifi.ssid") == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
      char ssid[WIFI_CONFIG_SSID_MAX];
      wifiConfigGetSsid(ssid, sizeof(ssid));
      Serial.printf("  > %s\n", ssid[0] ? ssid : "(none)");
#else
      Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
      Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
    } else if (strcmp(cli_command, "get wifi.status") == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
      char ssid[WIFI_CONFIG_SSID_MAX];
      wifiConfigGetSsid(ssid, sizeof(ssid));
      bool has_runtime = wifiConfigHasRuntime();
      int re = wifiConfigGetRadioEnabled() ? 1 : 0;
      Serial.printf("  > runtime=%d radio_enabled=%d ssid=%s\n", has_runtime ? 1 : 0, re,
                    (ssid[0] ? ssid : "(none)"));
      if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        Serial.printf("  > connected=1 ip=%d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
      } else {
        Serial.println("  > connected=0");
      }
#else
        Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
        Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
    } else if (strcmp(cli_command, "wifi.status") == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
      char ssid[WIFI_CONFIG_SSID_MAX];
      wifiConfigGetSsid(ssid, sizeof(ssid));
      bool has_runtime = wifiConfigHasRuntime();
      int re = wifiConfigGetRadioEnabled() ? 1 : 0;
      Serial.printf("  > runtime=%d radio_enabled=%d ssid=%s\n", has_runtime ? 1 : 0, re,
                    (ssid[0] ? ssid : "(none)"));
      if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        Serial.printf("  > connected=1 ip=%d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
      } else {
        Serial.println("  > connected=0");
      }
#else
        Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
        Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
    } else if (strcmp(cli_command, "wifi.apply") == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
      if (!wifiConfigGetRadioEnabled()) {
        Serial.println("  Error: wifi radio off; set wifi.radio 1 first");
      } else if (!wifiConfigHasRuntime()) {
        Serial.println("  Error: no runtime credentials set");
      } else {
        wifiConfigApply();
        Serial.println("  > OK: reconnecting WiFi");
      }
#else
        Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
        Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
    } else if (strcmp(cli_command, "wifi.clear") == 0) {
#ifdef ESP32
#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
      wifiConfigClear();
      Serial.println("  > OK: wifi credentials cleared");
#else
      Serial.println("  Error: WiFi config not enabled in this build");
#endif
#else
      Serial.println("  Error: WiFi config only supported on ESP32 builds");
#endif
    } else if (strcmp(cli_command, "rebuild") == 0) {
      bool success = _store->formatFileSystem();
      if (success) {
        _store->saveMainIdentity(self_id);
        savePrefs();
        saveContacts();
        saveChannels();
        Serial.println("  > erase and rebuild done");
      } else {
        Serial.println("  Error: erase failed");
      }
    } else if (strcmp(cli_command, "erase") == 0) {
      bool success = _store->formatFileSystem();
      if (success) {
        Serial.println("  > erase done");
      } else {
        Serial.println("  Error: erase failed");
      }
    } else if (memcmp(cli_command, "ls", 2) == 0) {

      // get path from command e.g: "ls /adafruit"
      const char *path = &cli_command[3];

      bool is_fs2 = false;
      if (memcmp(path, "UserData/", 9) == 0) {
        path += 8; // skip "UserData"
      } else if (memcmp(path, "ExtraFS/", 8) == 0) {
        path += 7; // skip "ExtraFS"
        is_fs2 = true;
      }
      Serial.printf("Listing files in %s\n", path);

      // log each file and directory
      File root = _store->openRead(path);
      if (is_fs2 == false) {
        if (root) {
          File file = root.openNextFile();
          while (file) {
            if (file.isDirectory()) {
              Serial.printf("[dir]  UserData%s/%s\n", path, file.name());
            } else {
              Serial.printf("[file] UserData%s/%s (%d bytes)\n", path, file.name(), file.size());
            }
            // move to next file
            file = root.openNextFile();
          }
          root.close();
        }
      }

      if (is_fs2 == true || strlen(path) == 0 || strcmp(path, "/") == 0) {
        if (_store->getSecondaryFS() != nullptr) {
          File root2 = _store->openRead(_store->getSecondaryFS(), path);
          File file = root2.openNextFile();
          while (file) {
            if (file.isDirectory()) {
              Serial.printf("[dir]  ExtraFS%s/%s\n", path, file.name());
            } else {
              Serial.printf("[file] ExtraFS%s/%s (%d bytes)\n", path, file.name(), file.size());
            }
            // move to next file
            file = root2.openNextFile();
          }
          root2.close();
        }
      }
    } else if (memcmp(cli_command, "cat", 3) == 0) {

      // get path from command e.g: "cat /contacts3"
      const char *path = &cli_command[4];
      
      bool is_fs2 = false;
      if (memcmp(path, "UserData/", 9) == 0) {
        path += 8; // skip "UserData"
      } else if (memcmp(path, "ExtraFS/", 8) == 0) {
        path += 7; // skip "ExtraFS"
        is_fs2 = true;
      } else {
        Serial.println("Invalid path provided, must start with UserData/ or ExtraFS/");
        cli_command[0] = 0;
        return;
      }

      // log file content as hex
      File file = _store->openRead(path);
      if (is_fs2 == true) {
        file = _store->openRead(_store->getSecondaryFS(), path);
      }
      if(file){

        // get file content
        int file_size = file.available();
        uint8_t buffer[file_size];
        file.read(buffer, file_size);

        // print hex
        mesh::Utils::printHex(Serial, buffer, file_size);
        Serial.print("\n");

        file.close();

      }

    } else if (memcmp(cli_command, "rm ", 3) == 0) {
      // get path from command e.g: "rm /adv_blobs"
      const char *path = &cli_command[3];
      MESH_DEBUG_PRINTLN("Removing file: %s", path);
      // ensure path is not empty, or root dir
      if(!path || strlen(path) == 0 || strcmp(path, "/") == 0){
        Serial.println("Invalid path provided");
      } else {
      bool is_fs2 = false;
      if (memcmp(path, "UserData/", 9) == 0) {
        path += 8; // skip "UserData"
      } else if (memcmp(path, "ExtraFS/", 8) == 0) {
        path += 7; // skip "ExtraFS"
        is_fs2 = true;
      }

        // remove file
        bool removed;
        if (is_fs2) {
          MESH_DEBUG_PRINTLN("Removing file from ExtraFS: %s", path);
          removed = _store->removeFile(_store->getSecondaryFS(), path);
        } else {
          MESH_DEBUG_PRINTLN("Removing file from UserData: %s", path);
          removed = _store->removeFile(path);
        }
        if(removed){
          Serial.println("File removed");
        } else {
          Serial.println("Failed to remove file");
        }

      }

    } else if (strcmp(cli_command, "reboot") == 0) {
      board.reboot();  // doesn't return
    } else {
      Serial.println("  Error: unknown command");
    }

    cli_command[0] = 0;  // reset command buffer
  }
}

void MyMesh::checkSerialInterface() {
  bool handled_cmd = false;
  // Drain a small burst of inbound frames each loop to reduce sync latency under load.
  for (int n = 0; n < 4; n++) {
    size_t len = _serial->checkRecvFrame(cmd_frame);
    if (len == 0) break;
    handled_cmd = true;
    handleCmdFrame(len);
  }
  if (!handled_cmd && _iter_started              // check if our ContactsIterator is 'running'
             && !_serial->isWriteBusy() // don't spam the Serial Interface too quickly!
  ) {
    // Restore reply target so CONTACT/END go to the client that got START (fixes WS/TCP when USB is polled first)
    _serial->setReplyTarget(_contact_list_reply_target);
    ContactInfo contact;
    bool found = false;
    while (_iter.hasNext(this, contact)) {
      if (contact.type != ADV_TYPE_NONE) {
        found = true;
        break;
      }
    }

    if (found) {
      if (contact.lastmod > _iter_filter_since) { // apply the 'since' filter
        // Retry so transient full buffers (TCP/WebSocket) don't drop CONTACT frames
        const int max_retries = 10;
        size_t sent = 0;
        for (int r = 0; r < max_retries && sent == 0; r++) {
          if (r > 0) delay(5);
          sent = writeContactRespFrame(RESP_CODE_CONTACT, contact);
        }
        _contact_send_index++;
        if (contact.lastmod > _most_recent_lastmod) {
          _most_recent_lastmod = contact.lastmod; // save for the RESP_CODE_END_OF_CONTACTS frame
        }
      }
    } else { // EOF
      ContactInfo meshcomod;
      getMeshcomodContact(meshcomod);
      // Retry meshcomod CONTACT and END so WiFi/WebSocket get full sequence
      const int max_retries = 10;
      size_t sent = 0;
      for (int r = 0; r < max_retries && sent == 0; r++) {
        if (r > 0) delay(5);
        sent = writeContactRespFrame(RESP_CODE_CONTACT, meshcomod);
      }
      out_frame[0] = RESP_CODE_END_OF_CONTACTS;
      memcpy(&out_frame[1], &_most_recent_lastmod,
             4); // include the most recent lastmod, so app can update their 'since'
      sent = 0;
      for (int r = 0; r < max_retries && sent != 5; r++) {
        if (r > 0) delay(5);
        sent = _serial->writeFrame(out_frame, 5);
      }
      _iter_started = false;
    }
  //} else if (!_serial->isWriteBusy()) {
  //  checkConnections();    // TODO - deprecate the 'Connections' stuff
  }
}

void MyMesh::loop() {
  BaseChatMesh::loop();

  if (_cli_rescue) {
    checkCLIRescueCmd();
  } else {
    // Prefer plain-text console commands (e.g. flasher Console) before binary
    // companion parsing — but only when the first byte looks like a command
    // letter (a-z, A-Z). App binary frames use command bytes 1–62; 32–62 are
    // printable, so we must not treat them as console or we break USB app connection.
#if defined(ESP32)
    if (Serial.available() > 0) {
      int first = Serial.peek();
      // Web consoles often send CRLF. Drop a leading line-ending byte so a
      // stale '\r'/'\n' cannot block the next plain-text command.
      if (first == '\r' || first == '\n') {
        Serial.read();
        if (Serial.available() > 0) {
          first = Serial.peek();
        }
      }
      if (first == '<') {
        // Companion frame marker: handled below by checkSerialInterface().
      } else if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z')) {
        checkCLIRescueCmd();
      }
    }
#endif
    // Always process companion frames in the same loop so TCP/BLE clients cannot
    // be starved by plain-text console traffic on USB Serial.
    checkSerialInterface();
  }

  // is there are pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    saveContacts();
    dirty_contacts_expiry = 0;
  }

#if defined(ENABLE_ADVERT_ON_BOOT) && ENABLE_ADVERT_ON_BOOT == 1
  // Fire the one-shot boot advert when the scheduled time passes. Flood so
  // it reaches peers across repeaters, refreshing any stale pubkey for us
  // in their contact lists (with auto-add on). Without this, DMs from us
  // silently MAC-fail at peers that still have a prior identity.
  if (!_boot_advert_done && _boot_advert_due_ms != 0 &&
      _ms->getMillis() >= _boot_advert_due_ms) {
    _boot_advert_done = true;
    bool ok = sendAdvert(true);
#ifdef DISPLAY_CLASS
    if (_ui) {
      char dbg[64];
      snprintf(dbg, sizeof(dbg), "TX self-advert flood %s",
               ok ? "ok" : "FAIL");
      _ui->appendDiag(dbg);
    }
#else
    (void)ok;
#endif
  }
#endif

#ifdef DISPLAY_CLASS
  if (_ui) _ui->setHasConnection(_serial->isConnected());
#endif
}

#if defined(ESP32) && (defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION))
void MyMesh::pushCompanionOtaProgressLine(const char* line) {
  if (!line || !_serial) return;
#if defined(MULTI_TRANSPORT_COMPANION)
  if (s_companion_ota_pinned_reply_target >= 0)
    _serial->setReplyTarget(s_companion_ota_pinned_reply_target);
#endif
  if (!_serial->isConnected()) return;
  int j = 0;
  out_frame[j++] = PUSH_CODE_BINARY_RESPONSE;
  out_frame[j++] = 0;
  uint32_t tag = 0;
  memcpy(&out_frame[j], &tag, 4);
  j += 4;
  int ll = (int)strlen(line);
  if (j + ll > MAX_FRAME_SIZE) ll = MAX_FRAME_SIZE - j;
  memcpy(&out_frame[j], line, (size_t)ll);
  j += ll;
  _serial->writeFrame(out_frame, j);
}

void meshcoreRepeaterTcpOtaEmitLine(const char* line) {
  the_mesh.pushCompanionOtaProgressLine(line);
#ifdef MULTI_TRANSPORT_COMPANION
  if (line && line[0])
    Serial.printf("%s\n", line);
#endif
}
#endif

bool MyMesh::advert() {
  return sendAdvert(false);   // backward-compat: original advert() was zero-hop
}

bool MyMesh::sendAdvert(bool flood) {
  mesh::Packet* pkt;
  if (_prefs.advert_loc_policy == ADVERT_LOC_NONE) {
    pkt = createSelfAdvert(_prefs.node_name);
  } else {
    pkt = createSelfAdvert(_prefs.node_name, sensors.node_lat, sensors.node_lon);
  }
  if (!pkt) return false;
  if (flood) {
    // path_hash_size = path_hash_mode + 1 (matches the CMD_SEND_ADVERT path).
    sendFlood(pkt, 0, (uint8_t)(_prefs.path_hash_mode + 1));
  } else {
    sendZeroHop(pkt);
  }
  return true;
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
  return _mgr->getOutboundTotal() > 0 || dirty_contacts_expiry != 0;
}
