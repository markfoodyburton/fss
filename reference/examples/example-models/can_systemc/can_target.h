/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_target.h — clean, FSS-independent SystemC CAN target, on standard TLM-2.0.
 *
 * An ordinary TLM-2.0 target: a simple_target_socket with a b_transport
 * callback.  CAN is mapped onto the *standard* generic payload (no custom
 * interface, no extensions):
 *     address      = CAN id
 *     data_length  = dlc
 *     data_ptr     = payload bytes
 *     command      = WRITE (CAN frames are broadcast writes)
 * This is the same mapping the FSS TLM carrier uses, so this target federates
 * unchanged once wrapped as an FSS artefact.
 */
#ifndef SC_CAN_TARGET_H
#define SC_CAN_TARGET_H

#include <cstdint>
#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

SC_MODULE(can_target) {
    tlm_utils::simple_target_socket<can_target> socket;

    /* Observable state (mirrors the FMI/Python/Rust CAN targets). */
    uint32_t last_id    = 0;
    uint8_t  last_byte0 = 0;
    uint8_t  last_dlc   = 0;
    uint64_t rx_count   = 0;

    /* The TLM target socket is named "can_rx" — the CAN receive port — so an FSS
     * wrapper exposes the same inbound endpoint name as the Rust/FMI CAN targets.
     * The C++ member stays `socket`, so the standalone harness is unaffected. */
    SC_CTOR(can_target) : socket("can_rx") {
        socket.register_b_transport(this, &can_target::b_transport);
    }

    void b_transport(tlm::tlm_generic_payload &gp, sc_core::sc_time &) {
        last_id    = static_cast<uint32_t>(gp.get_address());      /* CAN id  */
        last_dlc   = static_cast<uint8_t>(gp.get_data_length());   /* dlc     */
        const uint8_t *d = gp.get_data_ptr();                      /* payload */
        last_byte0 = (last_dlc > 0 && d) ? d[0] : 0;
        ++rx_count;
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};

#endif /* SC_CAN_TARGET_H */
