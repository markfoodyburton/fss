/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_native_bench.cpp — the "SystemC on its own, no FSS" baseline.
 *
 * Builds the SAME three CAN models as the combined FSS artefact — a paced
 * initiator, a queued controller (bounded TX FIFO drained at the bus rate), and
 * a paced target that self-terminates on completion — in ONE SystemC kernel,
 * with the IDENTICAL timing constants (PRODUCE/POLL/DRAIN/DEV intervals, queue
 * depth).  The ONLY difference from the FSS combined artefact is the transport
 * between the models: here the two hops are plain TLM-2.0 socket binds
 * (native b_transport), with NO FSS carriers, NO endpoint/binding, NO CCI.
 *
 * This isolates what FSS costs when models are co-located: contrast
 *   SC NATIVE PERF   (this program, pure TLM)
 * with the combined artefact's
 *   PLATFORM CAN COMBINED PERF   (same models, same kernel, internal FSS edges).
 * If they match, FSS's same-thread fast path adds nothing over native TLM.
 *
 * CAN is mapped onto the standard generic payload exactly as can_target.h does:
 *   address = CAN id, data_length = dlc, data_ptr = payload, command = WRITE.
 *
 * CLI:  sc_can_native_bench [frames]   (default 16)
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

namespace {

/* Timing — byte-for-byte the same as the FSS paced/queued models
 * (fss_can_models::can_initiator_paced / can_controller_queued / can_target_paced). */
static const sc_core::sc_time PRODUCE_INTERVAL(2, sc_core::SC_US);
static const sc_core::sc_time POLL_INTERVAL(5, sc_core::SC_US);
static const sc_core::sc_time IDLE_TICK(4, sc_core::SC_US);
static const unsigned         QUEUE_CAP = 4;
static const sc_core::sc_time DRAIN_INTERVAL(10, sc_core::SC_US);
static const sc_core::sc_time DEV_TICK(1, sc_core::SC_US);

/* ── initiator: paces itself against the controller's TX FIFO (native TLM) ─── */
struct can_initiator_paced : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<can_initiator_paced> socket;
    uint32_t count = 16;

    SC_CTOR(can_initiator_paced) : socket("socket") { SC_THREAD(run); }

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
            while (poll_free() == 0)
                sc_core::wait(POLL_INTERVAL);

            uint8_t payload[8];
            payload[0] = static_cast<uint8_t>(i & 0xFF);
            for (uint8_t b = 1; b < 8; ++b) payload[b] = b;

            tlm::tlm_generic_payload gp;
            gp.set_command(tlm::TLM_WRITE_COMMAND);
            gp.set_address(0x100);
            gp.set_data_ptr(payload);
            gp.set_data_length(8);
            gp.set_streaming_width(8);
            gp.set_byte_enable_ptr(nullptr);
            gp.set_dmi_allowed(false);
            gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
            sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
            socket->b_transport(gp, delay);

            sc_core::wait(PRODUCE_INTERVAL);
        }
        for (;;) sc_core::wait(IDLE_TICK);   /* stay live until the sink stops the kernel */
    }
};

/* ── controller: bounded TX FIFO, drained at the bus rate (native TLM out) ─── */
struct frame_t { uint64_t id; uint32_t dlc; uint8_t data[8]; };

struct can_controller_queued : sc_core::sc_module {
    tlm_utils::simple_target_socket<can_controller_queued>    tx;   /* from initiator */
    tlm_utils::simple_initiator_socket<can_controller_queued> out;  /* to device      */
    std::deque<frame_t> fifo;

    SC_CTOR(can_controller_queued) : tx("tx"), out("out") {
        tx.register_b_transport(this, &can_controller_queued::b_transport);
        SC_THREAD(drain);
    }

