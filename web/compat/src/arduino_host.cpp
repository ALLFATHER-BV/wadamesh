// SPDX-License-Identifier: GPL-3.0-or-later
// Arduino / ESP-IDF runtime pieces for the host: clocks, randomness, Serial, ESP, esp_timer.
#include <Arduino.h>
#include <esp_timer.h>
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wada_host.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
  #include <stdlib.h>   // arc4random_buf
#else
  #include <unistd.h>   // getentropy (glibc >= 2.25, musl, Emscripten)
#endif

namespace {
using Clock = std::chrono::steady_clock;
const Clock::time_point g_boot = Clock::now();
}

extern "C" unsigned long millis(void) {
  return (unsigned long)(uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_boot).count();
}
extern "C" unsigned long micros(void) {
  return (unsigned long)(uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_boot).count();
}
extern "C" int64_t esp_timer_get_time(void) {
  return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_boot).count();
}
#if defined(WADA_NO_THREADS)
// One thread (compat/src/freertos_coop.cpp): a task's delay yields to the others; the UI
// thread's runs them while it waits. Microsecond delays (bus timings) just spin.
extern "C" void delay(uint32_t ms) { wadaCoopSleep(ms); }
extern "C" void delayMicroseconds(uint32_t us) {
  const unsigned long t0 = micros();
  while (micros() - t0 < us) {}
}
extern "C" void yield(void) { if (wadaCoopInTask()) wadaCoopSleep(0); }
#else
extern "C" void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
extern "C" void delayMicroseconds(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
extern "C" void yield(void) { std::this_thread::yield(); }
#endif

extern "C" void esp_fill_random(void* buf, size_t len) {
#if defined(__APPLE__)
  arc4random_buf(buf, len);
#else
  uint8_t* p = (uint8_t*)buf;
  while (len) { const size_t n = len > 256 ? 256 : len; if (getentropy(p, n) != 0) break; p += n; len -= n; }
#endif
}
extern "C" uint32_t esp_random(void) { uint32_t v; esp_fill_random(&v, sizeof v); return v; }

// Arduino random(): seeded like the device (randomSeed), drawn from a PRNG.
static std::mt19937 g_rng(0x5eed);
void randomSeed(unsigned long seed) { if (seed) g_rng.seed((uint32_t)seed); }
long random(long howbig) {
  if (howbig <= 0) return 0;
  return (long)(g_rng() % (uint32_t)howbig);
}
long random(long howsmall, long howbig) {
  if (howsmall >= howbig) return howsmall;
  return random(howbig - howsmall) + howsmall;
}
long map(long x, long in_min, long in_max, long out_min, long out_max) {
  const long dividend = out_max - out_min, divisor = in_max - in_min;
  if (divisor == 0) return -1;
  return (x - in_min) * dividend / divisor + out_min;
}

extern "C" bool getLocalTime(struct tm* info, uint32_t) {
  time_t now = time(nullptr);
  localtime_r(&now, info);
  return info->tm_year > (2016 - 1900);
}
void tone(uint8_t, unsigned int, unsigned long) {}
void noTone(uint8_t) {}

// ---- Serial -> stdout ---------------------------------------------------------
HardwareSerial Serial(0);
HardwareSerial Serial1(1);
HardwareSerial Serial2(2);
size_t HardwareSerial::write(uint8_t c) {
  if (_num != 0) return 1;
  fputc(c, stdout);
  if (c == '\n') fflush(stdout);
  return 1;
}
size_t HardwareSerial::write(const uint8_t* buf, size_t size) {
  if (_num != 0) return size;
  fwrite(buf, 1, size, stdout);
  if (size && memchr(buf, '\n', size)) fflush(stdout);
  return size;
}
void HardwareSerial::flush() { if (_num == 0) fflush(stdout); }

// ---- ESP --------------------------------------------------------------------
EspClass ESP;
uint32_t EspClass::getCycleCount() { return (uint32_t)(esp_timer_get_time() * 240); }
uint64_t EspClass::getEfuseMac() {
  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
  uint64_t v = 0; for (int i = 5; i >= 0; --i) v = (v << 8) | m[i];
  return v;
}
// A stable, locally administered MAC (02:...), so the node keeps one identity per profile.
extern "C" esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t) {
  static const uint8_t k[6] = {0x02, 0x57, 0x41, 0x44, 0x41, 0x01};   // "WADA"
  memcpy(mac, k, 6);
  return ESP_OK;
}
void (*g_wada_restart_hook)() = nullptr;   // web/platform installs the real restart
void EspClass::restart() { esp_restart(); }
extern "C" void esp_restart(void) {
  fflush(stdout);
  if (g_wada_restart_hook) g_wada_restart_hook();
  _exit(0);
}

