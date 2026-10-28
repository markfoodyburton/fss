/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * combined.cpp — fss_can_combined_systemc: a single-kernel artefact that builds
 * all three CAN models (initiator + controller + device) inside ONE SystemC kernel
 * and wires them internally via FSS endpoints.
 *
 * This demonstrates two properties of the FSS architecture:
 *   1. "An artefact builds multiple models" — one fss_create call, one kernel
 *      thread, three sc_module instances in one sc_simcontext.
 *   2. FSS is ~free when models are co-located — same-thread FSS sends (init→ctrl,
 *      ctrl→dev) hit the SAME-THREAD FAST PATH in endpoint_receive, bypassing the
 *      doorbell/queue handoff used for cross-kernel federation.
 *
 * Hub topology:
 *   can_combined
 *     ├── init  (can_initiator_paced)  — outbound "init/socket"  → ctrl.tx
 *     ├── ctrl  (can_controller_queued) — inbound  "ctrl/tx"
 *     │                                  outbound  "ctrl/can_tx" → dev.can_rx
 *     └── dev   (can_target_paced)     — inbound  "dev/can_rx"
 *
 * Endpoint naming:
 *   The binding's get_endpoint("init/socket") maps '/' → '.' →
 *   "<hubname>.init.socket" under sc_find_object.
 *   "ctrl/tx"     → "<hubname>.ctrl.tx"    (target socket, inbound)
 *   "ctrl/can_tx" → endpoint_cache hit: registered by hub's fss_attach as
 *                   the controller's outbound (NOT auto-discovered via sc_find_object
 *                   because can_tx is not a TLM socket — it is a raw fss_endpoint_t*).
 *   "dev/can_rx"  → "<hubname>.dev.can_rx"  (target socket, inbound)
 *
 * The "ctrl/can_tx" outbound is declared by the hub's fss_attach under its namespaced
 * name (not by the controller's own fss_attach, which would register it as "can_tx"
 * and make it unreachable under the combined artefact's namespace).
 *
 * fss_configure is forwarded to init and dev so "frames"/"period_us" reach both
 * (the controller carries no run-length config).
 */

#include "fss/artefact_abi.h"
#include "fss/systemc_tlm/artefact.h"

#include "can_initiator_paced.h"
#include "can_controller_queued.h"
#include "can_target_paced.h"

#include <systemc>

/* Hub sc_module: builds the three models as named sub-modules.
 * Derives fss_model_hooks so the binding calls fss_attach after construction,
 * giving the hub a chance to register the controller's outbound under the
 * namespaced name "ctrl/can_tx". */
struct can_combined : sc_core::sc_module,
                      fss::systemc_tlm::fss_model_hooks {
    fss_can_models::can_initiator_paced   init{"init"};
    fss_can_models::can_controller_queued ctrl{"ctrl"};
    fss_can_models::can_target_paced      dev{"dev"};

    SC_CTOR(can_combined) {}

    /* Declare the controller's non-TLM outbound under its namespaced name.
     * The get_endpoint cache-lookup checks name == "ctrl/can_tx" and returns this
     * endpoint; the binding's transport installs its send pointer here.
     * We do NOT call ctrl.fss_attach — that would register "can_tx" (the bare name)
     * as a second entry, which get_endpoint("ctrl/can_tx") would never match. */
    void fss_attach(fss::systemc_tlm::fss_emitter &binding) override {
        ctrl.can_tx = binding.declare_outbound("ctrl/can_tx");
    }

    /* Forward config to initiator and device so "frames" / "period_us" reach both.
     * The controller carries no run-length config (its QUEUE_CAP/DRAIN_INTERVAL are
     * compile-time constants). */
    void fss_configure(const char *json) override {
        init.fss_configure(json);
        dev.fss_configure(json);
    }

    /* No external time peers in a single-kernel combined run; fss_peer_detached
     * will not fire.  Default (return false) is correct. */
};

using artefact_t = fss::systemc_tlm::artefact<can_combined>;

extern "C" FSS_EXPORT fss_node_t *fss_create(fss_transport_t * /*transport*/, const char *name)
{
    return reinterpret_cast<fss_node_t *>(new artefact_t(name));
}

extern "C" FSS_EXPORT fss_endpoint_t *fss_get_endpoint(fss_node_t *handle, const char *endpoint_name)
{
    /* Namespaced endpoints: "init/socket", "ctrl/tx", "ctrl/can_tx",
     * "dev/can_rx", and the standard "fss/sim" / "fss/time" / "fss/config". */
    return reinterpret_cast<artefact_t *>(handle)->get_endpoint(endpoint_name);
}

extern "C" FSS_EXPORT void fss_destroy(fss_node_t *handle)
{
    delete reinterpret_cast<artefact_t *>(handle);
}
