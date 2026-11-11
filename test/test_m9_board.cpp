// SPDX-License-Identifier: GPL-3.0-or-later
// Host unit tests for ThinkNode M9 board, charger/ADC, compass, IMU, and GPS.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#define M9_HOST_TEST 1
#define ESP32 1
#define HAS_THINKNODE_M9 1
#define HAS_M9_COMPASS 1
#define HAS_M9_IMU 1
#define HAS_M9_KEYBOARD 1

#define PIN_PERIPH_POWER 18
#define PIN_TFT_BL_EN 17
#define PIN_USB_5V_SENSE 1
#define PIN_CHG_DONE 8
#define PIN_KB_WAKEUP 12
#define THINKNODE_M9_BUZZER_PIN 9
#define PIN_KEY_LED 46
#define PIN_VBAT_READ 13
#define PIN_TFT_CS 16
#define P_LORA_NSS 39
#define PIN_SD_CS 48
#define P_LORA_SCLK 40
#define P_LORA_MISO 38
#define P_LORA_MOSI 47
#define PIN_GPS_STANDBY 10
#define PIN_GPS_EN 11
#define PIN_GPS_RESET 5
#define PIN_GPS_RX 3
#define PIN_GPS_TX 2

#include "m9_hw_fakes/Arduino.h"
#include "m9_hw_fakes/SPI.h"
#include "m9_hw_fakes/Wire.h"

uint32_t g_test_ms = 1000;
int g_pin_mode[64] = {};
int g_pin_level[64] = {};
std::vector<PinTransition> g_pin_writes;
std::deque<uint32_t> g_adc_mv_queue;
uint32_t g_adc_default_mv = 2000;
int g_adc_resolution = 0;
int g_adc_attenuation = -1;
uint32_t g_usb_serial_jtag_conf0_reg = (1u << 14);

Stream Serial;
Stream Serial1;
TwoWire Wire;
TwoWire Wire1;
SPIClass SPI;
FakeRadioDriver radio_driver;
FakeSensorManager sensors;

static uint8_t s_kb_backlight = 255;
void m9KeyboardBegin() {}
void m9KeyboardPoll() {}
int m9KeyboardReadKey() { return -1; }
bool m9KeyboardSetBacklight(uint8_t duty) {
  s_kb_backlight = duty;
  return true;
}
void m9KeyboardSetFoundCallback(void (*)(uint8_t)) {}

uint32_t touchPrefsGetGpsBaud(uint32_t fallback) { return fallback; }
bool touchPrefsGpsBaudChosen() { return false; }
bool touchPrefsSetGpsBaud(uint32_t) { return true; }
void touchPrefsSetGpsBaudFound(uint32_t) {}

#include "../variants/thinknode_m9/M9Board.h"
#include "../variants/thinknode_m9/M9Compass.h"
#include "../variants/thinknode_m9/M9Imu.h"
#include "../src/helpers/WadaNmeaLocationProvider.h"

static bool nearf(float a, float b, float eps = 0.01f) {
  return std::fabs(a - b) <= eps;
}

static void testChargerAndBattery() {
  ThinkNodeM9Board board;
  g_adc_default_mv = 2050;
  g_adc_mv_queue.clear();

  board.begin();

  assert(g_pin_mode[PIN_USB_5V_SENSE] == INPUT);
  assert(g_pin_mode[PIN_CHG_DONE] == INPUT_PULLUP);
  assert(g_adc_attenuation == ADC_11db);
  assert(g_pin_mode[PIN_TFT_CS] == OUTPUT && g_pin_level[PIN_TFT_CS] == HIGH);
  assert(g_pin_mode[P_LORA_NSS] == OUTPUT && g_pin_level[P_LORA_NSS] == HIGH);
  assert(g_pin_mode[PIN_SD_CS] == OUTPUT && g_pin_level[PIN_SD_CS] == HIGH);
  assert(Wire.clock_hz == 400000 && Wire.timeout_ms == 20);

  // Discharging: VBUS low
  g_pin_level[PIN_USB_5V_SENSE] = LOW;
  g_pin_level[PIN_CHG_DONE] = HIGH;
  assert(board.externalPowerPresent() == 0);
  assert(board.isCharging() == 0);
  assert(!board.isExternalPowered());

  // Charging: VBUS high, DONE# open-drain pulled high
  g_pin_level[PIN_USB_5V_SENSE] = HIGH;
  g_pin_level[PIN_CHG_DONE] = HIGH;
  assert(board.externalPowerPresent() == 1);
  assert(board.isCharging() == 1);
  assert(board.isExternalPowered());

  // Charge complete: VBUS high, DONE# pulled low by U6
  g_pin_level[PIN_USB_5V_SENSE] = HIGH;
  g_pin_level[PIN_CHG_DONE] = LOW;
  assert(board.externalPowerPresent() == 1);
  assert(board.isCharging() == 2);
  assert(board.isExternalPowered());

  for (int i = 0; i < 8; ++i) g_adc_mv_queue.push_back(2050);
  assert(board.getBattMilliVolts() == 4100);

  // Reject ADC2 contention samples (< 1250 mV at divider)
  g_adc_mv_queue = {0, 1900, 0, 1900, 1100, 1900, 0, 1900};
  assert(board.getBattMilliVolts() == 3800);

  // Retain cached reading if all 8 samples fail
  g_adc_mv_queue = {0, 0, 0, 0, 0, 0, 0, 0};
  assert(board.getBattMilliVolts() == 3800);

  g_adc_mv_queue = {1200, 1249, 800, 500, 1000, 1240, 0, 900};
  assert(board.getBattMilliVolts() == 3800);

  M9BatterySmoother smoother;
  assert(smoother.smooth(3800, 0, 0, 1000) == 3800);
  assert(smoother.smooth(3820, 0, 0, 5000) == 3800);
  assert(nearf(smoother.ema, 3803.0f, 0.5f));
  assert(smoother.smooth(3950, 1, 1, 6000) == 3950);
  assert(smoother.smooth(4350, 1, 2, 10000) == 4350);
  assert(smoother.smooth(4100, 0, 0, 12000) == 4100);
  assert(smoother.smooth(4000, 0, 0, 14000) == 4100);
  assert(smoother.smooth(4000, 0, 0, 15000) == 4000);
}

