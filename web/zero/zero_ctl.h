// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// wadamesh-cli control socket and the on-message hook (zero_ctl.cpp).
#include <stdint.h>
#include <string>
struct ZeroCtlActions {
  std::string (*screenshot)() = nullptr;
  void (*hud)() = nullptr;
  void (*sac)() = nullptr;
};
void zeroCtlBegin(const ZeroCtlActions& actions);
void zeroCtlPoll();
void zeroCtlShutdown();
void zeroCtlSetLink(const std::string& text);
std::string zeroCtlLinkText();
