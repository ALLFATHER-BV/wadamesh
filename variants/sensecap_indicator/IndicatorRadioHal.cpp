// SPDX-License-Identifier: GPL-3.0-or-later
#include "IndicatorRadioHal.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "IndicatorIo.h"

namespace {

using indicator::expanderBit;
using indicator::isExpanderPin;
using indicator::kExpanderIntGpio;
namespace pins = indicator::pins;

constexpr uint32_t kDispatchStackBytes = 3072;
constexpr TickType_t kLostIntPoll = pdMS_TO_TICKS(50);
constexpr uint32_t kResetSettleMs = 20;        // without it begin() reports CHIP_NOT_FOUND

// /INT wake log. Several port-1 inputs float with no pull-up, so /INT can toggle
// without end, and each toggle costs an I2C read at high priority. One line for
// each of the first six 10 s windows (the first minute), then only while the rate
// stays above 500/s, at most once a minute. The windows are counted rather than
// timed from the start: each closes on the first wake after 10 s, so six of them
// run slightly past 60 s.
constexpr TickType_t kWakeWindow = pdMS_TO_TICKS(10000);
constexpr uint8_t kWakeVerboseWindows = 6;
constexpr TickType_t kWakeStormLogEvery = pdMS_TO_TICKS(60000);
constexpr uint32_t kWakeStormPerWindow = 500 * 10;

// Set in OUTPUT and OUTPUT_OPEN_DRAIN, clear in INPUT and its pull variants.
constexpr uint32_t kOutputModeBit = OUTPUT & ~INPUT;

TaskHandle_t s_dispatch = nullptr;

// Only wakes the task: the expander read needs I2C and the bus mutex.
void IRAM_ATTR onExpanderInt() {
  BaseType_t woken = pdFALSE;
  if (s_dispatch) vTaskNotifyGiveFromISR(s_dispatch, &woken);
  if (woken) portYIELD_FROM_ISR();
}

// Never touches RadioLib or SPI. The input read clears /INT and feeds the
// observer, which calls the radio callback on a DIO1 rise. The timeout re-reads
// anyway, in case /INT fell while the ISR was not yet attached or a change
// landed inside another reader's transfer.
void dispatchTask(void*) {
  TickType_t windowStart = xTaskGetTickCount();
  TickType_t lastStormLog = 0;
  bool stormLogged = false;
  uint8_t verboseLeft = kWakeVerboseWindows;
  uint32_t wakes = 0, timeouts = 0;
  for (;;) {
    // The return value is the number of ISR notifications since the last take;
    // zero means the 50 ms timeout expired.
    const uint32_t notified = ulTaskNotifyTake(pdTRUE, kLostIntPoll);
    if (notified) wakes += notified;
    else ++timeouts;
    {
      IndicatorIo::BusLock lock;
      // Both input bytes: /INT clears only when the port that changed is read, so a
      // port-0-only read would leave a port-1 change holding GPIO42 low for good.
      // A port-1 pin read covers both bytes and still reports port 0 to the observer.
      bool strap = false;
      IndicatorIo::chip().readPin(pins::kRadioStrap, strap);
    }
    const TickType_t now = xTaskGetTickCount();
    if (now - windowStart < kWakeWindow) continue;
    const bool early = verboseLeft > 0;
    if (early) --verboseLeft;
    const bool storm = wakes > kWakeStormPerWindow &&
                       (!stormLogged || now - lastStormLog >= kWakeStormLogEvery);
    if (early || storm) {
      if (!early) {
        stormLogged = true;
        lastStormLog = now;
      }
      Serial.printf("[indicator] /INT wakes=%lu/10s timeouts=%lu\n", (unsigned long)wakes,
                    (unsigned long)timeouts);
    }
    wakes = 0;
    timeouts = 0;
    windowStart = now;
  }
}

}  // namespace

IndicatorRadioHal::IndicatorRadioHal(SPIClass& spi, uint32_t spiFreq)
    : ArduinoHal(spi, SPISettings(spiFreq, MSBFIRST, SPI_MODE0)) {}

// Module::init() runs this from SX126x::begin(), before any pin call. FSPI is
// already running on GPIO41/47/48: std_init starts it before begin(), and this
// HAL never calls spi.begin() itself, which would move FSPI to its default pins
// (RGB data lines on this board). The observer goes in here rather than
// in the constructor: the expander object lives in another translation unit and
// may not be constructed yet at static init time.
void IndicatorRadioHal::init() {
  ArduinoHal::init();
  IndicatorIo::BusLock lock;
  IndicatorIo::chip().setPort0Observer(&IndicatorRadioHal::onPort0, this);
}

