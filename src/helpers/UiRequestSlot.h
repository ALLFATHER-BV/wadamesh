// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// The touch UI's single-flight request slot (status, telemetry, neighbours, regions,
// the admin app's binary requests) and the deferred guest login share one deadline:
// MyMesh::uiRequestTick (src/MyMesh.h) and the web build's stand-in
// (web/mesh/WebMesh.cpp loop) free a slot nobody answered after MAX_MS. Not every
// sender keeps a deadline of its own (telemetry auto-poll, the Home sensor widget,
// the contact sheet's Telemetry on a board without SD), so without this one lost
// reply held the slot until reboot. Every sender that does keep a deadline gives up
// well before (30 s at most). Host test: test/test_ui_request_slot.cpp.
namespace UiRequestSlot {
static constexpr uint32_t MAX_MS = 45000;
// millis() wraps after ~49.7 days: unsigned subtraction keeps the age right across it.
inline bool expired(uint32_t now_ms, uint32_t taken_at_ms) {
  return (uint32_t)(now_ms - taken_at_ms) >= MAX_MS;
}
}  // namespace UiRequestSlot
