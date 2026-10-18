// SPDX-License-Identifier: GPL-3.0-or-later
// FreeRTOS on ONE thread, for the no-threads browser build (WADA_NO_THREADS: the http
// copy of the page, which is not cross-origin isolated and so has no SharedArrayBuffer
// and no pthreads). Same API as freertos_host.cpp; tasks are Emscripten fibers
// (Asyncify), run by the board loop (wadaCoopRunTasks, once per frame) until they wait.
//
// Every wait is a yield point: vTaskDelay, a semaphore / queue / notification wait,
// delay() and yield() (arduino_host.cpp), and an HTTP request (http_host.cpp: an async
// fetch the task waits for). A wait on the UI thread (the main context) runs the due
// tasks in between and, when nothing can move until the browser does (a fetch), sleeps
// with emscripten_sleep, which hands the thread back to the browser for a moment.
// Nothing preempts: a task owns the thread between two waits, as on one core.
#if defined(WADA_NO_THREADS)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "wada_host.h"
#include <emscripten.h>
#include <emscripten/fiber.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace {
constexpr size_t kTaskStack = 256 * 1024;     // C stack of a task (device tasks ask 6-16 KB)
constexpr size_t kTaskAsyncify = 128 * 1024;  // Asyncify's save area of a task
constexpr size_t kMainAsyncify = 1024 * 1024; // the UI thread's (it may yield deep inside the UI)
uint32_t nowMs() { return (uint32_t)emscripten_get_now(); }
}  // namespace

struct WadaHostTask {
  std::string name;
  emscripten_fiber_t fiber;
  void* cstack = nullptr;
  void* astack = nullptr;
  TaskFunction_t fn = nullptr;
  void* param = nullptr;
  uint32_t wake = 0;      // nowMs() it may run again
  bool done = false;
  uint32_t notify = 0;
};

namespace {
emscripten_fiber_t g_main;
void* g_main_astack = nullptr;
WadaHostTask g_main_task;
WadaHostTask* g_cur = nullptr;            // the task running now (nullptr: the UI thread)
std::vector<WadaHostTask*> g_tasks;
int g_running = 0;                        // wadaCoopRunTasks nesting (a UI wait inside a frame)
bool g_in_loop = false;                   // inside the main loop's step: the browser may be let in
// The UI thread waits: the tasks run meanwhile, and inside the main loop the browser is
// let in for a moment (emscripten_sleep) so a fetch a task waits for can land. Called
// from the page directly (wadaWebFlush, a backup), sleeping would unwind under a caller
// that is not waiting for it: there it only runs the tasks, for at most 3 s.
void mainWaitStep(uint32_t started) {
  wadaCoopRunTasks();
  if (g_in_loop) emscripten_sleep(1);
  (void)started;
}

void ensureMain() {
  if (g_main_astack) return;
  g_main_astack = malloc(kMainAsyncify);
  emscripten_fiber_init_from_current_context(&g_main, g_main_astack, kMainAsyncify);
  g_main_task.name = "loopTask";
}
void entry(void* arg) {
  auto* t = static_cast<WadaHostTask*>(arg);
  t->fn(t->param);
  t->done = true;   // a task function that returns ends, like vTaskDelete(NULL)
  emscripten_fiber_swap(&t->fiber, &g_main);
}
// The task gives the thread back until `wake`.
void taskYield(uint32_t wake) {
  WadaHostTask* t = g_cur;
  t->wake = wake;
  emscripten_fiber_swap(&t->fiber, &g_main);
}
// Wait for pred() up to `wait` ms (portMAX_DELAY: for ever). Tasks yield; the UI thread
// runs the tasks, then lets the browser in when that changed nothing.
bool waitFor(const std::function<bool()>& pred, TickType_t wait) {
  if (pred()) return true;
  if (wait == 0) return false;
  const uint32_t until = nowMs() + (wait == portMAX_DELAY ? 0 : wait);   // (portMAX: the start)
  for (;;) {
    if (g_cur) taskYield(nowMs() + 1);
    else if (!pred()) mainWaitStep(until);
    if (pred()) return true;
    if (wait != portMAX_DELAY && (int32_t)(nowMs() - until) >= 0) return false;
    if (!g_cur && !g_in_loop && wait == portMAX_DELAY && (int32_t)(nowMs() - (until + 3000)) >= 0) {
      fprintf(stderr, "[freertos] a wait outside the main loop gave up after 3 s\n");
      return false;
    }
  }
}
}  // namespace

bool wadaCoopInTask() { return g_cur != nullptr; }
void wadaCoopSetInLoop(bool in) { g_in_loop = in; }
void wadaCoopSleep(uint32_t ms) {
  if (g_cur) { taskYield(nowMs() + ms); return; }
  const uint32_t until = nowMs() + ms;
  while ((int32_t)(nowMs() - until) < 0) mainWaitStep(until);
}
void wadaCoopRunTasks() {
  if (g_cur) return;   // only the UI thread schedules
  ensureMain();
  ++g_running;
  const uint32_t now = nowMs();
  for (size_t i = 0; i < g_tasks.size(); ++i) {
    WadaHostTask* t = g_tasks[i];
    if (t->done || (int32_t)(now - t->wake) < 0) continue;
    g_cur = t;
    emscripten_fiber_swap(&g_main, &t->fiber);
    g_cur = nullptr;
  }
  --g_running;
}