// Runs inside a port-0 read, so under the BusLock the reader holds.
void IndicatorRadioHal::onPort0(uint8_t value, void* ctx) {
  static_cast<IndicatorRadioHal*>(ctx)->_dio1.onPort0(value);
}

// An output keeps its current latch: the NSS high set at boot must survive
// RadioLib's pinMode(cs, OUTPUT).
void IndicatorRadioHal::pinMode(uint32_t pin, uint32_t mode) {
  if (!isExpanderPin(pin)) {
    if (pin != RADIOLIB_NC) ::pinMode(pin, mode);
    return;
  }
  const uint8_t bit = expanderBit(pin);
  auto& io = IndicatorIo::chip();
  IndicatorIo::BusLock lock;
  if (mode & kOutputModeBit) {
    io.setOutput(bit, (io.output(bit >> 3) & (1U << (bit & 7))) != 0);
  } else {
    io.setInput(bit);
  }
}

void IndicatorRadioHal::digitalWrite(uint32_t pin, uint32_t value) {
  if (!isExpanderPin(pin)) {
    if (pin != RADIOLIB_NC) ::digitalWrite(pin, value);
    return;
  }
  const uint8_t bit = expanderBit(pin);
  {
    IndicatorIo::BusLock lock;
    IndicatorIo::chip().setOutput(bit, value != 0);
  }
  if (bit == pins::kLoraReset && value) ::delay(kResetSettleMs);
}

// A BUSY read is a port-0 read, so it samples DIO1 too: a rise that lands while
// RadioLib waits on BUSY fires the callback from here, and the read that clears
// /INT is never a lost edge.
uint32_t IndicatorRadioHal::digitalRead(uint32_t pin) {
  if (!isExpanderPin(pin)) return pin == RADIOLIB_NC ? 0 : ::digitalRead(pin);
  bool high = false;
  IndicatorIo::BusLock lock;
  if (!IndicatorIo::chip().readPin(expanderBit(pin), high)) return LOW;
  return high ? HIGH : LOW;
}

// Only DIO1 has an edge path, and Dio1Edge only knows rising edges, which is
// what SX126x::setDio1Action asks for.
void IndicatorRadioHal::attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void),
                                        uint32_t mode) {
  if (!isExpanderPin(interruptNum)) {
    if (interruptNum != RADIOLIB_NC) ::attachInterrupt(interruptNum, interruptCb, mode);
    return;
  }
  if (expanderBit(interruptNum) != pins::kLoraDio1) return;
  {
    IndicatorIo::BusLock lock;
    _dio1.arm(interruptCb);
  }
  startDispatch();
}

// The dispatch task keeps reading while disarmed, so Dio1Edge keeps tracking the
// level and a later re-arm with DIO1 already high is no edge.
void IndicatorRadioHal::detachInterrupt(uint32_t interruptNum) {
  if (!isExpanderPin(interruptNum)) {
    if (interruptNum != RADIOLIB_NC) ::detachInterrupt(interruptNum);
    return;
  }
  if (expanderBit(interruptNum) != pins::kLoraDio1) return;
  IndicatorIo::BusLock lock;
  _dio1.disarm();
}

uint32_t IndicatorRadioHal::pinToInterrupt(uint32_t pin) {
  return isExpanderPin(pin) ? pin : digitalPinToInterrupt(pin);
}

void IndicatorRadioHal::startDispatch() {
  if (s_dispatch) return;
  if (xTaskCreatePinnedToCore(dispatchTask, "ind_dio1", kDispatchStackBytes, nullptr,
                              configMAX_PRIORITIES - 2, &s_dispatch,
                              ARDUINO_RUNNING_CORE) != pdPASS) {
    s_dispatch = nullptr;
    Serial.println("[indicator] radio hal: dispatch task not created");
    return;
  }
  ::pinMode(kExpanderIntGpio, INPUT_PULLUP);
  ::attachInterrupt(kExpanderIntGpio, onExpanderInt, FALLING);
  Serial.printf("[indicator] radio hal: dio1 via expander /INT on GPIO%u\n",
                (unsigned)kExpanderIntGpio);
}
