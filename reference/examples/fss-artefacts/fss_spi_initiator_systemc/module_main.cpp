/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss_spi_initiator_systemc — FSS-binding wrap of example-models/spi_initiator_systemc,
 * the heavyweight CCI/DMI benchmark initiator.
 *
 * Used to drive the SPI chain (initiator -> controller -> memory) as a
 * *benchmark* federation: it exercises both the controller's register region
 * and (forwarded) memory region with real b_transport traffic. Two caveats it
 * handles gracefully across FSS:
 *   - DMI does NOT cross FSS (the wrapper's initiator stub declines
 *     get_direct_mem_ptr), so its DMI phases self-skip and fall back to
 *     b_transport — which is the point of testing it across a federation.
 *   - its default is 1,000,000 ops/phase; the federation sends a config carrier
 *     ("n_ops") to the fss/config port to dial that down for a quick test.
 *
 * Every transaction asserts TLM_OK; reaching the end prints "SPI BENCHMARK
 * COMPLETE" — the federation oracle.
 */
#include "fss/systemc_tlm/artefact.h"
#include "fss/artefact_abi.h"

#include "spi_initiator_systemc.h"

using artefact_t = fss::systemc_tlm::artefact<spi_initiator_systemc>;

extern "C" FSS_EXPORT fss_node_t *
fss_create(fss_transport_t * /*transport*/, const char *name)
{
    return reinterpret_cast<fss_node_t *>(new artefact_t(name));
}

extern "C" FSS_EXPORT fss_endpoint_t *
fss_get_endpoint(fss_node_t *handle, const char *endpoint_name)
{
    /* "socket" is the initiator socket; "fss/sim" the control port. */
    return reinterpret_cast<artefact_t *>(handle)->get_endpoint(endpoint_name);
}

extern "C" FSS_EXPORT void
fss_destroy(fss_node_t *handle)
{
    delete reinterpret_cast<artefact_t *>(handle);
}
