/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_initiator.h — clean, FSS-independent SystemC CAN initiator, on TLM-2.0.
 *
 * An ordinary TLM-2.0 initiator: a simple_initiator_socket driven by an
 * SC_THREAD that emits `count` CAN frames (id 0x100, 8-byte payload whose
 * first byte is an incrementing counter) as standard generic-payload
 * b_transport calls — one per frame (frame-by-frame), then stops.
 *
 * CAN-as-TLM mapping (standard payload, no extensions):
 *     address = CAN id, data_length = dlc, data_ptr = payload, command = WRITE.
 */
#ifndef SC_CAN_INITIATOR_H
#define SC_CAN_INITIATOR_H

#include <cstdint>
#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>

SC_MODULE(can_initiator) {
    tlm_utils::simple_initiator_socket<can_initiator> socket;

    uint32_t count    = 16;   /* frames to send */
    uint64_t tx_count = 0;

    SC_CTOR(can_initiator) : socket("socket") {
        SC_THREAD(run);
    }

    void run() {
        for (uint32_t i = 0; i < count; ++i) {
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
            if (gp.get_response_status() == tlm::TLM_OK_RESPONSE) ++tx_count;

            sc_core::wait(1, sc_core::SC_NS);
        }
        sc_core::sc_stop();
    }
};

#endif /* SC_CAN_INITIATOR_H */
