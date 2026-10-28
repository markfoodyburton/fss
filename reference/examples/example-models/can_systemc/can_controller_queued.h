/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_controller_queued.h — fss_can_models::can_controller_queued: a
 * kernel-bearing SystemC CAN controller with a bounded TX FIFO, drained on
 * its OWN clock.
 *
 * This is the time-sync bench's controller (a more faithful CAN controller than
 * the pure-translate one): a real TX queue filled by the initiator and drained
 * to the bus at the bus rate, so the federation's simulated time is dominated by
 * COMMUNICATION (queueing + draining), not setup — the regime in which time-sync
 * has teeth.  Two carriers still meet here: a TLM txn in, a can::frame out.
 *
 *   "tx"     inbound  — TLM register access (round-trip):
 *                         WRITE = enqueue a derived can::frame (TLM_OK), or
 *                                 TLM_GENERIC_ERROR if the queue is full;
 *                         READ  = report free queue slots (a uint32 in the
 *                                 payload) so the initiator can poll for space —
 *                                 the in-band back-pressure (we have no other
 *                                 way to push back across the FSS boundary).
 *   "can_tx" outbound — can::frame (one-way), emitted by the free-running drain
 *                       SC_THREAD, one frame per DRAIN_INTERVAL.  Declared via the
 *                       fss_emitter hook (the binding can't auto-bridge a non-TLM
 *                       outbound).
 *
 * Included by the standalone fss_can_controller_queued_systemc artefact and by
 * the combined fss_can_combined_systemc artefact (which builds all three models
 * in one kernel and names them under fss_can_models).
 */
#ifndef FSS_CAN_CONTROLLER_QUEUED_H
#define FSS_CAN_CONTROLLER_QUEUED_H

#include "fss/systemc_tlm/artefact.h"     /* artefact<>, fss_model_hooks/fss_emitter */
#include "fss/endpoint.h"
#include "fss/known_carriers/sccan_frame.h"  /* FSS_SCCAN_ID / FSS_SCCAN_META (SystemC's own frame) */

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>

namespace fss_can_models {

/* one sccan::frame {bus::Locator u64@0, bus::Length u32@8, can::Ide u32@12, bus::Data@16(8)} */
using frame_t = std::array<uint8_t, 24>;

/* TX FIFO depth and the bus drain rate.  The drain rate is the slow side, so it
 * sets the federation's pace — the initiator back-pressures down to it. */
static const unsigned        QUEUE_CAP = 4;
static const sc_core::sc_time DRAIN_INTERVAL(10, sc_core::SC_US);

struct can_controller_queued : sc_core::sc_module,
                               fss::systemc_tlm::fss_model_hooks {
    tlm_utils::simple_target_socket<can_controller_queued> tx;  /* inbound "tx" */
    fss_endpoint_t      *can_tx = nullptr;                      /* outbound, via hook */
    std::deque<frame_t>  fifo;

    SC_CTOR(can_controller_queued) : tx("tx") {
        tx.register_b_transport(this, &can_controller_queued::b_transport);
        SC_THREAD(drain);
    }

    void fss_attach(fss::systemc_tlm::fss_emitter &binding) override {
        can_tx = binding.declare_outbound("can_tx");
    }

    /* Teardown cascade (Mark's sink-driven design): when a timing peer detaches,
     * the relay's pipe is broken (in this bench the downstream device detaches
     * first, having received everything) — nothing left to relay, so stop.  The
     * binding then opens OUR fss/time window upstream, so the initiator stops too. */
    bool fss_peer_detached(int /*port_index*/) override { return true; }

    /* READ = poll free space; WRITE = enqueue a derived frame (if room). */
    void b_transport(tlm::tlm_generic_payload &gp, sc_core::sc_time & /*delay*/) {
        if (gp.get_command() == tlm::TLM_READ_COMMAND) {
            uint32_t free_slots = static_cast<uint32_t>(QUEUE_CAP - fifo.size());
            uint8_t *d = gp.get_data_ptr();
            if (d && gp.get_data_length() >= sizeof(free_slots))
                std::memcpy(d, &free_slots, sizeof(free_slots));
            gp.set_response_status(tlm::TLM_OK_RESPONSE);
            return;
        }

        if (fifo.size() >= QUEUE_CAP) {                 /* full → back-pressure */
            gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            return;
        }

        uint64_t id  = gp.get_address();
        uint32_t dlc = static_cast<uint32_t>(gp.get_data_length());
        const uint8_t *payload = gp.get_data_ptr();
        uint32_t ncopy = dlc <= 8 ? dlc : 8;

        /* Build SystemC's own sccan::frame from the registers the initiator wrote. */
        frame_t f; f.fill(0);
        std::memcpy(f.data() + 0,  &id,  8);  /* bus::Locator u64@0   */
        std::memcpy(f.data() + 8,  &dlc, 4);  /* bus::Length   u32@8  */
        uint32_t ide = 0;                     /* can::Ide=Standard    */
        std::memcpy(f.data() + 12, &ide, 4);  /* can::Ide      u32@12 */
        if (payload && ncopy) std::memcpy(f.data() + 16, payload, ncopy);  /* bus::Data@16 */
        fifo.push_back(f);
        gp.set_response_status(tlm::TLM_OK_RESPONSE);   /* ack the initiator */
    }

    /* Free-running drain: one frame per DRAIN_INTERVAL — the controller's clock
     * advances on its own, independent of the initiator. */
    void drain() {
        for (;;) {
            sc_core::wait(DRAIN_INTERVAL);
            if (fifo.empty()) continue;
            frame_t f = fifo.front();
            fifo.pop_front();
            if (can_tx && can_tx->send) {
                fss_carrier_t c;
                std::memset(&c, 0, sizeof(c));
                c.id        = FSS_SCCAN_ID;
                c.data_ptr  = f.data();
                c.data_size = f.size();
                c.meta_ptr  = (void *)FSS_SCCAN_META;
                c.meta_size = FSS_SCCAN_META_LEN;
                can_tx->send(can_tx->send_ctx, &c);
            }
        }
    }
};

} // namespace fss_can_models

#endif /* FSS_CAN_CONTROLLER_QUEUED_H */
