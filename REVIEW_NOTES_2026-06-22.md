## Code Review Notes

Date: 2026-06-22

### Findings

1. `tcp on` does not restart the TCP server after `tcp off`.
   - `src/helpers/esp32/MultiTransportCompanionInterface.cpp:55`
   - `src/MyMesh.cpp:645`
   - `src/ui-touch/UITask.cpp:4166`
   - `src/main.cpp:619`

2. OTA UI guard for dual-slot partition support is implemented but unused.
   - `src/ui-touch/UITask.cpp:4932`
   - `src/ui-touch/UITask.cpp:18788`

3. Build warnings are globally suppressed with `-w`, which can hide OTA/network regressions.
   - `platformio.ini:20`
   - `platformio.ini:224`

### OTA Section

- Mesh command handling: `src/MyMesh.cpp:704`
- HTTP OTA URL path: `src/MyMesh.cpp:729`
- Companion transport suspend/restore during OTA: `src/helpers/esp32/MultiTransportCompanionInterface.cpp:142`
- UI update status/button: `src/ui-touch/UITask.cpp:2389`
- OTA currently paused in UI: `src/ui-touch/UITask.cpp:2469`
- Deferred post-OTA reboot poll: `src/main.cpp:674`
