/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fmi_timesync.h — shared time-sync constants and helpers for the three FMI
 * CAN artefacts (can_fmi_ctrl_queued, can_fmi_tx_paced, can_fmi_rx_paced).
 *
 * INCLUDE AFTER:
 *   (a) node_t is fully defined in the including translation unit,
 *   (b) `static void *clock_thread(void *arg);` is forward-declared, AND
 *   (c) fss/protocols/sim.h, fss/endpoint.h, fss/carrier.h, pthread.h,
 *       string.h, and time.h are already included.
 *
 * Uses `static` so each MODULE translation unit gets its own copy (C mixin
 * pattern — no link-time ODR conflicts between separately loaded .so/.dylib
 * objects that each include this header).
 */
#ifndef FSS_FMI_TIMESYNC_H
#define FSS_FMI_TIMESYNC_H

#include <stdint.h>

/* Shared time-sync protocol constants.
 * FSS_TIME_ID/META come from fss/protocols/time_sync.h (included by the caller).
 * MAX_SIM_DOWN is NOT here: it is a struct-array dimension, so it must be defined
 * before each artefact's node_t declaration (before this header is included). */
#define TIME_QUANTUM_PS 1000000ULL      /* 1 µs in picoseconds                     */
#define TIME_UNIT_PS    1               /* time::Unit value for picoseconds         */
#define TIME_OPEN_UPPER 0xFFFFFFFFFFFFFFFFULL  /* open-window / detach sentinel    */
#define TW_SIZE         24              /* time::window payload size in bytes       */

/* sim::EventCode values (normative; from the generated fss/protocols/sim.h). */
#define SIM_BOUND       0u  /* sim::EventCode::Bound      */
#define SIM_ELABORATED  1u  /* sim::EventCode::Elaborated */
#define SIM_STARTED     2u  /* sim::EventCode::Started    */
#define SIM_STOPPED     5u  /* sim::EventCode::Stopped    */

/* Convert a time::window timestamp to picoseconds.
 * unit >= 1: value is in 10^(3*(unit-1)) ps (unit=1 → ps, unit=2 → ns, …).
 * unit <  1: value is in sub-ps units (divided by 1000 to get ps). */
static inline uint64_t ts_to_ps(int32_t unit, uint64_t value)
{
    if (unit >= 1) {
        unsigned __int128 v = value;
        for (int i = 0; i < unit - 1; ++i) v *= 1000u;
        return (uint64_t)(v > (unsigned __int128)UINT64_MAX ? UINT64_MAX : v);
    }
    return value / 1000u;
}

/* Announce sim::Elaborated UP the chain on n->sim (retry until delivered or
 * until n->ts_stop — readiness handshake, not a data window). */
static void send_sim_up(node_t *n)
{
    fss_endpoint_t *up = &n->sim;
    if (!up->send) return;
    uint32_t code = SIM_ELABORATED;
    uint8_t  buf[4]; memcpy(buf, &code, 4);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_SIM_ID; c.data_ptr = buf; c.data_size = 4;
    c.meta_ptr = (void *)FSS_SIM_META; c.meta_size = FSS_SIM_META_LEN;
    for (;;) {
        if (up->send(up->send_ctx, &c) == FSS_SEND_OK) return;
        pthread_mutex_lock(&n->mu);
        int stop = n->ts_stop;
        pthread_mutex_unlock(&n->mu);
        if (stop) return;
        struct timespec ts = { 0, 2 * 1000000L }; nanosleep(&ts, NULL);
    }
}

/* START BARRIER: spawn the clock once wired AND all downstream peers ready.
 * Called from sim_receive after Bound/Started/Elaborated events. */
static void try_start(node_t *n)
{
    if (!n->wired || n->downstream_ready < n->n_downstream) return;
    pthread_mutex_lock(&n->mu);
    int spawn = !n->ts_running && !n->ts_stop;
    if (spawn) n->ts_running = 1;
    pthread_mutex_unlock(&n->mu);
    if (spawn) pthread_create(&n->clock_thread, NULL, clock_thread, n);
}

#endif /* FSS_FMI_TIMESYNC_H */
