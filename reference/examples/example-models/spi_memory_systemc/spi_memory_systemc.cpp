/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* spi_memory_systemc.cpp — TLM target backed by a byte array; grants DMI over the full region. */

#include "spi_memory_systemc.h"

#include <cstring>

spi_memory_systemc::spi_memory_systemc(sc_core::sc_module_name n)
    : sc_core::sc_module(n), socket("socket"), mem_(size, 0)
{
    socket.register_b_transport(this, &spi_memory_systemc::b_transport);
    socket.register_get_direct_mem_ptr(this, &spi_memory_systemc::get_direct_mem_ptr);
    socket.register_transport_dbg(this, &spi_memory_systemc::transport_dbg);
}

void spi_memory_systemc::b_transport(tlm::tlm_generic_payload &gp, sc_core::sc_time &t)
{
    sc_dt::uint64 addr = gp.get_address();
    unsigned int  len  = gp.get_data_length();

    if (addr + len > mem_.size()) {
        gp.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        return;
    }

    switch (gp.get_command()) {
        case tlm::TLM_READ_COMMAND:
            std::memcpy(gp.get_data_ptr(), mem_.data() + addr, len);
            break;
        case tlm::TLM_WRITE_COMMAND:
            std::memcpy(mem_.data() + addr, gp.get_data_ptr(), len);
            break;
        default:
            gp.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
            return;
    }
    gp.set_response_status(tlm::TLM_OK_RESPONSE);
    t += access_latency;
}

bool spi_memory_systemc::get_direct_mem_ptr(tlm::tlm_generic_payload &gp, tlm::tlm_dmi &dmi)
{
    /* Grants DMI for the whole region; the spi_controller is responsible for
       gating which addresses are DMI-eligible (memory region only). */
    (void)gp;
    dmi.set_granted_access(tlm::tlm_dmi::DMI_ACCESS_READ_WRITE);
    dmi.set_dmi_ptr(mem_.data());
    dmi.set_start_address(0);
    dmi.set_end_address(static_cast<sc_dt::uint64>(mem_.size() - 1));
    dmi.set_read_latency(dmi_read_latency);
    dmi.set_write_latency(dmi_write_latency);
    return true;
}

unsigned int spi_memory_systemc::transport_dbg(tlm::tlm_generic_payload &gp)
{
    sc_dt::uint64 addr = gp.get_address();
    unsigned int  len  = gp.get_data_length();
    if (addr + len > mem_.size()) return 0;

    switch (gp.get_command()) {
        case tlm::TLM_READ_COMMAND:
            std::memcpy(gp.get_data_ptr(), mem_.data() + addr, len);
            break;
        case tlm::TLM_WRITE_COMMAND:
            std::memcpy(mem_.data() + addr, gp.get_data_ptr(), len);
            break;
        default:
            return 0;
    }
    return len;
}
