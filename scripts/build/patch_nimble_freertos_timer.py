# SPDX-License-Identifier: GPL-3.0-or-later

# NimBLE-Arduino 1.4.3 clears a FreeRTOS callout immediately after
# xTimerDelete(), but xTimerDelete() only queues the deletion. If the timer has
# already expired, the timer daemon can still run the callback against the
# cleared callout and call address zero. T-Pager exposes this when Wi-Fi link
# recovery tears NimBLE down dynamically.
#
# Keep the host timer alive until nimble_port_stop() has finished, drain every
# callout deletion before clearing its callback state, and keep the host event
# queue alive until those callouts are gone. The barrier is FIFO on the same
# timer command queue, so any already-due callback runs while both its callout
# and destination event queue are still valid.
#
# Idempotent and deliberately scoped to environments that list this script.
# All replacements are exact and fail closed if the pinned dependency drifts.
Import("env")
import os

LIB_ROOT = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),
                        "NimBLE-Arduino", "src", "nimble")

HOST_PATH = os.path.join(LIB_ROOT, "nimble", "host", "src", "ble_hs.c")
PORT_PATH = os.path.join(LIB_ROOT, "porting", "nimble", "src",
                         "nimble_port.c")
NPL_PATH = os.path.join(LIB_ROOT, "porting", "npl", "freertos", "src",
                        "npl_os_freertos.c")

HOST_MARKER = "wadamesh-nimble-defer-host-timer-delete"
HOST_OLD = """    if (!ble_hs_is_enabled()) {
        ble_npl_callout_stop(&ble_hs_timer);
        ble_npl_callout_deinit(&ble_hs_timer);
    } else {"""
HOST_NEW = """    if (!ble_hs_is_enabled()) {
        /* wadamesh-nimble-defer-host-timer-delete: nimble_port_stop() can
         * reach this while the FreeRTOS timer daemon still owns an expiry.
         * Stop here; ble_hs_deinit() destroys the callout after the host exits.
         */
        ble_npl_callout_stop(&ble_hs_timer);
    } else {"""

PORT_MARKER = "wadamesh-nimble-drain-callouts-before-eventq-delete"
PORT_OLD = """    ble_npl_eventq_deinit(&g_eventq_dflt);
#endif
    ble_hs_deinit();

#if !SOC_ESP_NIMBLE_CONTROLLER
#if CONFIG_NIMBLE_STACK_USE_MEM_POOLS"""
PORT_NEW = """#endif
    /* wadamesh-nimble-drain-callouts-before-eventq-delete: host callouts can
     * still deliver an already-expired event while their FreeRTOS timer delete
     * is draining. Keep the destination queue valid until they are gone.
     */
    ble_hs_deinit();

#if !SOC_ESP_NIMBLE_CONTROLLER
    ble_npl_eventq_deinit(&g_eventq_dflt);
#if CONFIG_NIMBLE_STACK_USE_MEM_POOLS"""

NPL_MARKER = "wadamesh-nimble-timer-delete-barrier"
NPL_OLD = """void
npl_freertos_callout_deinit(struct ble_npl_callout *co)
{
    if (!co->handle) {
        return;
    }

#if CONFIG_BT_NIMBLE_USE_ESP_TIMER
    if(esp_timer_stop(co->handle))
\tESP_LOGW(LOG_TAG, \"Timer not stopped\");

    if(esp_timer_delete(co->handle))
\tESP_LOGW(LOG_TAG, \"Timer not deleted\");
#else
    xTimerDelete(co->handle, portMAX_DELAY);
    ble_npl_event_deinit(&co->ev);
#endif
    memset(co, 0, sizeof(struct ble_npl_callout));
}"""
NPL_NEW = """#if !CONFIG_BT_NIMBLE_USE_ESP_TIMER
static void
npl_freertos_callout_delete_barrier(void *arg, uint32_t unused)
{
    (void)unused;
    xSemaphoreGive((SemaphoreHandle_t)arg);
}
#endif

void
npl_freertos_callout_deinit(struct ble_npl_callout *co)
{
    if (!co->handle) {
        return;
    }

#if CONFIG_BT_NIMBLE_USE_ESP_TIMER
    if(esp_timer_stop(co->handle))
\tESP_LOGW(LOG_TAG, \"Timer not stopped\");

    if(esp_timer_delete(co->handle))
\tESP_LOGW(LOG_TAG, \"Timer not deleted\");
#else
    /* wadamesh-nimble-timer-delete-barrier: xTimerDelete() is asynchronous.
     * Queue a no-allocation barrier behind it and wait before invalidating the
     * callback state the timer daemon may still be using.
     */
    StaticSemaphore_t barrier_storage;
    SemaphoreHandle_t barrier = xSemaphoreCreateBinaryStatic(&barrier_storage);
    BaseType_t deleted = barrier
        ? xTimerDelete(co->handle, portMAX_DELAY)
        : pdFAIL;
    BaseType_t queued = deleted == pdPASS
        ? xTimerPendFunctionCall(npl_freertos_callout_delete_barrier,
                                 barrier, 0, portMAX_DELAY)
        : pdFAIL;
    BaseType_t drained = queued == pdPASS
        ? xSemaphoreTake(barrier, portMAX_DELAY)
        : pdFAIL;
    if (drained != pdPASS) {
        assert(false);
        return;
    }
    if (co->evq && co->ev.queued) {
        npl_freertos_eventq_remove(co->evq, &co->ev);
    }
    ble_npl_event_deinit(&co->ev);
#endif
    memset(co, 0, sizeof(struct ble_npl_callout));
}"""


def patch_exact(path, marker, old, new):
    if not os.path.isfile(path):
        raise RuntimeError("NimBLE patch target is missing: %s" % path)
    with open(path) as source_file:
        source = source_file.read()
    if marker in source:
        print("[patch_nimble_freertos_timer] already patched: %s" % os.path.basename(path))
        return
    if old not in source:
        raise RuntimeError("NimBLE 1.4.3 patch context drifted: %s" % path)
    with open(path, "w") as source_file:
        source_file.write(source.replace(old, new, 1))
    print("[patch_nimble_freertos_timer] patched: %s" % os.path.basename(path))


patch_exact(HOST_PATH, HOST_MARKER, HOST_OLD, HOST_NEW)
patch_exact(PORT_PATH, PORT_MARKER, PORT_OLD, PORT_NEW)
patch_exact(NPL_PATH, NPL_MARKER, NPL_OLD, NPL_NEW)
