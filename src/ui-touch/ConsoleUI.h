// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Console mode: a text front end with NO LVGL (CONSOLE_MODE.md).
//
// Draws straight to the panel through DisplayDriver and reads input from the
// board's own keyboard/touch drivers, so nothing here needs LVGL initialised.
// Commands go to MyMesh::runLocalCli() and its replies come back through
// MyMesh::setTerminalSink(), both of which already exist and are already what
// the LVGL Terminal app uses.
//
// Phase 1 (see CONSOLE_MODE.md): the front end itself. Booting into it and
// skipping LVGL entirely is Phase 2; this module is self-contained until then
// so it can be exercised without touching the graphical path.
#include "device_caps.h"

#if CAP_CONSOLE
class DisplayDriver;

// Take over the panel. Safe to call twice; a second call just re-renders.
void consoleBegin(DisplayDriver* d);
// Release it. Does not reboot or touch LVGL; the caller decides what happens next.
void consoleEnd();
bool consoleActive();

// Call every loop while active: polls touch, blinks the cursor, redraws when dirty.
void consoleLoop();

// Append one line of output. This is the MyMesh terminal-sink signature, so it
// can be handed to setTerminalSink() directly. Wraps long lines.
void consoleWriteLine(const char* line);
void consolePrintf(const char* fmt, ...);

// Feed one character from a hardware keyboard. '\n' submits, '\b' deletes.
// Returns true if the console consumed it.
bool consoleKey(int c);
#endif
