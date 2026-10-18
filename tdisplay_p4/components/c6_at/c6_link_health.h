// SPDX-License-Identifier: GPL-3.0-or-later
// c6_link_health: pure state machine that decides when the AT-over-SDIO link to the C6 has
// stalled and when the worker may reset it. No ESP-IDF dependencies, so it compiles on the host
// (test/test_c6_link_health.cpp) exactly as it does in c6_at.c.
//
// Inputs are the outcomes of AT commands the worker runs while the link is up:
//   OK / ERR   the C6 answered with a terminal status line (ERROR is still an answer)
//   TIMEOUT    no terminal status before the deadline, or the SDIO send itself failed
//   BUSY       ESP-AT replied "busy p..." (parser alive, but it is stuck on something)
// and the command's kind:
//   CMD        a normal command: TIMEOUT/BUSY count toward the stall streak
//   SOFT       a command whose timeout can be the network's fault (AT+CIPSTART to a dead host):
//              it never counts toward the streak, it only asks for a liveness ping
//   PING       the plain "AT" liveness probe the worker sends after a timeout
//
// Decisions:
//   - a CMD/SOFT timeout asks for a PING; a PING that times out is an immediate STALL (the AT
//     parser itself is gone). A PING that answers changes nothing: the parser can be alive while
//     the Wi-Fi stack under it is wedged, so it must not clear the streak.
//   - C6H_STALL_STREAK CMD timeouts in a row with no CMD answer in between is a STALL.
//   - a STALL takes the link down at once; the reset itself waits for the backoff window
//     (30 s, 60 s, 120 s, 240 s, then 5 min) so a dead C6 is not reset in a tight loop.
//   - the backoff ladder drops back to its first step once a recovered link has answered
//     commands for C6H_STABLE_MS.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define C6H_STALL_STREAK   3
#define C6H_BACKOFF_MIN_MS 30000u
#define C6H_BACKOFF_MAX_MS 300000u
#define C6H_STABLE_MS      600000u     // 10 min of a healthy link resets the backoff ladder

typedef enum { C6H_RES_OK = 0, C6H_RES_ERR, C6H_RES_TIMEOUT, C6H_RES_BUSY } c6h_res_t;
typedef enum { C6H_KIND_CMD = 0, C6H_KIND_SOFT, C6H_KIND_PING } c6h_kind_t;
typedef enum { C6H_ACT_NONE = 0, C6H_ACT_PING, C6H_ACT_STALL } c6h_act_t;

typedef struct {
    uint8_t  streak;          // consecutive CMD timeouts/busy replies
    uint8_t  resets;          // resets since the link was last stable (drives the backoff)
    bool     had_reset;       // at least one runtime reset happened (stability clock is live)
    uint32_t last_reset_ms;   // when the last reset attempt ran
    uint32_t next_reset_ms;   // earliest time the next reset may run
} c6h_t;

static inline void c6h_init(c6h_t *h) {
    h->streak = 0; h->resets = 0; h->had_reset = false;
    h->last_reset_ms = 0; h->next_reset_ms = 0;
}

// Backoff before reset number `n` (0-based count of earlier resets in this unstable run).
static inline uint32_t c6h_backoff_ms(uint8_t n) {
    uint32_t ms = C6H_BACKOFF_MIN_MS;
    for (uint8_t i = 0; i < n && ms < C6H_BACKOFF_MAX_MS; i++) ms *= 2;
    return ms > C6H_BACKOFF_MAX_MS ? C6H_BACKOFF_MAX_MS : ms;
}

// Wrap-safe "now has reached t".
static inline bool c6h_reached(uint32_t now_ms, uint32_t t_ms) {
    return (int32_t)(now_ms - t_ms) >= 0;
}

static inline c6h_act_t c6h_note(c6h_t *h, c6h_res_t r, c6h_kind_t k, uint32_t now_ms) {
    const bool answered = (r == C6H_RES_OK || r == C6H_RES_ERR);
    if (k == C6H_KIND_PING) {
        return (r == C6H_RES_TIMEOUT) ? C6H_ACT_STALL : C6H_ACT_NONE;
    }
    if (answered) {
        if (k == C6H_KIND_CMD) h->streak = 0;
        if (h->had_reset && h->resets && c6h_reached(now_ms, h->last_reset_ms + C6H_STABLE_MS))
            h->resets = 0;
        return C6H_ACT_NONE;
    }
    if (k == C6H_KIND_SOFT) return C6H_ACT_PING;
    if (h->streak < 255) h->streak++;
    return (h->streak >= C6H_STALL_STREAK) ? C6H_ACT_STALL : C6H_ACT_PING;
}

// May the worker run a reset now? The first reset of an unstable run is immediate.
static inline bool c6h_reset_due(const c6h_t *h, uint32_t now_ms) {
    return h->resets == 0 || c6h_reached(now_ms, h->next_reset_ms);
}

// Record a reset attempt (successful or not) and arm the backoff for the next one.
static inline void c6h_on_reset(c6h_t *h, uint32_t now_ms) {
    h->next_reset_ms = now_ms + c6h_backoff_ms(h->resets);
    if (h->resets < 255) h->resets++;
    h->had_reset = true;
    h->last_reset_ms = now_ms;
    h->streak = 0;
}
