/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * inproc.h — in-process FSS transport plugin.
 *
 * This is a loadable transport PLUGIN: it exports the well-known factory
 * symbols `fss_transport_create` / `fss_transport_destroy` (declared in
 * fss/transport_abi.h), which the host resolves after dlopening this module.
 * The host does not link or include this transport — it is selected at deploy
 * time by path.  There is no inproc-specific public API; this header exists
 * only to document the module.
 *
 * Implements the normative fss_transport_t vtable (fss/transport_abi.h):
 *   channel(name, round_trip, owner, hints, n) — create/find one channel
 *   attach(channel, endpoint, role)            — join as INITIATOR or TARGET
 *   send_to(channel, carrier)                  — one-shot delivery
 *
 * The reference implementation has one channel per transport instance.  A 1:1
 * channel direct-wires initiator send to target receive.  A one-way channel
 * with two or more targets uses an in-process fan-out shim.  A round-trip
 * channel refuses a second target because write-back would be ambiguous.
 * Artefacts are constructed by the placement facility; this plugin has no node
 * registry and no inproc-specific public API.
 */
#ifndef FSS_INPROC_H
#define FSS_INPROC_H

#include "fss/transport_abi.h"   /* fss_transport_create / fss_transport_destroy */

#endif
