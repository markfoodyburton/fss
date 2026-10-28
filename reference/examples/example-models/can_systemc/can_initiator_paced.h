/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_initiator_paced.h — fss_can_models::can_initiator_paced: a kernel-bearing
 * SystemC CAN initiator that PACES itself against the controller's TX FIFO.
 *
 * The time-sync bench's initiator.  Unlike the fire-and-forget initiator (which
 * blasts N writes in ~0 time), this one is a flow-controlled producer:
 *
 *   for each of `count` frames:
 *     poll free space  (a TLM READ round-trip to the controller's "tx")
 *     while full: wait(POLL_INTERVAL)         — back off, advancing OUR clock
 *     write the frame  (a TLM WRITE = enqueue)
 *     wait(PRODUCE_INTERVAL)                  — production pacing
 *
 *   "socket"  outbound — TLM initiator socket carrying both the WRITE (enqueue)
 *                        and the READ (free-space poll); the host binds it to the
 *                        controller's "tx".
 *
 * Included by the standalone fss_can_initiator_paced_systemc artefact and by the
 * combined fss_can_combined_systemc artefact (which builds all three models in
 * one kernel and names them under fss_can_models).
 */
#ifndef FSS_CAN_INITIATOR_PACED_H
#define FSS_CAN_INITIATOR_PACED_H

#include "fss/systemc_tlm/artefact.h"
#include "fss/c/cfg_json.h"

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>

#include <cstdint>

namespace fss_can_models {

/* The initiator's pacing.  PRODUCE_INTERVAL is its intrinsic production period;
 * POLL_INTERVAL is the back-off between free-space polls when the FIFO is full.
 * Both are µs-scale so the run spans many quanta (the time-sync exercise).
 * IDLE_TICK keeps the initiator's clock advancing AFTER it has produced
 * everything, so it stays a live timing peer (no sc_stop, no mid-run detach) until
 * its downstream peer — the controller — detaches, at which point teardown
 * cascades source-ward and the model lets the kernel stop. */
static const sc_core::sc_time PRODUCE_INTERVAL(2, sc_core::SC_US);
static const sc_core::sc_time POLL_INTERVAL(5, sc_core::SC_US);
static const sc_core::sc_time IDLE_TICK(4, sc_core::SC_US);

struct can_initiator_paced : sc_core::sc_module,
                             fss::systemc_tlm::fss_model_hooks {
    tlm_utils::simple_initiator_socket<can_initiator_paced> socket;
    uint32_t count = 16;   /* frames to send (the device oracle expects rx=16) */
    sc_core::sc_time produce_interval = PRODUCE_INTERVAL;  /* production period */

    SC_CTOR(can_initiator_paced) : socket("socket") {
        SC_THREAD(run);
    }

    /* fss/config (JSON form): set run length and production rate before the run.
     * "frames" = total frames to emit; "period_us" = production period in µs. */
    void fss_configure(const char *json) override {
        uint64_t v = 0;
        if (fss_cfg_json_get_u64(json, "frames", &v))    count = static_cast<uint32_t>(v);
        if (fss_cfg_json_get_u64(json, "period_us", &v) && v > 0)
            produce_interval = sc_core::sc_time(static_cast<double>(v), sc_core::SC_US);
    }

    /* Teardown cascade: when the controller (our only downstream timing peer)
     * detaches, we have nothing left to do — let the kernel stop. */
    bool fss_peer_detached(int /*port_index*/) override { return true; }

    /* One TLM READ round-trip: returns the controller's free queue slots. */
    uint32_t poll_free() {
        uint32_t free_slots = 0;
        tlm::tlm_generic_payload gp;
        gp.set_command(tlm::TLM_READ_COMMAND);
        gp.set_address(0);
        gp.set_data_ptr(reinterpret_cast<uint8_t *>(&free_slots));
        gp.set_data_length(sizeof(free_slots));
        gp.set_streaming_width(sizeof(free_slots));
        gp.set_byte_enable_ptr(nullptr);
        gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        socket->b_transport(gp, delay);
        return gp.get_response_status() == tlm::TLM_OK_RESPONSE ? free_slots : 0;
    }

    void run() {
        for (uint32_t i = 0; i < count; ++i) {
            while (poll_free() == 0)            /* full → back off on our clock */
                sc_core::wait(POLL_INTERVAL);

            uint8_t payload[8];
            payload[0] = static_cast<uint8_t>(i & 0xFF);
            for (uint8_t b = 1; b < 8; ++b) payload[b] = b;

            tlm::tlm_generic_payload gp;
            gp.set_command(tlm::TLM_WRITE_COMMAND);
            gp.set_address(0x100);                 /* CAN id  */
            gp.set_data_ptr(payload);              /* payload */
            gp.set_data_length(8);                 /* dlc     */
            gp.set_streaming_width(8);
            gp.set_byte_enable_ptr(nullptr);
            gp.set_dmi_allowed(false);
            gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

            sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
            socket->b_transport(gp, delay);

            sc_core::wait(produce_interval);       /* production pacing */
        }
        /* Produced everything — keep ticking (stay a live timing peer) until the
         * controller detaches; fss_peer_detached then stops the kernel. */
        for (;;) sc_core::wait(IDLE_TICK);
    }
};

} // namespace fss_can_models

#endif /* FSS_CAN_INITIATOR_PACED_H */
