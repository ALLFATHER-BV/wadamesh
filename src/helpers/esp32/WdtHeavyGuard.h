#pragma once
// -----------------------------------------------------------------------------
// WdtHeavyGuard — suspend core 0's IDLE-task watchdog around a bounded-but-slow
// flash burst.
//
// On ESP32 a fragmenting SPIFFS write can trigger garbage collection: a
// multi-second flash operation that disables the CPU cache and starves the idle
// task long enough to trip the TASK watchdog. Seen in a coredump as an abort
// from task_wdt_isr while the loop task was inside spiffs_gc_clean during
// DataStore::saveContacts. Bracket such writes with a WdtHeavyGuard so a
// bounded-but-slow GC pass stalls the UI briefly instead of rebooting.
//
// Ref-counted and SHARED across translation units — the touch-UI history/backup
// saves (UITask.cpp) AND the core contact save (DataStore::saveContacts) use the
// same count. That matters because a backup import runs under a guard and itself
// calls saveContacts: one balanced count means the inner guard never re-arms the
// watchdog while the outer import is still writing. The counter lives in an
// inline function's static, so there is a single instance program-wide without a
// dedicated .cpp (and no ordering/link surprises).
//
// IMPORTANT: touch ONLY core 0's IDLE-task WDT. In the Arduino S3 build only core
// 0's idle task is subscribed to the task watchdog (CHECK_IDLE_TASK_CPU1 is off),
// so disableCore1WDT() merely fails while enableCore1WDT() *newly subscribes*
// IDLE1 — arming a core-1 watchdog that did not exist before, which a later slow
// flash burst then trips. Leaving core 1 alone keeps the Arduino default
// (loopTask not WDT-watched), so a slow write just stalls the UI briefly.
// -----------------------------------------------------------------------------
#if defined(ESP32)
#include <Arduino.h>   // disableCore0WDT / enableCore0WDT
#include <esp_task_wdt.h>
#include <atomic>

// The depth is shared: the UI/loop task and the core-1 history worker both take
// this guard around multi-second flash bursts, and they run at the same priority
// on the same core, so plain tick round-robin can preempt between the load and
// the store of a d++/--d. rebootDevice() makes that collision likely rather than
// theoretical -- it takes the guard immediately before waiting up to 9 s on the
// very worker that also takes it. A lost update either strands the depth above
// zero (IDLE0 never re-subscribed to the task watchdog for the rest of the
// session) or drops it to zero while a guard is still held (watchdog re-armed
// during the burst the guard exists to cover, which panics and loses the
// unflushed chat history rebootDevice() was written to save).
inline std::atomic<int>& _wdtHeavyDepth() { static std::atomic<int> d{0}; return d; }
inline void wdtHeavyBegin() {
  if (_wdtHeavyDepth().fetch_add(1, std::memory_order_acq_rel) == 0) disableCore0WDT();
}
inline void wdtHeavyEnd() {
  // Keep the original underflow guard rather than a bare fetch_sub:
  // doFactoryReset() calls wdtHeavyBegin() with no matching end (it reboots),
  // so the counter can legitimately stay elevated and an unbalanced end must
  // never drive it negative.
  std::atomic<int>& d = _wdtHeavyDepth();
  int prev = d.load(std::memory_order_relaxed);
  while (prev > 0 && !d.compare_exchange_weak(prev, prev - 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {}
  if (prev == 1) enableCore0WDT();
}

// Arduino's enableLoopWDT() subscribes loopTask even when it was not watched
// before. Preserve the caller's actual task-WDT state around slow filesystem
// work instead of silently arming a new watchdog on the way out. The guard can
// be constructed by a storage worker, so query Arduino's loopTask handle rather
// than `nullptr` (which means the current worker task to ESP-IDF).
#if defined(CONFIG_AUTOSTART_ARDUINO)
extern TaskHandle_t loopTaskHandle;
struct LoopWdtGuard {
  bool subscribed;
  LoopWdtGuard() : subscribed(loopTaskHandle != nullptr &&
                              esp_task_wdt_status(loopTaskHandle) == ESP_OK) {
    if (subscribed) disableLoopWDT();
  }
  ~LoopWdtGuard() {
    if (subscribed) enableLoopWDT();
  }
};
#else
// Tanmatsu and T-Display-P4 supply app_main() themselves. Their Arduino
// component has no loopTaskHandle or loop-WDT helpers to preserve.
struct LoopWdtGuard { LoopWdtGuard() {} ~LoopWdtGuard() {} };
#endif
#else
inline void wdtHeavyBegin() {}
inline void wdtHeavyEnd()   {}
struct LoopWdtGuard { LoopWdtGuard() {} ~LoopWdtGuard() {} };
#endif

struct WdtHeavyGuard { WdtHeavyGuard() { wdtHeavyBegin(); } ~WdtHeavyGuard() { wdtHeavyEnd(); } };
