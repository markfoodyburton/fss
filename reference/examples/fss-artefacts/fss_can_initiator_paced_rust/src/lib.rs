/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

//! fss_can_initiator_paced_rust — time-synced, flow-controlled CAN initiator (cdylib).
//!
//! Rust port of fss_can_initiator_paced_systemc.  Produces 16 frames (CAN id 0x100,
//! payload[0]=frame-index, payload[1..7]=1..7) paced one-per-quantum, with
//! back-pressure via Status readback: if the controller's FIFO is full (Status!=1)
//! the same frame is retried next tick.  Once all 16 are accepted, idles (keeps the
//! clock alive) until the controller detaches, triggering teardown cascade.
//!
//! R12 — hop1 is a REGISTER WRITE into the controller, and this initiator emits its
//! OWN native carrier for it: rustreg::write ({bus::Data@0 inline(8), bus::Locator
//! u32@8, bus::Length u32@12, bus::Operation u32@16, bus::Status i32@20}), NOT a
//! repacked flat TLM.  All fields are bus:: atoms, so the controller reads it by
//! meta-resolving qnames with no bridge.  bus::Data is carried INLINE (the classic
//! register form: ≤8 bytes), so there is no payload pointer to keep alive.
//!
//!   "socket"   outbound — rustreg::write register-write (round-trip); bound to ctrl/"tx".
//!   "fss/time" — timing-peer port (TimeSync, one downstream peer = controller).

use fss::{Artefact, Carrier, Endpoint, Node, TimeSync};
use std::ffi::c_void;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;

#[allow(dead_code, non_upper_case_globals)]
mod rustreg {
    include!(concat!(env!("FSS_RUSTREG_RS")));
}
use rustreg::*;

const FRAMES: u32 = 16;
const CAN_ID: u32 = 0x100;
const DLC: u32 = 8;
const REG_BYTES: usize = 24; // rustreg::write core data region

/// Stable *mut Endpoint promised to outlive the clock thread (Node owns it).
struct SendPtr(*mut Endpoint);
unsafe impl Send for SendPtr {}

pub struct CanInitiatorPacedRust {
    // Field order matters: Rust drops fields in declaration order, and fss_destroy
    // drops this struct.  _ts MUST drop before node so the clock thread is stopped
    // and joined BEFORE the endpoints it (and peers) deliver into are freed —
    // otherwise a peer's in-flight fss/time window races a freed endpoint.
    _ts: TimeSync,
    node: Node,
}

impl Artefact for CanInitiatorPacedRust {
    fn create(name: &str) -> Self {
        let nm = if name.is_empty() { "rust-init" } else { name };
        let mut node = Node::new();
        node.add_outbound("socket");
        let socket = SendPtr(node.endpoint_ptr("socket"));

        // fss/config: the host pushes "frames" before SIM_STARTED.  Shared with the
        // clock thread's on_tick producer; written once on the host thread before
        // the clock starts, so Relaxed ordering suffices.
        let frames = Arc::new(AtomicU32::new(FRAMES));
        let frames_cfg = frames.clone();
        node.add_endpoint("fss/config", move |c: &mut Carrier| {
            if let Some(v) = fss::config_get_u64(c, "frames") {
                frames_cfg.store(v as u32, Ordering::Relaxed);
            }
            fss::FSS_SEND_OK
        });

        let ts = TimeSync::new(nm)
            .stop_on_peer_detach() // controller detach → we stop → cascade
            .on_tick({
                // `sent` is owned by this FnMut (the clock thread is its sole
                // caller); the socket ptr is captured as usize so the closure is Send.
                let socket = socket.0 as usize;
                let frames = frames.clone();
                let mut sent: u32 = 0;
                move |_now_ps: u64| {
                    if sent >= frames.load(Ordering::Relaxed) {
                        return; // idle — keep clock alive until peer detaches
                    }
                    let i = sent;

                    // Build a rustreg::write register-write; bus::Data carried INLINE.
                    let mut reg = [0u8; REG_BYTES];
                    reg[FSS_RUSTREG_OFF_bus_Data] = (i & 0xFF) as u8; // payload[0] = frame index
                    for b in 1..8usize {
                        reg[FSS_RUSTREG_OFF_bus_Data + b] = b as u8;
                    }
                    reg[FSS_RUSTREG_OFF_bus_Locator..FSS_RUSTREG_OFF_bus_Locator + 4]
                        .copy_from_slice(&CAN_ID.to_le_bytes());
                    reg[FSS_RUSTREG_OFF_bus_Length..FSS_RUSTREG_OFF_bus_Length + 4]
                        .copy_from_slice(&DLC.to_le_bytes());
                    reg[FSS_RUSTREG_OFF_bus_Operation..FSS_RUSTREG_OFF_bus_Operation + 4]
                        .copy_from_slice(&1u32.to_le_bytes()); // Write=1
                    reg[FSS_RUSTREG_OFF_bus_Status..FSS_RUSTREG_OFF_bus_Status + 4]
                        .copy_from_slice(&0i32.to_le_bytes()); // clear before round-trip

                    let mut c = Carrier {
                        id: FSS_RUSTREG_FLAT_ID,
                        data_ptr: reg.as_mut_ptr() as *mut c_void,
                        data_size: REG_BYTES as u64,
                        meta_ptr: FSS_RUSTREG_FLAT_META.as_ptr() as *mut c_void,
                        meta_size: FSS_RUSTREG_FLAT_META.len() as u64,
                    };
                    unsafe {
                        fss::send_on(socket as *mut Endpoint, &mut c);
                    }
                    // Read Status written back by the in-proc controller.
                    let status = i32::from_le_bytes(
                        reg[FSS_RUSTREG_OFF_bus_Status..FSS_RUSTREG_OFF_bus_Status + 4]
                            .try_into()
                            .unwrap(),
                    );
                    if status == 1 {
                        sent += 1; // accepted
                    }
                    // else: full → retry same frame next tick
                }
            });

        // One timing peer: the downstream controller on fss/time.
        ts.install(&mut node, &[""]);

        CanInitiatorPacedRust { _ts: ts, node }
    }

    fn get_endpoint(&mut self, name: &str) -> *mut Endpoint {
        self.node.endpoint_ptr(name)
    }
}

fss::fss_export_artefact!(CanInitiatorPacedRust);
