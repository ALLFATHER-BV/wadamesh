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
#include <stdint.h>
#include <stddef.h>

#if CAP_CONSOLE
class DisplayDriver;

// Take over the panel. Safe to call twice; a second call just re-renders.
void consoleBegin(DisplayDriver* d);
// Release it. Does not reboot or touch LVGL; the caller decides what happens next.
void consoleEnd();
bool consoleActive();

// Call every loop while active: polls touch, blinks the cursor, redraws when dirty.
void consoleLoop();

// Line colours. Kept as a small named set rather than raw values so the console
// reads as one thing: the meaning of a line picks the colour, not the caller's
// taste.
enum ConsoleColor : uint8_t {
  CC_TEXT = 0,   // normal output
  CC_DIM,        // secondary / hints
  CC_ECHO,       // the command you typed
  CC_OK,         // success
  CC_WARN,       // warnings, refusals
  CC_ERR,        // failures
  CC_CHAN,       // the channel a message arrived on
  CC_SENDER,     // who sent it
  CC_DM,         // a direct message (its sender)
  CC_HEAD,       // section headings, the banner
};

// Append one line of output. This is the MyMesh terminal-sink signature, so it
// can be handed to setTerminalSink() directly. Wraps long lines.
void consoleWriteLine(const char* line);
void consoleWriteLineC(uint8_t colour, const char* line);
void consolePrintf(const char* fmt, ...);
void consolePrintfC(uint8_t colour, const char* fmt, ...);
// A line in up to THREE colours: [0,len1) in c1, [len1,len1+len2) in c2, the
// remainder as normal text. That is exactly the shape of an incoming message:
// the channel, then who said it, then what they said, each its own colour.
void consoleWriteLineSeg(uint8_t c1, int len1, uint8_t c2, int len2, const char* line);
void consoleWriteLineSplit(uint8_t colour, int split, const char* line);   // two-tone shorthand

// The login banner: ASCII mark, who this node is, and the quick-launch menu.
void consoleBanner(const char* node_name, const char* version);
void consoleSetNodeName(const char* n);
// Scroll the view. +1 = one line back into history, -1 = one line towards live.
void consoleScroll(int delta);

// Feed one character from a hardware keyboard. '\n' submits, '\b' deletes.
// Returns true if the console consumed it.
bool consoleKey(int c);
#endif
