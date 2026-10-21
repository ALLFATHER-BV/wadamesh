#pragma once
// Host-side knobs shared by the compat layer and web/platform.
#include <string>
// Root of the virtual flash: every SPIFFS/LittleFS file and the NVS store live under it.
// $WADA_DATA_DIR, else WADA_DEFAULT_DATA_DIR (./wadamesh-data natively, /data in the
// browser, where it is an IDBFS mount: web/shell/wadamesh.js).
const std::string& wadaHostDataDir();
// Every change to the virtual flash (a file written or closed after writing, a remove,
// rename, mkdir, format, an NVS commit) bumps one counter. Thread-safe. The browser
// front end polls it to persist the IDBFS mount after writes settle; native ignores it.
void wadaHostFsTouched();
unsigned wadaHostFsGeneration();
// mkdir -p
bool wadaHostMkdirs(const std::string& path);

#if defined(WADA_NO_THREADS)
// The no-threads browser build (web/compat/src/freertos_coop.cpp): FreeRTOS tasks are
// cooperative fibers on the one thread. They run from the board loop and give the
// thread back at every wait (vTaskDelay, a semaphore / queue / notification wait,
// delay(), an HTTP request).
#include <stdint.h>
bool wadaCoopInTask();               // running inside a task fiber
void wadaCoopSleep(uint32_t ms);     // a task: yield for ms; the UI thread: wait, running the tasks
void wadaCoopRunTasks();             // the board loop: every task that is due runs until it waits
void wadaCoopSetInLoop(bool in);     // web_main.cpp: true while the main loop's step runs
#endif
