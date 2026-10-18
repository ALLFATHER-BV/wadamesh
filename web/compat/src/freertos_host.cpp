// SPDX-License-Identifier: GPL-3.0-or-later
// FreeRTOS shim implementation (see web/compat/freertos/FreeRTOS.h for what is faked):
// tasks are threads. The no-threads browser build (WADA_NO_THREADS) uses
// freertos_coop.cpp instead: the same API on cooperative fibers.
#if !defined(WADA_NO_THREADS)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(__EMSCRIPTEN__)
#include <pthread.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
const Clock::time_point g_boot = Clock::now();
Clock::time_point deadline(TickType_t wait) {
  return Clock::now() + std::chrono::milliseconds(wait);
}
struct TaskExit {};   // thrown by vTaskDelete(nullptr), caught by the thread trampoline
}  // namespace

struct WadaHostTask {
  std::string name;
  std::mutex m;
  std::condition_variable cv;
  uint32_t notify = 0;
};

static thread_local WadaHostTask* t_self = nullptr;
static WadaHostTask g_main_task{"loopTask"};

BaseType_t xPortGetCoreID() { return t_self ? 0 : 1; }   // the UI loop reports core 1, like Arduino

static void runTask(WadaHostTask* t, TaskFunction_t fn, void* param) {
  t_self = t;
  try { fn(param); } catch (const TaskExit&) {}
  // A FreeRTOS task function must not return; ours may (it simply ends the thread).
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t, void* param,
                                   UBaseType_t, TaskHandle_t* out, BaseType_t) {
  auto* t = new WadaHostTask();
  t->name = name ? name : "task";
  if (out) *out = t;
  std::thread(runTask, t, fn, param).detach();
  return pdPASS;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char* name, uint32_t depth, void* param,
                                           UBaseType_t prio, StackType_t*, StaticTask_t*, BaseType_t core) {
  TaskHandle_t h = nullptr;
  xTaskCreatePinnedToCore(fn, name, depth, param, prio, &h, core);
  return h;
}
void vTaskDelete(TaskHandle_t t) {
  if (t == nullptr || t == t_self) {
#if defined(__EMSCRIPTEN__)
    // The browser build has no C++ exceptions (smaller, faster wasm): end the pthread
    // directly. Like vTaskDelete(NULL) on FreeRTOS, the task's locals are not unwound.
    if (t_self) pthread_exit(nullptr);
#else
    if (t_self) throw TaskExit{};
#endif
    fprintf(stderr, "[freertos] vTaskDelete(NULL) from the main thread ignored\n");
    return;
  }
  fprintf(stderr, "[freertos] vTaskDelete(%s) of another task is not supported on the host\n",
          t->name.c_str());
}
void vTaskDelay(TickType_t ticks) {
  if (ticks == 0) { std::this_thread::yield(); return; }
  std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}
void vTaskDelayUntil(TickType_t* prev, TickType_t inc) {
  TickType_t target = *prev + inc;
  TickType_t now = xTaskGetTickCount();
  if ((int32_t)(target - now) > 0) vTaskDelay(target - now);
  *prev = target;
}
TickType_t xTaskGetTickCount() {
  return (TickType_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_boot).count();
}
TaskHandle_t xTaskGetCurrentTaskHandle() { return t_self ? t_self : &g_main_task; }
const char* pcTaskGetName(TaskHandle_t t) {
  if (!t) t = xTaskGetCurrentTaskHandle();
  return t->name.c_str();
}
BaseType_t xTaskNotifyGive(TaskHandle_t t) {
  if (!t) return pdFAIL;
  { std::lock_guard<std::mutex> lk(t->m); t->notify++; }
  t->cv.notify_all();
  return pdPASS;
}
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait) {
  WadaHostTask* t = xTaskGetCurrentTaskHandle();
  std::unique_lock<std::mutex> lk(t->m);
  auto ready = [t] { return t->notify != 0; };
  if (wait == portMAX_DELAY) t->cv.wait(lk, ready);
  else t->cv.wait_until(lk, deadline(wait), ready);
  uint32_t v = t->notify;
  if (v) t->notify = clear_on_exit ? 0 : v - 1;
  return v;
}
BaseType_t xTaskNotify(TaskHandle_t t, uint32_t value, eNotifyAction action) {
  if (!t) return pdFAIL;
  {
    std::lock_guard<std::mutex> lk(t->m);
    switch (action) {
      case eSetBits: t->notify |= value; break;
      case eIncrement: t->notify++; break;
      case eSetValueWithOverwrite: case eSetValueWithoutOverwrite: t->notify = value; break;
      default: break;
    }
  }
  t->cv.notify_all();
  return pdPASS;
}
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t* value, TickType_t wait) {
  WadaHostTask* t = xTaskGetCurrentTaskHandle();
  std::unique_lock<std::mutex> lk(t->m);
  t->notify &= ~clear_on_entry;
  auto ready = [t] { return t->notify != 0; };
  bool ok = (wait == portMAX_DELAY) ? (t->cv.wait(lk, ready), true) : t->cv.wait_until(lk, deadline(wait), ready);
  if (value) *value = t->notify;
  if (ok) t->notify &= ~clear_on_exit;
  return ok ? pdTRUE : pdFALSE;
}

