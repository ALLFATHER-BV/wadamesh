// SPDX-License-Identifier: GPL-3.0-or-later
#include "IndicatorDisplay.h"

// Placeholder until the panel driver lands: the target links and boots to a
// blank screen.

bool IndicatorDisplay::begin() { return false; }

void IndicatorDisplay::turnOn() {}
void IndicatorDisplay::turnOff() {}
void IndicatorDisplay::clear() {}
void IndicatorDisplay::startFrame(ColorVal) {}
void IndicatorDisplay::setTextSize(int) {}
void IndicatorDisplay::setColor(ColorVal) {}
void IndicatorDisplay::setCursor(int, int) {}
void IndicatorDisplay::print(const char*) {}
void IndicatorDisplay::fillRect(int, int, int, int) {}
void IndicatorDisplay::drawRect(int, int, int, int) {}
void IndicatorDisplay::drawXbm(int, int, const uint8_t*, int, int) {}
uint16_t IndicatorDisplay::getTextWidth(const char*) { return 0; }
void IndicatorDisplay::endFrame() {}

void IndicatorDisplay::writePixelsRGB565(int, int, int, int, const uint16_t*) {}
void IndicatorDisplay::setDisplayRotation(uint8_t) {}
void IndicatorDisplay::setBrightness(uint8_t) {}