// ---- esp_timer callbacks on a host thread -------------------------------------
struct esp_timer {
  esp_timer_create_args_t args;
  std::mutex m;
  std::condition_variable cv;
  uint64_t gen = 0;
  bool active = false;
};
extern "C" esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
  auto* t = new esp_timer();
  t->args = *args;
  *out = t;
  return ESP_OK;
}
#if defined(WADA_NO_THREADS)
// One thread: a timer is a small task that sleeps, then calls back.
struct TimerRun { esp_timer_handle_t t; uint64_t us; bool periodic; uint64_t gen; };
static void timerTask(void* arg) {
  TimerRun r = *static_cast<TimerRun*>(arg);
  delete static_cast<TimerRun*>(arg);
  for (;;) {
    vTaskDelay((TickType_t)(r.us / 1000));
    if (r.t->gen != r.gen) break;
    if (!r.periodic) r.t->active = false;
    r.t->args.callback(r.t->args.arg);
    if (!r.periodic) break;
  }
  vTaskDelete(nullptr);
}
static void timerArm(esp_timer_handle_t t, uint64_t us, bool periodic) {
  t->gen++; t->active = true;
  xTaskCreate(timerTask, "esp_timer", 4096, new TimerRun{t, us, periodic, t->gen}, 1, nullptr);
}
#else
static void timerArm(esp_timer_handle_t t, uint64_t us, bool periodic) {
  uint64_t gen;
  { std::lock_guard<std::mutex> lk(t->m); gen = ++t->gen; t->active = true; }
  std::thread([t, us, periodic, gen] {
    for (;;) {
      std::unique_lock<std::mutex> lk(t->m);
      if (t->cv.wait_for(lk, std::chrono::microseconds(us), [&] { return t->gen != gen; })) return;
      if (!periodic) t->active = false;
      lk.unlock();
      t->args.callback(t->args.arg);
      if (!periodic) return;
    }
  }).detach();
}
#endif
extern "C" esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { timerArm(t, us, false); return ESP_OK; }
extern "C" esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) { timerArm(t, us, true); return ESP_OK; }
extern "C" esp_err_t esp_timer_stop(esp_timer_handle_t t) {
  { std::lock_guard<std::mutex> lk(t->m); t->gen++; t->active = false; }
  t->cv.notify_all();
  return ESP_OK;
}
extern "C" esp_err_t esp_timer_delete(esp_timer_handle_t t) { esp_timer_stop(t); /* leaked: a stopped thread may still hold it */ return ESP_OK; }
extern "C" bool esp_timer_is_active(esp_timer_handle_t t) { std::lock_guard<std::mutex> lk(t->m); return t->active; }

extern "C" const char* pathToFileName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}
extern "C" void esp_deep_sleep_start(void) { esp_restart(); }

// newlib's itoa/utoa (arduino-esp32's WString uses them; a host libc has neither).
extern "C" char* itoa(int value, char* str, int base) {
  if (base < 2 || base > 36) { *str = 0; return str; }
  char buf[34]; int i = 0; unsigned int v = (value < 0 && base == 10) ? (unsigned int)(-(long)value) : (unsigned int)value;
  do { int d = (int)(v % (unsigned)base); buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)base; } while (v);
  int j = 0; if (value < 0 && base == 10) str[j++] = '-';
  while (i) str[j++] = buf[--i];
  str[j] = 0;
  return str;
}
extern "C" char* utoa(unsigned int value, char* str, int base) {
  if (base < 2 || base > 36) { *str = 0; return str; }
  char buf[33]; int i = 0;
  do { int d = (int)(value % (unsigned)base); buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); value /= (unsigned)base; } while (value);
  int j = 0; while (i) str[j++] = buf[--i];
  str[j] = 0;
  return str;
}

#if defined(__EMSCRIPTEN__)
// The browser owns the clock: ClockFloorRTC::pushSystemClock's settimeofday() fails
// as it does for a normal user natively (Emscripten's libc has no settimeofday).
#include <errno.h>
#include <sys/time.h>
extern "C" int settimeofday(const struct timeval*, const struct timezone*) { errno = EPERM; return -1; }
#endif