static void testCompassHeading() {
  assert(nearf(m9CompassHeadingFromBody(+1.0f,  0.0f),   0.0f));
  assert(nearf(m9CompassHeadingFromBody( 0.0f, -1.0f),  90.0f));
  assert(nearf(m9CompassHeadingFromBody(-1.0f,  0.0f), 180.0f));
  assert(nearf(m9CompassHeadingFromBody( 0.0f, +1.0f), 270.0f));

  {
    std::ifstream lua_in("deploy/apps/gpscompass/1.0/gpscompass.lua");
    assert(lua_in.good());
    std::ostringstream ss;
    ss << lua_in.rdbuf();
    const std::string lua_src = ss.str();
    assert(lua_src.find("math.atan(-by, bx)") != std::string::npos);
    assert(lua_src.find("math.atan(by, bx)") == std::string::npos);
  }

  g_test_ms = 2000;
  Wire.qmc = FakeQmc6309{};
  m9CompassBegin(Wire);
  assert(m9CompassPresent());
  assert(Wire.qmc.ctrl1 == 0x40);

  Wire.qmc.configured_wake_drdy_delay_polls = 2;
  Wire.qmc.clear_drdy_on_status_read = true;
  Wire.qmc.reg_read_trace.clear();
  Wire.qmc.setFieldMilliGauss(0, 500, -400, false);

  float hdg = -1.0f;
  assert(m9CompassReadHeadingDeg(&hdg));
  assert(nearf(hdg, 0.0f));
  assert(Wire.qmc.ctrl1 == 0x41);
  assert(Wire.qmc.status_polls_before_first_data_read == 3);
  assert(Wire.qmc.data_reads_since_wake == 1);
  assert((Wire.qmc.reg_read_trace == std::vector<uint8_t>{0x09, 0x09, 0x09, 0x01}));

  g_test_ms += 20;
  Wire.qmc.setFieldMilliGauss(500, 0, -400, false);
  assert(m9CompassReadHeadingDeg(&hdg) && nearf(hdg, 90.0f));

  g_test_ms += 20;
  Wire.qmc.setFieldMilliGauss(0, -500, -400, false);
  assert(m9CompassReadHeadingDeg(&hdg) && nearf(hdg, 180.0f));

  g_test_ms += 20;
  Wire.qmc.setFieldMilliGauss(-500, 0, -400, false);
  assert(m9CompassReadHeadingDeg(&hdg) && nearf(hdg, 270.0f));

  g_test_ms += 2500;
  m9CompassIdleTick();
  assert(Wire.qmc.ctrl1 == 0x40);
  Wire.qmc.configured_wake_drdy_delay_polls = 3;
  Wire.qmc.reg_read_trace.clear();
  Wire.qmc.setFieldMilliGauss(500, 0, -400, false);
  assert(m9CompassReadHeadingDeg(&hdg) && nearf(hdg, 90.0f));
  assert(Wire.qmc.status_polls_before_first_data_read == 4);
  assert((Wire.qmc.reg_read_trace == std::vector<uint8_t>{0x09, 0x09, 0x09, 0x09, 0x01}));
}

