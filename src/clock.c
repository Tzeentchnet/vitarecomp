/* SPDX-License-Identifier: MIT */
#include "clock.h"

#include "vitarecomp/host.h"

#include <limits.h>
#include <threads.h>
#include <time.h>

#define VBLANKS_PER_SECOND 60ull
#define DEFAULT_VIRTUAL_EPOCH_US UINT64_C(1767225600000000)

typedef struct clock_state {
    mtx_t lock;
    int virtual_clock;
    uint64_t now_us;
    uint64_t epoch_us;
    uint64_t read_step_us;
    uint64_t host_start_us;
} clock_state;

static once_flag g_once = ONCE_FLAG_INIT;
static clock_state g_clock;

static void clock_once(void) {
    (void)mtx_init(&g_clock.lock, mtx_plain);
    g_clock.epoch_us = DEFAULT_VIRTUAL_EPOCH_US;
}

static uint64_t add_saturated(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static uint64_t host_unix_us(void) {
    struct timespec value;
    (void)timespec_get(&value, TIME_UTC);
    if (value.tv_sec < 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000) +
           (uint64_t)value.tv_nsec / UINT64_C(1000);
}

void vita_clock_configure(int virtual_clock, uint64_t epoch_us,
                          uint64_t read_step_us) {
    call_once(&g_once, clock_once);
    mtx_lock(&g_clock.lock);
    g_clock.virtual_clock = virtual_clock != 0;
    g_clock.now_us = 0;
    g_clock.epoch_us = epoch_us ? epoch_us : DEFAULT_VIRTUAL_EPOCH_US;
    g_clock.read_step_us = read_step_us;
    g_clock.host_start_us = 0;
    mtx_unlock(&g_clock.lock);
}

void vita_clock_virtual(void) {
    vita_clock_configure(1, DEFAULT_VIRTUAL_EPOCH_US, 1);
}

void vita_clock_reset(void) {
    vita_clock_configure(0, DEFAULT_VIRTUAL_EPOCH_US, 0);
}

int vita_clock_is_virtual(void) {
    int result;
    call_once(&g_once, clock_once);
    mtx_lock(&g_clock.lock);
    result = g_clock.virtual_clock;
    mtx_unlock(&g_clock.lock);
    return result;
}

uint64_t vita_clock_us(void) {
    uint64_t result;
    call_once(&g_once, clock_once);
    mtx_lock(&g_clock.lock);
    if (g_clock.virtual_clock) {
        result = g_clock.now_us;
        g_clock.now_us = add_saturated(g_clock.now_us,
                                       g_clock.read_step_us);
    } else {
        uint64_t now = host_unix_us();
        if (!g_clock.host_start_us) g_clock.host_start_us = now;
        result = now >= g_clock.host_start_us
               ? now - g_clock.host_start_us : 0;
    }
    mtx_unlock(&g_clock.lock);
    return result;
}

uint64_t vita_clock_unix_us(void) {
    uint64_t result;
    call_once(&g_once, clock_once);
    mtx_lock(&g_clock.lock);
    result = g_clock.virtual_clock
           ? add_saturated(g_clock.epoch_us, g_clock.now_us)
           : host_unix_us();
    mtx_unlock(&g_clock.lock);
    return result;
}

void vita_clock_advance(uint64_t us) {
    call_once(&g_once, clock_once);
    mtx_lock(&g_clock.lock);
    if (g_clock.virtual_clock)
        g_clock.now_us = add_saturated(g_clock.now_us, us);
    mtx_unlock(&g_clock.lock);
}

void vita_clock_wait(uint64_t us) {
    call_once(&g_once, clock_once);
    mtx_lock(&g_clock.lock);
    int virtual_clock = g_clock.virtual_clock;
    if (virtual_clock)
        g_clock.now_us = add_saturated(g_clock.now_us, us);
    mtx_unlock(&g_clock.lock);
    if (!virtual_clock && us) {
        struct timespec delay = {
            (time_t)(us / UINT64_C(1000000)),
            (long)((us % UINT64_C(1000000)) * UINT64_C(1000))
        };
        (void)thrd_sleep(&delay, NULL);
    }
}

void vita_clock_vblank(void) {
    call_once(&g_once, clock_once);
    mtx_lock(&g_clock.lock);
    if (g_clock.virtual_clock) {
        uint64_t second = g_clock.now_us / UINT64_C(1000000);
        uint64_t within = g_clock.now_us % UINT64_C(1000000);
        if (second > (UINT64_MAX - VBLANKS_PER_SECOND) /
                     VBLANKS_PER_SECOND) {
            g_clock.now_us = UINT64_MAX;
        } else {
            uint64_t frame = second * VBLANKS_PER_SECOND +
                within * VBLANKS_PER_SECOND / UINT64_C(1000000) + 1;
            if (frame > (UINT64_MAX - VBLANKS_PER_SECOND + 1) /
                        UINT64_C(1000000)) {
                g_clock.now_us = UINT64_MAX;
            } else {
                g_clock.now_us =
                    (frame * UINT64_C(1000000) +
                     VBLANKS_PER_SECOND - 1) / VBLANKS_PER_SECOND;
            }
        }
    }
    mtx_unlock(&g_clock.lock);
}
