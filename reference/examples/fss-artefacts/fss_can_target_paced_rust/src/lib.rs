/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

//! fss_can_target_paced_rust — a TIME-SYNCED Rust CAN device (cdylib artefact).
//!
//! The Rust counterpart of fss_can_target_paced_systemc: the plain Rust CAN rx
//! target plus a windowed INVENTED clock (fss::TimeSync) so it is a first-class
//! federation timing peer — it honours the time::window protocol on `fss/time`
//! against its peer (the controller), staying within one quantum.  Being the SINK,
//! it also drives teardown: on receiving the expected frames it stops its clock,
//! which sends an open_window (detach) to the controller, cascading the stop.
//!
//! R12 — the endpoint receive is the SHIM: it accepts ANY framework's CAN frame
//! (sccan/fmican/rustcan/pycan — four distinct byte layouts) by META-RESOLVING
//! bus::Locator/bus::Data from the carrier's own meta (never gating on a carrier
//! id, never a fixed offset), then hands the model a native (id, first-byte)
//! pair.  The model has no FSS code — it just counts.
//!
//!   "can_rx"   inbound — any CAN frame (bus::-composed; meta-resolved).
//!   "fss/time" — the binding-provided timing-peer port (TimeSync).
//!
//! Prints "RUST CAN TARGET PASS: rx=16 last_id=0x100 last_b0=15" (the ctest
//! oracle) and the TimeSync skew line.

use fss::{Artefact, Carrier, Endpoint, Node, TimeSync, FSS_SEND_OK};
use std::io::Write;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;

const EXPECT_FRAMES: u32 = 16;

/// Read a little-endian unsigned integer of `sz` (≤8) bytes at `off`, or None.
/// A CAN id is u32 in some frames, u64 in others — read the meta-resolved width.
fn read_uint_le(d: &[u8], off: usize, sz: usize) -> Option<u64> {
    let n = sz.min(8);
    let s = d.get(off..off + n)?;
    let mut v = [0u8; 8];
    v[..n].copy_from_slice(s);
    Some(u64::from_le_bytes(v))
}

pub struct CanTargetPacedRust {
    // Field order matters: drops run in declaration order, so _ts (stop + join the
    // clock thread) MUST precede node (free the endpoints) — else a peer's in-flight
    // fss/time window races a freed endpoint during teardown.
    _ts: TimeSync,
    node: Node,
}

impl Artefact for CanTargetPacedRust {
    fn create(name: &str) -> Self {
        let nm = if name.is_empty() { "rust-dev" } else { name };
        let ts = TimeSync::new(nm);
        let stop = ts.handle();

        let mut node = Node::new();

        // fss/config: the host pushes "frames" (how many to expect) before
        // SIM_STARTED.  Shared with the receive closure; written once on the host
        // thread before any frame arrives, so Relaxed suffices.
        let expect = Arc::new(AtomicU32::new(EXPECT_FRAMES));
        let expect_cfg = expect.clone();
        node.add_endpoint("fss/config", move |c: &mut Carrier| {
            if let Some(v) = fss::config_get_u64(c, "frames") {
                expect_cfg.store(v as u32, Ordering::Relaxed);
            }
            FSS_SEND_OK
        });

        // rx state owned by the receive closure; `stop` (a TsHandle) lets the sink
        // halt its clock on completion → open_window → teardown cascade.
        let mut count: u32 = 0;
        let mut last_id: u32 = 0;
        let mut last_b0: u8 = 0;
        node.add_endpoint("can_rx", move |c: &mut Carrier| {
            // SHIM: meta-resolve the frame's fields by qname (id-agnostic) → native
            // (id, first payload byte).  bus::Data is INLINE in every CAN frame, but
            // resolve the layout and honour a by-pointer form too (uniform with hop1).
            let decoded = (|| -> Option<(u32, u8)> {
                let (lo_off, lo_sz, _) = unsafe { fss::carrier_field_inline(c, "bus::Locator") }?;
                let (d_off, _d_sz, d_inline) = unsafe { fss::carrier_field_inline(c, "bus::Data") }?;
                let data = unsafe { c.data() };
                let id = read_uint_le(data, lo_off as usize, lo_sz as usize)? as u32;
                let b0 = if d_inline {
                    *data.get(d_off as usize)?
                } else {
                    let p = read_uint_le(data, d_off as usize, 8)? as *const u8;
                    if p.is_null() { return None; }
                    unsafe { *p }
                };
                Some((id, b0))
            })();
            let (id, b0) = match decoded {
                Some(x) => x,
                None => return FSS_SEND_OK, // meta absent/foreign — drop, never guess
            };

            // MODEL: count + remember (no FSS types here).
            last_id = id;
            last_b0 = b0;
            count += 1;
            if count == expect.load(Ordering::Relaxed) {
                println!(
                    "RUST CAN TARGET PASS: rx={count} last_id=0x{last_id:X} last_b0={last_b0}"
                );
                let _ = std::io::stdout().flush();
                stop.signal_stop(); // sink done → detach → cascade teardown
            }
            FSS_SEND_OK
        });

        // One timing peer (the upstream controller), on the standard fss/time port.
        ts.install(&mut node, &[""]);

        CanTargetPacedRust { _ts: ts, node }
    }

    fn get_endpoint(&mut self, name: &str) -> *mut Endpoint {
        self.node.endpoint_ptr(name)
    }
}

fss::fss_export_artefact!(CanTargetPacedRust);
