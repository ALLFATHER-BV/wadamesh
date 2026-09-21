// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(HAS_CARDKB) && defined(ESP32)

void cardKbPoll();
int  cardKbReadKey();
bool cardKbPresent();

#endif