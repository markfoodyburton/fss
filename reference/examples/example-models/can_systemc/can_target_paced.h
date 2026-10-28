/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_target_paced.h — fss_can_models::can_target_paced: the time-sync bench's
 * CAN device — the clean can_target plus a free-running heartbeat so it has its
 * OWN clock and can PARTICIPATE in time-sync, AND a completion-stop so it drives
 * teardown.
 *
 * The plain can_target is purely reactive (it advances no time of its own).  A
 * node with no autonomous clock cannot participate in the windowed sync: it would
 * suspend at its first quantum and never reach its bound to hand a window back,
 * stalling its peer.  This subclass adds a heartbeat SC_THREAD so the device
 * advances independently and exchanges windows with the controller — making it a
 * first-class third timing node (so the controller has TWO timing peers).
 *
 * It also drives TEARDOWN (Mark's sink-driven design): the device is the SINK, so
 * it knows when the federation is done — when it has received all EXPECTED frames.
 * At that point it sc_stop()s; its binding opens its fss/time window to the
 * controller, whose consumer is now gone, so the controller stops and opens ITS
 * window upstream, so the initiator stops — teardown cascades sink→source through
 * fss/time, with NO post-work idle tail (sim time ends right at completion).
 *
 *   "can_rx"  inbound — can::frame (one-way), recorded by the base b_transport.
 *
 * Included by the standalone fss_can_target_paced_systemc artefact and by the
 * combined fss_can_combined_systemc artefact (which builds all three models in
 * one kernel and names them under fss_can_models).
 */
#ifndef FSS_CAN_TARGET_PACED_H
#define FSS_CAN_TARGET_PACED_H

#include "fss/systemc_tlm/artefact.h"
#include "fss/c/cfg_json.h"

#include "can_target.h"   /* base: records rx_count / last_id / last_byte0 */

#include <cstdint>
#include <cstdio>
#include <systemc>

namespace fss_can_models {

/* The device's intrinsic clock period (its "bus idle" tick).  Free-running so the
 * device's kernel advances time on its own and reaches each window bound.  Set to
 * the sync quantum (1 µs): a node's effective skew floor is its OWN clock
 * granularity (between ticks it really IS up to one tick past its last report), so
 * a coarse heartbeat would SHOW as skew even when sync is perfect.  At the quantum,
 * the device reports every quantum and the skew stays at ~1 quantum. */
static const sc_core::sc_time DEV_TICK(1, sc_core::SC_US);

/* The federation is done when the sink has received this many frames (default
 * matches the bench's 16; overridable via fss/config "frames"). */
static const uint64_t EXPECT_FRAMES = 16;

struct can_target_paced : can_target, fss::systemc_tlm::fss_model_hooks {
    SC_HAS_PROCESS(can_target_paced);
    uint64_t expect_frames = EXPECT_FRAMES;   /* frames to await before teardown */
    explicit can_target_paced(sc_core::sc_module_name n) : can_target(n) {
        SC_THREAD(heartbeat);
    }

    /* fss/config (JSON form): set how many frames to expect before driving
     * teardown.  "frames" mirrors the initiator's production count. */
    void fss_configure(const char *json) override {
        uint64_t v = 0;
        if (fss_cfg_json_get_u64(json, "frames", &v)) expect_frames = v;
    }

    /* Free-running clock AND sink-driven teardown: tick on our own time, and once
     * everything has arrived (rx_count is set by the base b_transport on the same
     * kernel thread), sc_stop().  The binding then opens our fss/time window → the
     * controller's consumer is gone → it stops and opens its window upstream → the
     * initiator stops.  Teardown cascades sink→source with no idle tail.  Checking
     * in the heartbeat (rather than overriding b_transport, which the base already
     * registered on the socket) keeps it within one DEV_TICK of completion. */
    void heartbeat() {
        for (;;) {
            sc_core::wait(DEV_TICK);
            if (rx_count >= expect_frames) { sc_core::sc_stop(); return; }
        }
    }

    ~can_target_paced() {
        std::printf("SC CAN TARGET PASS: rx=%llu last_id=0x%X last_b0=%u\n",
                    static_cast<unsigned long long>(rx_count), last_id, last_byte0);
        std::fflush(stdout);
    }
};

} // namespace fss_can_models

#endif /* FSS_CAN_TARGET_PACED_H */