static void testImuWakeCache() {
  g_test_ms = 10000;
  Wire.qmi = FakeQmi8658{};
  const uint32_t t_before_begin = g_test_ms;
  m9ImuBegin(Wire);
  assert(m9ImuPresent());
  assert((g_test_ms - t_before_begin) <= 20);
  assert(Wire.qmi.ctrl7 == 0x00 && Wire.qmi.ctrl1 == 0x41);

  Wire.qmi.setAccelG(0.125f, -0.250f, 0.96875f, true);
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  assert(!m9ImuReadAccelG(&ax, &ay, &az));
  assert(Wire.qmi.ctrl1 == 0x40 && Wire.qmi.ctrl7 == 0x01);

  g_test_ms += 60;
  assert(m9ImuReadAccelG(&ax, &ay, &az));
  assert(nearf(ax, 0.125f) && nearf(ay, -0.250f) && nearf(az, 0.96875f));
  assert(Wire.qmi.data_reads == 1);

  g_test_ms += 2500;
  m9ImuIdleTick();
  assert(Wire.qmi.ctrl7 == 0x00 && Wire.qmi.ctrl1 == 0x41);

  Wire.qmi.setAccelG(0.500f, -0.500f, 0.7071f, true);
  ax = ay = az = 0.0f;
  assert(m9ImuReadAccelG(&ax, &ay, &az));
  assert(nearf(ax, 0.125f) && nearf(ay, -0.250f) && nearf(az, 0.96875f));
  assert(Wire.qmi.ctrl1 == 0x40 && Wire.qmi.ctrl7 == 0x01);
  assert(Wire.qmi.data_reads == 1);

  for (uint32_t dt : {10u, 20u, 24u}) {
    g_test_ms += dt;
    ax = ay = az = 0.0f;
    assert(m9ImuReadAccelG(&ax, &ay, &az));
    assert(nearf(ax, 0.125f) && nearf(ay, -0.250f) && nearf(az, 0.96875f));
    assert(Wire.qmi.data_reads == 1);
  }

  g_test_ms += 1;
  assert(m9ImuReadAccelG(&ax, &ay, &az));
  assert(nearf(ax, 0.500f, 0.002f) && nearf(ay, -0.500f, 0.002f) && nearf(az, 0.7071f, 0.002f));
  assert(Wire.qmi.data_reads == 2);
}

static int s_post_enable_calls = 0;
static void fakeM9GpsWakePulse() {
  ++s_post_enable_calls;
  pinMode(PIN_GPS_STANDBY, OUTPUT);
  digitalWrite(PIN_GPS_STANDBY, HIGH);
  delay(5);
  digitalWrite(PIN_GPS_STANDBY, LOW);
  delay(50);
  pinMode(PIN_GPS_STANDBY, INPUT);
}

static void testGpsResetAndStop() {
  s_post_enable_calls = 0;
  g_pin_writes.clear();
  Serial1 = Stream{};

  mesh::RTCClock clk;
  WadaNmeaLocationProvider gps(
      Serial1, &clk,
      PIN_GPS_RESET, PIN_GPS_EN,
      PIN_GPS_RX, PIN_GPS_TX,
      115200);
  gps.setPostEnableHook(fakeM9GpsWakePulse);

  assert(g_pin_level[PIN_GPS_EN] == HIGH);

  gps.begin();
  assert(gps.isEnabled());
  assert(g_pin_level[PIN_GPS_EN] == LOW);
  assert(Serial1.last_set_rx == PIN_GPS_RX && Serial1.last_set_tx == PIN_GPS_TX);
  assert(s_post_enable_calls == 1);
  assert(g_pin_mode[PIN_GPS_STANDBY] == INPUT);

  g_pin_writes.clear();
  const uint32_t t0 = g_test_ms;
  gps.reset();
  assert(s_post_enable_calls == 2);
  assert(g_pin_level[PIN_GPS_RESET] == LOW);
  assert(g_pin_writes.size() >= 2);
  assert(g_pin_writes[0].pin == PIN_GPS_RESET && g_pin_writes[0].level == HIGH);
  assert(g_pin_writes[1].pin == PIN_GPS_RESET && g_pin_writes[1].level == LOW);
  assert((g_pin_writes[1].at_ms - t0) == 10);

  pinMode(PIN_GPS_TX, OUTPUT);
  pinMode(PIN_GPS_RX, OUTPUT);
  pinMode(PIN_GPS_STANDBY, OUTPUT);

  gps.stop();
  assert(!gps.isEnabled());
  assert(g_pin_level[PIN_GPS_EN] == HIGH);
  assert(g_pin_mode[PIN_GPS_RESET] == OUTPUT && g_pin_level[PIN_GPS_RESET] == LOW);
  assert(g_pin_mode[PIN_GPS_TX] == INPUT);
  assert(g_pin_mode[PIN_GPS_RX] == INPUT);
  assert(g_pin_mode[PIN_GPS_STANDBY] == INPUT);
}

int main() {
  testChargerAndBattery();
  testCompassHeading();
  testImuWakeCache();
  testGpsResetAndStop();
  std::printf("test_m9_board: OK\n");
  return 0;
}
