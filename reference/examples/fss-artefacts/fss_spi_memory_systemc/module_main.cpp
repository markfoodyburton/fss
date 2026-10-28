/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss_spi_memory_systemc — FSS-binding wrap of the spi_memory_systemc model.
 *
 * The model is wrapped directly — it IS the artefact's top module, no
 * scaffolding.  The wrapper discovers its target socket "socket" on
 * get_endpoint(), binds the local bw counterpart (the real initiator is the
 * remote FSS peer), and dispatches b_transport via the target's fw interface.
 */
#include "fss/systemc_tlm/artefact.h"
#include "fss/artefact_abi.h"

#include "spi_memory_systemc.h"

using artefact_t = fss::systemc_tlm::artefact<spi_memory_systemc>;

extern "C" FSS_EXPORT fss_node_t *
fss_create(fss_transport_t * /*transport*/, const char *name)
{
    return reinterpret_cast<fss_node_t *>(new artefact_t(name));
}

extern "C" FSS_EXPORT fss_endpoint_t *
fss_get_endpoint(fss_node_t *handle, const char *endpoint_name)
{
    /* "socket" is the model's target socket; "fss/sim" is the control port. */
    return reinterpret_cast<artefact_t *>(handle)->get_endpoint(endpoint_name);
}

extern "C" FSS_EXPORT void
fss_destroy(fss_node_t *handle)
{
    delete reinterpret_cast<artefact_t *>(handle);
}
