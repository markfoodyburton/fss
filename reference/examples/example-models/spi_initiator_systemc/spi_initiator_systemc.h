/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* spi_initiator_systemc.h — TLM-2.0 initiator: CCI-parameterised address-space benchmark. */

#pragma once

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <cci_configuration>

#include <cstdint>

class spi_initiator_systemc : public sc_core::sc_module
{
public:
    tlm_utils::simple_initiator_socket<spi_initiator_systemc> socket;

    explicit spi_initiator_systemc(sc_core::sc_module_name n)
        : sc_core::sc_module(n), socket("socket")
    {
        SC_THREAD(run);
    }

    /* Address-space constants (visible to test benches). */
    static constexpr uint64_t REGISTER_REGION_SIZE = 0x1000;   /* 4 KiB */
    static constexpr uint64_t MEMORY_REGION_SIZE   = 0x1000;   /* 4 KiB */

private:
    void run();

    cci::cci_param<uint64_t> n_ops{
        "n_ops", 1000000, "Number of ops per benchmark phase"
    };
    cci::cci_param<uint64_t> register_base{
        "register_base", 0x0, "Base address of the SPI controller register region"
    };
    cci::cci_param<uint64_t> memory_base{
        "memory_base", 0x1000, "Base address of the SPI controller memory region"
    };
    cci::cci_param<uint64_t> naughty_offset{
        "naughty_offset", 0xF00,
        "Register-region offset of the naughty register on the controller"
    };
};
