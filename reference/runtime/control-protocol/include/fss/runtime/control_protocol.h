/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/runtime/control_protocol.h — the orchestrator<->agent control language.
 *
 * The instruction rides the standard, self-describing `control::instr` carrier
 * (protocols/control/, id FSS_CONTROL_ID): a normative FSS carrier with
 * meta, consumed like any other (id-recognise or meta-resolve).  The opcodes
 * below are the C spelling of the standard control::OpCode encoding, so an
 * independently built agent speaks the same vocabulary.  What stays the runtime's
 * own affair (NON-NORMATIVE) is only the PLAN: which instructions it issues, and
 * in what order, to realise a particular topology.
 *
 * The instruction is round-trip: the orchestrator sends a filled fss_ctrl_instr
 * in the carrier's data region, the agent interprets it and writes `status` back
 * in place (control::Status is mutable_by:'target'), and the round-trip returns
 * the mutated struct.  v0.1 carries every field inline, so the data region is
 * the flat struct below; the payload therefore fits the control transport's slot
 * (the reference shm slot is 256 B, a transport property chosen via the
 * breadcrumb, not a property of this protocol).
 */
#ifndef FSS_RUNTIME_CONTROL_PROTOCOL_H
#define FSS_RUNTIME_CONTROL_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes.  Each is interpreted by the agent against its own state; the
 * orchestrator holds the plan and drives the sequence. */
enum {
    FSS_OP_PING      = 0,
    FSS_OP_CONSTRUCT = 1,   /* arg0=node name, arg1=model locator (dll path) */
    FSS_OP_SERVER    = 2,   /* inbound channel: create[-rr] arg1, attach TARGET -> arg0 (node/ep) */
    FSS_OP_CLIENT    = 3,   /* outbound channel: attach[-rr] arg1, attach INITIATOR arg0 (node/ep) -> arg1/arg2 */
    FSS_OP_ARM       = 4,   /* local_sim(arg0 node/ep, SIM_BOUND) */
    FSS_OP_BIND      = 5,   /* in-process edge INSIDE the agent: arg0 (node/ep) .send -> arg1 (node/ep) */
    FSS_OP_STOP      = 6,   /* a non-counting agent's teardown trigger (orchestrator-driven) */
    FSS_OP_DONE      = 9,   /* wiring complete: the agent arms, runs, waits, tears down */
};

/* Per-op flags. */
enum {
    FSS_CFLAG_COUNT    = 1u,   /* OP_SERVER/OP_BIND: wrap the sink with the frame-count oracle */
    FSS_CFLAG_OPTIONAL = 2u,   /* OP_CLIENT: skip (status ok) if the local endpoint is absent */
    /* (bit 4 was FSS_CFLAG_ZENOH: the data-edge transport is now named in the
     * `transport` field below, resolved to a plugin by the agent.) */
};

/* One control instruction.  16 + 64 + 128 + 48 + 32 = 288 (the control
 * transport's slot must fit it; the reference shm slot cap tracks this). */
typedef struct {
    uint32_t op;
    uint32_t status;      /* written back by the agent: 0 = ok */
    uint32_t roundtrip;   /* SERVER/CLIENT: create-rr/attach-rr vs create/attach */
    uint32_t flags;       /* FSS_CFLAG_* */
    char     arg0[64];    /* node name, or node/ep */
    char     arg1[128];   /* model locator (dll path), or channel name */
    char     arg2[48];    /* CLIENT: peer endpoint name (the bind locator's ep) */
    char     transport[32]; /* SERVER/CLIENT: data-edge transport name (fss_transport_name()); "" = default channel */
} fss_ctrl_instr;

#ifdef __cplusplus
}
#endif
#endif