    void b_transport(tlm::tlm_generic_payload &gp, sc_core::sc_time & /*delay*/) {
        if (gp.get_command() == tlm::TLM_READ_COMMAND) {
            uint32_t free_slots = static_cast<uint32_t>(QUEUE_CAP - fifo.size());
            uint8_t *d = gp.get_data_ptr();
            if (d && gp.get_data_length() >= sizeof(free_slots))
                std::memcpy(d, &free_slots, sizeof(free_slots));
            gp.set_response_status(tlm::TLM_OK_RESPONSE);
            return;
        }
        if (fifo.size() >= QUEUE_CAP) {
            gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            return;
        }
        frame_t f; std::memset(&f, 0, sizeof(f));
        f.id  = gp.get_address();
        f.dlc = static_cast<uint32_t>(gp.get_data_length());
        uint32_t ncopy = f.dlc <= 8 ? f.dlc : 8;
        const uint8_t *p = gp.get_data_ptr();
        if (p && ncopy) std::memcpy(f.data, p, ncopy);
        fifo.push_back(f);
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    void drain() {
        for (;;) {
            sc_core::wait(DRAIN_INTERVAL);
            if (fifo.empty()) continue;
            frame_t f = fifo.front();
            fifo.pop_front();
            tlm::tlm_generic_payload gp;
            gp.set_command(tlm::TLM_WRITE_COMMAND);
            gp.set_address(f.id);
            gp.set_data_ptr(f.data);
            gp.set_data_length(f.dlc);
            gp.set_streaming_width(f.dlc);
            gp.set_byte_enable_ptr(nullptr);
            gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
            sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
            out->b_transport(gp, delay);
        }
    }
};

/* ── device: paced sink; self-terminates when it has all expected frames ───── */
struct can_target_paced : sc_core::sc_module {
    tlm_utils::simple_target_socket<can_target_paced> socket;   /* "can_rx" */
    uint64_t expect_frames = 16;
    uint32_t last_id = 0; uint8_t last_byte0 = 0; uint64_t rx_count = 0;

    SC_CTOR(can_target_paced) : socket("can_rx") {
        socket.register_b_transport(this, &can_target_paced::b_transport);
        SC_THREAD(heartbeat);
    }

    void b_transport(tlm::tlm_generic_payload &gp, sc_core::sc_time & /*delay*/) {
        last_id = static_cast<uint32_t>(gp.get_address());
        const uint8_t *d = gp.get_data_ptr();
        last_byte0 = (gp.get_data_length() > 0 && d) ? d[0] : 0;
        ++rx_count;
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    void heartbeat() {
        for (;;) {
            sc_core::wait(DEV_TICK);
            if (rx_count >= expect_frames) { sc_core::sc_stop(); return; }
        }
    }
};

static double now_ns() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

} // namespace

int sc_main(int argc, char **argv)
{
    uint32_t N = (argc > 1) ? static_cast<uint32_t>(std::atol(argv[1])) : 16;

    can_initiator_paced   ini("initiator");
    can_controller_queued ctrl("controller");
    can_target_paced      dev("device");
    ini.count = N;
    dev.expect_frames = N;

    /* Native TLM binds — the only difference from the FSS combined artefact. */
    ini.socket.bind(ctrl.tx);
    ctrl.out.bind(dev.socket);

    double t0 = now_ns();
    sc_core::sc_start();          /* runs to the device's sc_stop() */
    double t1 = now_ns();

    bool ok = dev.rx_count == N && dev.last_id == 0x100u
           && dev.last_byte0 == static_cast<uint8_t>((N - 1) & 0xFF);

    std::printf("SC CAN NATIVE PASS: rx=%llu/%u last_id=0x%X last_b0=%u\n",
                static_cast<unsigned long long>(dev.rx_count), N, dev.last_id, dev.last_byte0);
    if (ok) {
        double ns = t1 - t0;
        double ns_per_txn = ns / (double)N;
        double tps = ns > 0.0 ? (double)N * 1e9 / ns : 0.0;
        std::printf("SC NATIVE PERF: frames=%u ns_per_txn=%.1f txns_per_sec=%.0f\n",
                    N, ns_per_txn, tps);
        return 0;
    }
    std::printf("[FAIL] sc native bench mismatch\n");
    return 1;
}
