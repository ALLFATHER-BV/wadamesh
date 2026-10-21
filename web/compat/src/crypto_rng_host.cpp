// SPDX-License-Identifier: GPL-3.0-or-later
// rweather/Crypto's RNG on a host: the OS CSPRNG. Crypto's own RNG.cpp gathers entropy
// from board noise sources and EEPROM; Ed25519.cpp only needs rand() (key generation).
#include <RNG.h>
#include <esp_system.h>
RNGClass RNG;
RNGClass::RNGClass() : credits(0), firstSave(0), initialized(1), trngPending(0), timer(0), timeout(0), count(0), trngPosn(0) {}
RNGClass::~RNGClass() {}
void RNGClass::begin(const char*) {}
void RNGClass::rand(uint8_t* data, size_t len) { esp_fill_random(data, len); }
bool RNGClass::available(size_t) const { return true; }
void RNGClass::stir(const uint8_t*, size_t, unsigned int) {}
void RNGClass::loop() {}
