// SPDX-License-Identifier: GPL-3.0-or-later
//
// A7682E 4G modem service for the LilyGo T-Deck Max: power, SIM, network
// registration, voice calls, SMS and HTTP(S) through the modem's own IP stack.
//
// Threading: the modem is owned by ONE FreeRTOS task ("cell"). Nothing else
// touches the UART. Other code talks to it through three thread-safe surfaces:
//   - commands   (cellDial, cellSmsSend, ...): queued, never block the caller
//   - a snapshot (cellGetStatus): copied under a mutex
//   - events     (cellPollEvent): a small ring drained by the UI thread
// This keeps every AT round trip (some take tens of seconds) off the Arduino
// loop, where LVGL and the mesh run.
//
// Pins and power sequence follow LilyGo's own T-Deck MAX A7670G/A7682E
// examples (lib/TDeckMaxBoard + examples/A7670G/A7670GBoard.h):
//   UART: ESP32 RX = GPIO 11, ESP32 TX = GPIO 10, 115200 8N1
//   RI = GPIO 7 (input), DTR = GPIO 8 (held LOW = modem awake)
//   supply = XL9555 line 0, PWRKEY = XL9555 line 8 (pulse HIGH 1.1 s)
//   speaker mux = XL9555 line 10 (HIGH = modem audio), amp = XL9555 line 6
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef PIN_MODEM_RX
  #define PIN_MODEM_RX   11   // ESP32 receives here (LilyGo: BOARD_A7682E_TXD)
#endif
#ifndef PIN_MODEM_TX
  #define PIN_MODEM_TX   10   // ESP32 transmits here (LilyGo: BOARD_A7682E_RXD)
#endif
#ifndef PIN_MODEM_RI
  #define PIN_MODEM_RI    7
#endif
#ifndef PIN_MODEM_DTR
  #define PIN_MODEM_DTR   8
#endif

enum CellPower : uint8_t {
  CELL_POWER_OFF = 0,
  CELL_POWER_BOOTING,     // rail up, waiting for AT
  CELL_POWER_READY,       // AT answers, init done
  CELL_POWER_ERROR,       // no AT response after power-on
};

enum CellSim : uint8_t {
  CELL_SIM_UNKNOWN = 0,
  CELL_SIM_READY,
  CELL_SIM_PIN,           // locked, no (or wrong) PIN configured
  CELL_SIM_PUK,
  CELL_SIM_ABSENT,
};

enum CellReg : uint8_t {
  CELL_REG_NONE = 0,
  CELL_REG_SEARCHING,
  CELL_REG_HOME,
  CELL_REG_ROAMING,
  CELL_REG_DENIED,
};

enum CellCall : uint8_t {
  CELL_CALL_IDLE = 0,
  CELL_CALL_DIALING,      // outgoing, not yet ringing at the far end
  CELL_CALL_ALERTING,     // outgoing, far end ringing
  CELL_CALL_INCOMING,     // ringing here
  CELL_CALL_ACTIVE,       // connected, audio routed to the modem
  CELL_CALL_HELD,
};

enum CellEventType : uint8_t {
  CELL_EV_NONE = 0,
  CELL_EV_STATUS,         // power/sim/registration changed
  CELL_EV_CALL,           // call state changed (text = number, code = CellCall)
  CELL_EV_RING,           // one RING while incoming (text = number) -- ~every 3 s
  CELL_EV_SMS,            // SMS received (text = sender, body = message)
  CELL_EV_SMS_SENT,       // code = 1 ok / 0 failed (text = number)
  CELL_EV_HTTP,           // HTTP finished (code = status or <0 error), see cellHttpTake
  CELL_EV_DATA,           // data context up/down (code = 1/0)
};

struct CellStatus {
  CellPower power;
  CellSim   sim;
  CellReg   reg;
  CellCall  call;
  int8_t    csq;              // 0..31, -1 = unknown
  int8_t    rssi_dbm;         // derived from csq, 0 = unknown
  bool      data_up;
  bool      enabled;          // user setting: modem should be on
  char      op[24];           // operator name
  char      ip[40];
  char      call_number[32];
  uint32_t  call_started_ms;  // millis() at ACTIVE, 0 otherwise
  char      imei[20];
  char      apn[48];
  char      last_error[48];
};

struct CellEvent {
  CellEventType type;
  int16_t       code;
  char          text[32];     // number / sender
  char          body[284];    // SMS body (UTF-8; one UCS2 SMS = 70 units)
  char          when[24];     // SMS: network timestamp "yy/MM/dd,hh:mm:ss+zz" ("" if none)
};

// SMS inbox, kept on the app storage root (SD) by the UI side (CellUi.cpp) so a
// message is not lost when no app is open. Newest first.
struct CellInboxEntry {
  char from[32];
  char when[24];
  char text[284];
};
int  cellInboxLoad(CellInboxEntry* out, int max);   // UI thread; returns count
bool cellInboxClear();

// Boot hook: reads the saved settings and powers the modem if it was enabled.
// Safe to call once from setup() after board.begin().
void cellBegin();

// User setting. Persists; powering on takes ~10-20 s (reported via events).
void cellSetEnabled(bool on);
// APN for data ("" = modem default). user/pass are optional. Persists.
void cellSetApn(const char* apn, const char* user, const char* pass);
// SIM PIN used if the card is locked. Persists. "" = none.
void cellSetPin(const char* pin);

void cellGetStatus(CellStatus* out);
bool cellPollEvent(CellEvent* out);           // UI thread; false = ring empty

// Commands (async; results arrive as events). Return false if the queue is
// full or the request is invalid -- they never block.
bool cellDial(const char* number);
bool cellAnswer();
bool cellHangup();
bool cellDtmf(char digit);
bool cellSetVolume(int level);                // 0..5
bool cellSetMute(bool mute);
bool cellSmsSend(const char* number, const char* utf8_text);
bool cellDataConnect(bool up);

// One HTTP(S) request at a time over the modem's own stack.
// method: "GET" or "POST". body may be null. max_bytes caps the response.
bool cellHttpStart(const char* method, const char* url, const char* content_type,
                   const char* body, size_t body_len, size_t max_bytes);
bool cellHttpBusy();
// Blocking GET for another task (map tiles): waits up to timeout_ms, never on
// the UI thread. Returns the HTTP status or <0, and a PSRAM buffer the caller
// frees with heap_caps_free. No CELL_EV_HTTP event is raised for it.
int  cellHttpGetSync(const char* url, char** buf, size_t* len, size_t max_bytes, uint32_t timeout_ms);
// Modem on and a data context up: the 4G network can carry HTTP right now.
bool cellDataIsUp();
// After CELL_EV_HTTP: take ownership of the response buffer (PSRAM,
// free with heap_caps_free). Returns status (or <0) and sets *buf/*len.
int  cellHttpTake(char** buf, size_t* len);

// Names for logs / Lua.
const char* cellPowerName(CellPower p);
const char* cellSimName(CellSim s);
const char* cellRegName(CellReg r);
const char* cellCallName(CellCall c);
