/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * target_paced.cpp — fss_can_target_paced_systemc artefact entry point.
 *
 * The model is defined in can_target_paced.h (fss_can_models namespace).
 * This file provides the three artefact ABI exports using artefact<can_target_paced>.
 */
#include "fss/artefact_abi.h"
#include "fss/systemc_tlm/artefact.h"

#include "can_target_paced.h"

using artefact_t = fss::systemc_tlm::artefact<fss_can_models::can_target_paced>;

extern "C" FSS_EXPORT fss_node_t *fss_create(fss_transport_t * /*transport*/, const char *name)
{
    return reinterpret_cast<fss_node_t *>(new artefact_t(name));
}

extern "C" FSS_EXPORT fss_endpoint_t *fss_get_endpoint(fss_node_t *handle, const char *endpoint_name)
{
    /* "can_rx" (target socket) + standard "fss/sim" / "fss/time" / "fss/config". */
    return reinterpret_cast<artefact_t *>(handle)->get_endpoint(endpoint_name);
}

extern "C" FSS_EXPORT void fss_destroy(fss_node_t *handle)
{
    delete reinterpret_cast<artefact_t *>(handle);
}
