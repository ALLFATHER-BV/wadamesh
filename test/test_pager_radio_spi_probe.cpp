// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual SPI probe with fake pins and a scripted radio response.
// g++ -std=c++17 -I test/pager_radio_fakes -I variants/lilygo_tlora_pager test/test_pager_radio_spi_probe.cpp -o pager_spi_test
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#define TLORA_PAGER_UNIFIED 1
#define P_LORA_NSS 36
#define P_LORA_RESET 47
#define P_LORA_BUSY 48
#include "../variants/lilygo_tlora_pager/PagerRadio.cpp"

uint32_t fakeMillis = 0;
bool fakeBusy = false;
unsigned fakeResets = 0;

static void resetFake() { fakeMillis = 0; fakeBusy = false; fakeResets = 0; }
static void append(std::vector<uint8_t>& to, std::initializer_list<uint8_t> v) {
  to.insert(to.end(), v.begin(), v.end());
}

int main() {
  const std::vector<uint8_t> sxRead = {0x1d, 0x03, 0x20, 0, 0, 0, 0, 0, 0, 0};
  {
    resetFake(); SPIClass spi;
    append(spi.responses, {0, 0, 0, 0, 'S', 'X', '1', '2', '6', '1'});
    append(spi.responses, {0, 0, 0, 0, 'S', 'X', '1', '2', '6', '1'});
    assert(pagerDetectRadio(spi) == PagerRadioFamily::SX1262);
    assert(fakeResets == 1 && spi.writes.size() == 2);
    assert(spi.writes[0] == sxRead && spi.writes[1] == sxRead);
  }
  for (uint8_t status : {uint8_t(0x04), uint8_t(0x06), uint8_t(0x00), uint8_t(0xff)}) {
    resetFake(); SPIClass spi;
    append(spi.responses, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    append(spi.responses, {0, 0, status, 1, 3, 4, 1});
    append(spi.responses, {0, 0, status, 1, 3, 4, 1});
    const auto found = pagerDetectRadio(spi);
    const bool valid = status == 0x04 || status == 0x06;
    assert(found == (valid ? PagerRadioFamily::LR1121 : PagerRadioFamily::None));
    assert(fakeResets == 2 && spi.writes[0] == sxRead);
    assert((spi.writes[1] == std::vector<uint8_t>{1, 1}));
    assert((spi.writes[2] == std::vector<uint8_t>{0, 0, 0, 0, 0}));
    assert(spi.writes.size() == (valid ? 5u : 3u));
  }
  {
    resetFake(); fakeBusy = true; SPIClass spi;
    assert(pagerDetectRadio(spi) == PagerRadioFamily::None);
    assert(fakeResets == 1 && spi.writes.empty() && fakeMillis == 261);
  }
  std::puts("test_pager_radio_spi_probe: OK");
}
