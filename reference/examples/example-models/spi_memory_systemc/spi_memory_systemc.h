/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* spi_memory_systemc.h — SPI-like TLM target with a backing byte array and DMI support. */

#pragma once

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

#include <cci_configuration>

#include <cstddef>
#include <vector>

class spi_memory_systemc : public sc_core::sc_module
{
public:
    tlm_utils::simple_target_socket<spi_memory_systemc> socket;

    cci::cci_param<std::size_t>      size{"size", 4096, "Memory size in bytes"};
    cci::cci_param<sc_core::sc_time> access_latency{"access_latency",
        sc_core::SC_ZERO_TIME, "Per-b_transport access latency"};
    cci::cci_param<sc_core::sc_time> dmi_read_latency{"dmi_read_latency",
        sc_core::sc_time(5, sc_core::SC_NS), "DMI read latency reported in DMI grant"};
    cci::cci_param<sc_core::sc_time> dmi_write_latency{"dmi_write_latency",
        sc_core::sc_time(5, sc_core::SC_NS), "DMI write latency reported in DMI grant"};

    explicit spi_memory_systemc(sc_core::sc_module_name n);

private:
    void         b_transport(tlm::tlm_generic_payload &gp, sc_core::sc_time &t);
    bool         get_direct_mem_ptr(tlm::tlm_generic_payload &gp, tlm::tlm_dmi &dmi);
    unsigned int transport_dbg(tlm::tlm_generic_payload &gp);

    std::vector<uint8_t> mem_;
};