BaseType_t xPortGetCoreID() { return g_cur ? 0 : 1; }   // the UI loop reports core 1, like Arduino

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t, void* param,
                                   UBaseType_t, TaskHandle_t* out, BaseType_t) {
  ensureMain();
  auto* t = new WadaHostTask();
  t->name = name ? name : "task";
  t->fn = fn; t->param = param;
  t->cstack = malloc(kTaskStack);
  t->astack = malloc(kTaskAsyncify);
  if (!t->cstack || !t->astack) { free(t->cstack); free(t->astack); delete t; return pdFAIL; }
  emscripten_fiber_init(&t->fiber, entry, t, t->cstack, kTaskStack, t->astack, kTaskAsyncify);
  t->wake = nowMs();
  g_tasks.push_back(t);
  if (out) *out = t;
  return pdPASS;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char* name, uint32_t depth, void* param,
                                           UBaseType_t prio, StackType_t*, StaticTask_t*, BaseType_t core) {
  TaskHandle_t h = nullptr;
  xTaskCreatePinnedToCore(fn, name, depth, param, prio, &h, core);
  return h;
}
void vTaskDelete(TaskHandle_t t) {
  if (t == nullptr || t == g_cur) {
    if (!g_cur) { fprintf(stderr, "[freertos] vTaskDelete(NULL) from the main context ignored\n"); return; }
    g_cur->done = true;
    emscripten_fiber_swap(&g_cur->fiber, &g_main);   // never resumed
    return;
  }
  t->done = true;   // another task: it simply never runs again (its memory stays: it may hold a lock)
}
void vTaskDelay(TickType_t ticks) { wadaCoopSleep(ticks); }
void vTaskDelayUntil(TickType_t* prev, TickType_t inc) {
  TickType_t target = *prev + inc;
  TickType_t now = xTaskGetTickCount();
  if ((int32_t)(target - now) > 0) vTaskDelay(target - now);
  *prev = target;
}
TickType_t xTaskGetTickCount() {
  static const double t0 = emscripten_get_now();
  return (TickType_t)(emscripten_get_now() - t0);
}
TaskHandle_t xTaskGetCurrentTaskHandle() { return g_cur ? g_cur : &g_main_task; }
const char* pcTaskGetName(TaskHandle_t t) {
  if (!t) t = xTaskGetCurrentTaskHandle();
  return t->name.c_str();
}
BaseType_t xTaskNotifyGive(TaskHandle_t t) { if (!t) return pdFAIL; t->notify++; return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait) {
  WadaHostTask* t = xTaskGetCurrentTaskHandle();
  waitFor([t] { return t->notify != 0; }, wait);
  uint32_t v = t->notify;
  if (v) t->notify = clear_on_exit ? 0 : v - 1;
  return v;
}
BaseType_t xTaskNotify(TaskHandle_t t, uint32_t value, eNotifyAction action) {
  if (!t) return pdFAIL;
  switch (action) {
    case eSetBits: t->notify |= value; break;
    case eIncrement: t->notify++; break;
    case eSetValueWithOverwrite: case eSetValueWithoutOverwrite: t->notify = value; break;
    default: break;
  }
  return pdPASS;
}
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t* value, TickType_t wait) {
  WadaHostTask* t = xTaskGetCurrentTaskHandle();
  t->notify &= ~clear_on_entry;
  const bool ok = waitFor([t] { return t->notify != 0; }, wait);
  if (value) *value = t->notify;
  if (ok) t->notify &= ~clear_on_exit;
  return ok ? pdTRUE : pdFALSE;
}

// ---- semaphores -------------------------------------------------------------
struct WadaHostSem {
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
  WadaHostTask* me = xTaskGetCurrentTaskHandle();
  if (s->recursive && s->owner == me && s->depth) { s->depth++; return pdTRUE; }
  if (!waitFor([s] { return s->count > 0; }, wait)) return pdFALSE;
  s->count--;
  if (s->recursive) { s->owner = me; s->depth = 1; }
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  if (!s) return pdFAIL;
  if (s->recursive && s->depth > 1) { s->depth--; return pdTRUE; }
  if (s->count >= s->max) return pdFALSE;
  s->count++;
  s->owner = nullptr; s->depth = 0;
  return pdTRUE;
}
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s) { return s ? s->count : 0; }
void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }

// ---- queues -----------------------------------------------------------------
struct WadaHostQueue {
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
  std::vector<uint8_t> v((const uint8_t*)item, (const uint8_t*)item + q->item_size);
  if (pos == queueOVERWRITE) { q->items.clear(); q->items.push_back(std::move(v)); return pdPASS; }
  if (!waitFor([q] { return q->items.size() < q->length; }, wait)) return errQUEUE_FULL;
  if (pos == queueSEND_TO_FRONT) q->items.push_front(std::move(v)); else q->items.push_back(std::move(v));
  return pdPASS;
}
static BaseType_t queueTake(QueueHandle_t q, void* out, TickType_t wait, bool remove) {
  if (!q) return pdFAIL;
  if (!waitFor([q] { return !q->items.empty(); }, wait)) return errQUEUE_EMPTY;
  memcpy(out, q->items.front().data(), q->item_size);
  if (remove) q->items.pop_front();
  return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t q, void* out, TickType_t wait) { return queueTake(q, out, wait, true); }
BaseType_t xQueuePeek(QueueHandle_t q, void* out, TickType_t wait) { return queueTake(q, out, wait, false); }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return q ? (UBaseType_t)q->items.size() : 0; }
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q) { return q ? q->length - (UBaseType_t)q->items.size() : 0; }
BaseType_t xQueueReset(QueueHandle_t q) { if (!q) return pdFAIL; q->items.clear(); return pdPASS; }
void vQueueDelete(QueueHandle_t q) { delete q; }
#endif  // WADA_NO_THREADS
