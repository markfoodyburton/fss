/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* spi_initiator_systemc.cpp — benchmark process: phases A–G covering b_transport and DMI. */

#include "spi_initiator_systemc.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

using clk = std::chrono::steady_clock;

/* Returns elapsed wall time in milliseconds. */
static double wall_ms(clk::time_point t0, clk::time_point t1)
{
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

/* Convenience: begin-of-phase banner. */
static void phase_start(const char *label, uint64_t n)
{
    std::printf("[T=%s]  %s  (N=%llu)\n",
                sc_core::sc_time_stamp().to_string().c_str(),
                label,
                (unsigned long long)n);
}

/* Convenience: end-of-phase summary. */
static void phase_end(double wms, sc_core::sc_time sim_elapsed, uint64_t n)
{
    double total_ns = wms * 1e6;          /* ms → ns */
    double ns_per_op = total_ns / (double)n;
    double mops = (double)n / (wms * 1e3); /* ms → us → Mops */
    std::printf("    done in %.2f ms wall, %.3f us sim — %.1f ns/op, %.2f Mops/s\n",
                wms,
                sim_elapsed.to_seconds() * 1e6,
                ns_per_op,
                mops);
}

void spi_initiator_systemc::run()
{
    const uint64_t n        = n_ops;   /* capture for the duration of run() */
    const uint64_t reg_base = register_base.get_value();
    const uint64_t mem_base = memory_base.get_value();

    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    uint8_t data[4] = {0xa5, 0xa5, 0xa5, 0xa5};

    tlm::tlm_generic_payload gp;
    gp.set_data_ptr(data);
    gp.set_data_length(4);
    gp.set_streaming_width(4);
    gp.set_byte_enable_ptr(nullptr);
    gp.set_dmi_allowed(false);
    gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    /* ------------------------------------------------------------------ */
    /* Phase A: warm-up — one b_transport write to register region.        */
    /* ------------------------------------------------------------------ */
    phase_start("phase A: warm-up b_transport write", 1);
    sc_core::sc_time sim_t0 = sc_core::sc_time_stamp();
    clk::time_point w0 = clk::now();

    gp.set_command(tlm::TLM_WRITE_COMMAND);
    gp.set_address(reg_base);
    gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    socket->b_transport(gp, delay);
    assert(gp.get_response_status() == tlm::TLM_OK_RESPONSE);

    {
        auto w_end = clk::now();
        wait(delay);
        delay = sc_core::SC_ZERO_TIME;
        phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, 1);
    }
    wait(10, sc_core::SC_NS);

    /* ------------------------------------------------------------------ */
    /* Phase B: b_transport register writes (N) + reads (N).              */
    /* ------------------------------------------------------------------ */
    phase_start("phase B: b_transport register writes", n);
    sim_t0 = sc_core::sc_time_stamp();
    w0 = clk::now();

    gp.set_command(tlm::TLM_WRITE_COMMAND);
    for (uint64_t i = 0; i < n; ++i) {
        gp.set_address(reg_base + (i * 4) % REGISTER_REGION_SIZE);
        gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        socket->b_transport(gp, delay);
        assert(gp.get_response_status() == tlm::TLM_OK_RESPONSE);
    }
    {
        auto w_end = clk::now();
        wait(delay);
        delay = sc_core::SC_ZERO_TIME;
        phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
    }

    phase_start("phase B: b_transport register reads", n);
    sim_t0 = sc_core::sc_time_stamp();
    w0 = clk::now();

    gp.set_command(tlm::TLM_READ_COMMAND);
    for (uint64_t i = 0; i < n; ++i) {
        gp.set_address(reg_base + (i * 4) % REGISTER_REGION_SIZE);
        gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        socket->b_transport(gp, delay);
        assert(gp.get_response_status() == tlm::TLM_OK_RESPONSE);
    }
    {
        auto w_end = clk::now();
        wait(delay);
        delay = sc_core::SC_ZERO_TIME;
        phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
    }
    wait(10, sc_core::SC_NS);

    /* ------------------------------------------------------------------ */
    /* Phase C: b_transport memory writes (N) + reads (N) — slow path.    */
    /* ------------------------------------------------------------------ */
    phase_start("phase C: b_transport memory writes", n);
    sim_t0 = sc_core::sc_time_stamp();
    w0 = clk::now();

    gp.set_command(tlm::TLM_WRITE_COMMAND);
    for (uint64_t i = 0; i < n; ++i) {
        gp.set_address(mem_base + (i * 4) % MEMORY_REGION_SIZE);
        gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        socket->b_transport(gp, delay);
        assert(gp.get_response_status() == tlm::TLM_OK_RESPONSE);
    }
    {
        auto w_end = clk::now();
        wait(delay);
        delay = sc_core::SC_ZERO_TIME;
        phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
    }

    phase_start("phase C: b_transport memory reads", n);
    sim_t0 = sc_core::sc_time_stamp();
    w0 = clk::now();

    gp.set_command(tlm::TLM_READ_COMMAND);
    for (uint64_t i = 0; i < n; ++i) {
        gp.set_address(mem_base + (i * 4) % MEMORY_REGION_SIZE);
        gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        socket->b_transport(gp, delay);
        assert(gp.get_response_status() == tlm::TLM_OK_RESPONSE);
    }
    {
        auto w_end = clk::now();
        wait(delay);
        delay = sc_core::SC_ZERO_TIME;
        phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
    }
    wait(10, sc_core::SC_NS);

    /* ------------------------------------------------------------------ */
    /* Phase D: DMI grant for memory region; bulk writes (N) + reads (N). */
    /* ------------------------------------------------------------------ */
    phase_start("phase D: DMI memory bulk writes+reads", n);
    tlm::tlm_dmi dmi;
    gp.set_address(mem_base);
    gp.set_command(tlm::TLM_READ_COMMAND);
    if (socket->get_direct_mem_ptr(gp, dmi)) {
        uint8_t *base = dmi.get_dmi_ptr();
        std::size_t dmi_sz =
            (std::size_t)(dmi.get_end_address() - dmi.get_start_address() + 1);

        if (dmi.is_write_allowed()) {
            sim_t0 = sc_core::sc_time_stamp();
            w0 = clk::now();
            for (uint64_t i = 0; i < n; ++i) {
                std::size_t off = (i * 4) % (dmi_sz > 4 ? dmi_sz - 4 : 1);
                std::memcpy(base + off, data, 4);
            }
            {
                auto w_end = clk::now();
                sc_core::sc_time accum = dmi.get_write_latency() * n;
                wait(accum);
                phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
            }
            phase_start("phase D: DMI memory bulk reads", n);
        } else {
            std::printf("    DMI write not allowed — skipping write sub-phase\n");
        }

        if (dmi.is_read_allowed()) {
            sim_t0 = sc_core::sc_time_stamp();
            w0 = clk::now();
            for (uint64_t i = 0; i < n; ++i) {
                std::size_t off = (i * 4) % (dmi_sz > 4 ? dmi_sz - 4 : 1);
                std::memcpy(data, base + off, 4);
            }
            {
                auto w_end = clk::now();
                sc_core::sc_time accum = dmi.get_read_latency() * n;
                wait(accum);
                phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
            }
        } else {
            std::printf("    DMI read not allowed — skipping read sub-phase\n");
        }
    } else {
        std::printf("    DMI not granted — skipping phase D\n");
    }
    wait(10, sc_core::SC_NS);

    /* ------------------------------------------------------------------ */
    /* Phase E: b_transport memory writes+reads with wait(1 NS) each.     */
    /* Demonstrates how simulation time advances per transaction.          */
    /* ------------------------------------------------------------------ */
    phase_start("phase E: b_transport memory writes (1 NS gaps)", n);
    sim_t0 = sc_core::sc_time_stamp();
    w0 = clk::now();

    gp.set_command(tlm::TLM_WRITE_COMMAND);
    for (uint64_t i = 0; i < n; ++i) {
        gp.set_address(mem_base + (i * 4) % MEMORY_REGION_SIZE);
        gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        socket->b_transport(gp, delay);
        assert(gp.get_response_status() == tlm::TLM_OK_RESPONSE);
        wait(1, sc_core::SC_NS);
    }
    {
        auto w_end = clk::now();
        wait(delay);
        delay = sc_core::SC_ZERO_TIME;
        phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
    }

    phase_start("phase E: b_transport memory reads (1 NS gaps)", n);
    sim_t0 = sc_core::sc_time_stamp();
    w0 = clk::now();

    gp.set_command(tlm::TLM_READ_COMMAND);
    for (uint64_t i = 0; i < n; ++i) {
        gp.set_address(mem_base + (i * 4) % MEMORY_REGION_SIZE);
        gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        socket->b_transport(gp, delay);
        assert(gp.get_response_status() == tlm::TLM_OK_RESPONSE);
        wait(1, sc_core::SC_NS);
    }
    {
        auto w_end = clk::now();
        wait(delay);
        delay = sc_core::SC_ZERO_TIME;
        phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
    }
    wait(10, sc_core::SC_NS);

    /* ------------------------------------------------------------------ */
    /* Phase F: DMI memory writes+reads with wait(1 NS) each.             */
    /* Shows time advance when DMI is paced.                              */
    /* ------------------------------------------------------------------ */
    phase_start("phase F: DMI memory writes (1 NS gaps)", n);
    tlm::tlm_dmi dmi2;
    gp.set_address(mem_base);
    gp.set_command(tlm::TLM_READ_COMMAND);
    if (socket->get_direct_mem_ptr(gp, dmi2)) {
        uint8_t *base2 = dmi2.get_dmi_ptr();
        std::size_t dmi2_sz =
            (std::size_t)(dmi2.get_end_address() - dmi2.get_start_address() + 1);

        if (dmi2.is_write_allowed()) {
            sim_t0 = sc_core::sc_time_stamp();
            w0 = clk::now();
            for (uint64_t i = 0; i < n; ++i) {
                std::size_t off = (i * 4) % (dmi2_sz > 4 ? dmi2_sz - 4 : 1);
                std::memcpy(base2 + off, data, 4);
                wait(1, sc_core::SC_NS);
            }
            {
                auto w_end = clk::now();
                sc_core::sc_time accum = dmi2.get_write_latency() * n;
                wait(accum);
                phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
            }
            phase_start("phase F: DMI memory reads (1 NS gaps)", n);
        } else {
            std::printf("    DMI write not allowed — skipping write sub-phase\n");
        }

        if (dmi2.is_read_allowed()) {
            sim_t0 = sc_core::sc_time_stamp();
            w0 = clk::now();
            for (uint64_t i = 0; i < n; ++i) {
                std::size_t off = (i * 4) % (dmi2_sz > 4 ? dmi2_sz - 4 : 1);
                std::memcpy(data, base2 + off, 4);
                wait(1, sc_core::SC_NS);
            }
            {
                auto w_end = clk::now();
                sc_core::sc_time accum = dmi2.get_read_latency() * n;
                wait(accum);
                phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
            }
        } else {
            std::printf("    DMI read not allowed — skipping read sub-phase\n");
        }
    } else {
        std::printf("    DMI not granted — skipping phase F\n");
    }

    /* ------------------------------------------------------------------ */
    /* Phase G: naughty register accesses — addr = register_base +         */
    /*          naughty_offset.  N writes then N reads; no per-op wait in  */
    /*          the initiator.  The controller calls wait() inside          */
    /*          b_transport, so sim time advances per transaction.          */
    /* ------------------------------------------------------------------ */
    const uint64_t naughty_addr = reg_base + naughty_offset.get_value();

    phase_start("phase G: naughty register writes", n);
    sim_t0 = sc_core::sc_time_stamp();
    w0 = clk::now();

    gp.set_command(tlm::TLM_WRITE_COMMAND);
    for (uint64_t i = 0; i < n; ++i) {
        gp.set_address(naughty_addr);
        gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        socket->b_transport(gp, delay);
        assert(gp.get_response_status() == tlm::TLM_OK_RESPONSE);
    }
    {
        auto w_end = clk::now();
        wait(delay);
        delay = sc_core::SC_ZERO_TIME;
        phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
    }

    phase_start("phase G: naughty register reads", n);
    sim_t0 = sc_core::sc_time_stamp();
    w0 = clk::now();

    gp.set_command(tlm::TLM_READ_COMMAND);
    for (uint64_t i = 0; i < n; ++i) {
        gp.set_address(naughty_addr);
        gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        socket->b_transport(gp, delay);
        assert(gp.get_response_status() == tlm::TLM_OK_RESPONSE);
    }
    {
        auto w_end = clk::now();
        wait(delay);
        delay = sc_core::SC_ZERO_TIME;
        phase_end(wall_ms(w0, w_end), sc_core::sc_time_stamp() - sim_t0, n);
    }

    /* Every transaction above asserted TLM_OK, so reaching here means the full
     * benchmark completed successfully — the federation oracle. */
    std::printf("SPI BENCHMARK COMPLETE\n");
    std::fflush(stdout);
}