// ---- semaphores -------------------------------------------------------------
struct WadaHostSem {
  std::mutex m;
  std::condition_variable cv;
  UBaseType_t count, max;
  bool recursive;
  WadaHostTask* owner = nullptr;
  UBaseType_t depth = 0;
};
SemaphoreHandle_t wadaSemCreate(UBaseType_t max_count, UBaseType_t initial, bool recursive) {
  auto* s = new WadaHostSem();
  s->count = initial; s->max = max_count; s->recursive = recursive;
  return s;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
  if (!s) return pdFAIL;
  std::unique_lock<std::mutex> lk(s->m);
  WadaHostTask* me = xTaskGetCurrentTaskHandle();
  if (s->recursive && s->owner == me && s->depth) { s->depth++; return pdTRUE; }
  auto ready = [s] { return s->count > 0; };
  bool ok = (wait == portMAX_DELAY) ? (s->cv.wait(lk, ready), true) : s->cv.wait_until(lk, deadline(wait), ready);
  if (!ok) return pdFALSE;
  s->count--;
  if (s->recursive) { s->owner = me; s->depth = 1; }
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  if (!s) return pdFAIL;
  {
    std::lock_guard<std::mutex> lk(s->m);
    if (s->recursive && s->depth > 1) { s->depth--; return pdTRUE; }
    if (s->count >= s->max) return pdFALSE;
    s->count++;
    s->owner = nullptr; s->depth = 0;
  }
  s->cv.notify_one();
  return pdTRUE;
}
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s) {
  if (!s) return 0;
  std::lock_guard<std::mutex> lk(s->m);
  return s->count;
}
void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }

// ---- queues -----------------------------------------------------------------
struct WadaHostQueue {
  std::mutex m;
  std::condition_variable cv_items, cv_space;
  UBaseType_t length, item_size;
  std::deque<std::vector<uint8_t>> items;
};
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
  auto* q = new WadaHostQueue();
  q->length = length; q->item_size = item_size;
  return q;
}
BaseType_t xQueueGenericSend(QueueHandle_t q, const void* item, TickType_t wait, BaseType_t pos) {
  if (!q) return pdFAIL;
  std::unique_lock<std::mutex> lk(q->m);
  std::vector<uint8_t> v((const uint8_t*)item, (const uint8_t*)item + q->item_size);
  if (pos == queueOVERWRITE) {
    q->items.clear();
    q->items.push_back(std::move(v));
  } else {
    auto space = [q] { return q->items.size() < q->length; };
    bool ok = (wait == portMAX_DELAY) ? (q->cv_space.wait(lk, space), true) : q->cv_space.wait_until(lk, deadline(wait), space);
    if (!ok) return errQUEUE_FULL;
    if (pos == queueSEND_TO_FRONT) q->items.push_front(std::move(v)); else q->items.push_back(std::move(v));
  }
  lk.unlock();
  q->cv_items.notify_one();
  return pdPASS;
}
static BaseType_t queueTake(QueueHandle_t q, void* out, TickType_t wait, bool remove) {
  if (!q) return pdFAIL;
  std::unique_lock<std::mutex> lk(q->m);
  auto has = [q] { return !q->items.empty(); };
  bool ok = (wait == portMAX_DELAY) ? (q->cv_items.wait(lk, has), true) : q->cv_items.wait_until(lk, deadline(wait), has);
  if (!ok) return errQUEUE_EMPTY;
  memcpy(out, q->items.front().data(), q->item_size);
  if (remove) { q->items.pop_front(); lk.unlock(); q->cv_space.notify_one(); }
  return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t q, void* out, TickType_t wait) { return queueTake(q, out, wait, true); }
BaseType_t xQueuePeek(QueueHandle_t q, void* out, TickType_t wait) { return queueTake(q, out, wait, false); }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
  if (!q) return 0;
  std::lock_guard<std::mutex> lk(q->m);
  return (UBaseType_t)q->items.size();
}
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q) {
  if (!q) return 0;
  std::lock_guard<std::mutex> lk(q->m);
  return q->length - (UBaseType_t)q->items.size();
}
BaseType_t xQueueReset(QueueHandle_t q) {
  if (!q) return pdFAIL;
  { std::lock_guard<std::mutex> lk(q->m); q->items.clear(); }
  q->cv_space.notify_all();
  return pdPASS;
}
void vQueueDelete(QueueHandle_t q) { delete q; }

#endif  // !WADA_NO_THREADS
