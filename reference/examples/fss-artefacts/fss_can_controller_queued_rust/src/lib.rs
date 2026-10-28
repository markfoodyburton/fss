/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

//! fss_can_controller_queued_rust — time-synced CAN controller with a bounded TX FIFO (cdylib).
//!
//! Rust port of fss_can_controller_queued_systemc.  Accepts a REGISTER WRITE on
//! "tx" from ANY framework's initiator; enqueues a derived CAN frame (cap
//! QUEUE_CAP=4) or writes Status=0 (full, back-pressure) for the initiator to
//! retry.  Drains the FIFO on its own clock at DRAIN_INTERVAL=10 quanta (10µs) via
//! TimeSync::on_tick; emits its OWN native CAN frame one-way on "can_tx".  Two
//! timing peers: upstream initiator (fss/time) and downstream device (fss/time/dev).
//!
//! R12 — the endpoint receive is the SHIM: it META-RESOLVES bus::Operation/Locator/
//! Length/Data (+ optional bus::Status) from the incoming carrier's own meta by
//! qname (NEVER gating on a carrier id), honouring inline vs by-pointer bus::Data,
//! so a SystemC (tlm::transaction, Data by pointer), FMI (fmireg::write), Rust
//! (rustreg::write) or Python (pyreg::write) initiator all interoperate with no
//! bridge.  Its OUTBOUND hop2 is Rust's OWN native carrier, rustcan::frame.
//!
//!   "tx"         inbound  — any framework's register-write (meta-resolved).
//!   "can_tx"     outbound — rustcan::frame (one-way); bound to dev/"can_rx".
//!   "fss/time"   — upstream timing peer (initiator).
//!   "fss/time/dev" — downstream timing peer (device).

use fss::{Artefact, Carrier, Endpoint, Node, TimeSync, FSS_SEND_NO_RECEIVER, FSS_SEND_OK};
use std::collections::VecDeque;
use std::ffi::c_void;
use std::sync::{Arc, Mutex};

#[allow(dead_code, non_upper_case_globals)]
mod rustcan {
    include!(concat!(env!("FSS_RUSTCAN_RS")));
}

const QUEUE_CAP: usize = 4;
const DRAIN_INTERVAL: u64 = 10; // quanta between drain steps (10µs at 1µs/quantum)
const RUSTCAN_BYTES: usize = 16; // rustcan::frame core data region

/// Stable *mut Endpoint promised to outlive the clock thread.
struct SendPtr(*mut Endpoint);
unsafe impl Send for SendPtr {}

/// Read a little-endian unsigned integer of `sz` (≤8) bytes at `off`, or None.
/// Register carriers vary their field widths (u32 vs u64); read the resolved one.
fn read_uint_le(d: &[u8], off: usize, sz: usize) -> Option<u64> {
    let n = sz.min(8);
    let s = d.get(off..off + n)?;
    let mut v = [0u8; 8];
    v[..n].copy_from_slice(s);
    Some(u64::from_le_bytes(v))
}

pub struct CanControllerQueuedRust {
    // Field order matters: drops run in declaration order, so _ts (stop + join the
    // clock thread) MUST precede node (free the endpoints) — else a peer's in-flight
    // fss/time window races a freed endpoint during teardown.
    _ts: TimeSync,
    node: Node,
}

impl Artefact for CanControllerQueuedRust {
    fn create(name: &str) -> Self {
        let nm = if name.is_empty() { "rust-ctrl" } else { name };
        let mut node = Node::new();
        node.add_outbound("can_tx");
        let can_tx = SendPtr(node.endpoint_ptr("can_tx"));

        // FIFO shared between the tx-receive closure (producer's send thread) and
        // the on_tick drain closure (clock thread).  Each entry is a fully-built
        // rustcan::frame (16 bytes).
        let fifo: Arc<Mutex<VecDeque<[u8; RUSTCAN_BYTES]>>> = Arc::new(Mutex::new(VecDeque::new()));
        let fifo_rx = fifo.clone();

        // "tx" inbound: accept ANY framework's register-write by META-RESOLVE (no id
        // gate); enqueue a derived rustcan::frame or back-pressure.
        node.add_endpoint("tx", move |c: &mut Carrier| {
            // SHIM: resolve the bus:: fields (offset, size, inline) by qname.
            let (op_off, op_sz, _) = match unsafe { fss::carrier_field_inline(c, "bus::Operation") } {
                Some(x) => x,
                None => return FSS_SEND_NO_RECEIVER,
            };
            let (lo_off, lo_sz, _) = match unsafe { fss::carrier_field_inline(c, "bus::Locator") } {
                Some(x) => x,
                None => return FSS_SEND_NO_RECEIVER,
            };
            let (len_off, len_sz, _) = match unsafe { fss::carrier_field_inline(c, "bus::Length") } {
                Some(x) => x,
                None => return FSS_SEND_NO_RECEIVER,
            };
            let (data_off, _data_sz, data_inline) =
                match unsafe { fss::carrier_field_inline(c, "bus::Data") } {
                    Some(x) => x,
                    None => return FSS_SEND_NO_RECEIVER,
                };
            // bus::Status is optional (a one-way pyreg::write has none): resolve if present.
            let status = unsafe { fss::carrier_field_inline(c, "bus::Status") };

            let data = unsafe { c.data_mut() };
            let write_status = |data: &mut [u8], s: i32| {
                if let Some((st_off, _, _)) = status {
                    if let Some(slot) = data.get_mut(st_off as usize..st_off as usize + 4) {
                        slot.copy_from_slice(&s.to_le_bytes());
                    }
                }
            };

            let op = read_uint_le(data, op_off as usize, op_sz as usize).unwrap_or(0);

            // In-band back-pressure, two protocols on the same socket:
            //   READ (Operation=0)  → report free FIFO slots (an SC initiator polls
            //                         this and only WRITEs when there is room);
            //   WRITE (Operation=1) → enqueue, or Status=0 if full (a Rust/FMI
            //                         initiator retries on the not-Ok status).
            if op == 0 {
                let free = (QUEUE_CAP - fifo_rx.lock().unwrap().len()) as u32;
                // The free-slots count goes to the initiator's poll buffer.  A poll
                // carrier's bus::Data is by-pointer (the SC live TLM form): the slot
                // holds the buffer address.  (An inline-Data carrier would put it in
                // place — no reg initiator polls, but honour both uniformly.)
                if data_inline {
                    if let Some(slot) = data.get_mut(data_off as usize..data_off as usize + 4) {
                        slot.copy_from_slice(&free.to_le_bytes());
                    }
                } else if let Some(pp) = read_uint_le(data, data_off as usize, 8) {
                    let pptr = pp as *mut u8;
                    if !pptr.is_null() {
                        // SAFETY: pptr is the in-proc initiator's 4-byte poll buffer.
                        unsafe {
                            std::ptr::copy_nonoverlapping(free.to_le_bytes().as_ptr(), pptr, 4)
                        };
                    }
                }
                write_status(data, 1);
                return FSS_SEND_OK;
            }

            // WRITE: read the register values (native), then enqueue a rustcan::frame.
            let id = read_uint_le(data, lo_off as usize, lo_sz as usize).unwrap_or(0) as u32;
            let dlc = read_uint_le(data, len_off as usize, len_sz as usize).unwrap_or(0) as u32;
            let n = dlc.min(8) as usize;
            let mut payload = [0u8; 8];
            if data_inline {
                if let Some(src) = data.get(data_off as usize..data_off as usize + n) {
                    payload[..n].copy_from_slice(src);
                }
            } else if let Some(pp) = read_uint_le(data, data_off as usize, 8) {
                let pptr = pp as *const u8;
                if !pptr.is_null() && n > 0 {
                    // SAFETY: pptr is the in-proc initiator's payload, valid for this call.
                    let src = unsafe { std::slice::from_raw_parts(pptr, n) };
                    payload[..n].copy_from_slice(src);
                }
            }

            {
                let mut q = fifo_rx.lock().unwrap();
                if q.len() >= QUEUE_CAP {
                    // Full: write Status=0 (back-pressure); initiator retries next tick.
                    write_status(data, 0);
                    return FSS_SEND_OK;
                }
                // MODEL → native rustcan::frame {Data@0 inline, Locator u32@8,
                // Length u16@12, can::Ide u8@14, bus::Operation u8@15}.
                let mut frame = [0u8; RUSTCAN_BYTES];
                frame[rustcan::FSS_RUSTCAN_OFF_bus_Data..rustcan::FSS_RUSTCAN_OFF_bus_Data + n]
                    .copy_from_slice(&payload[..n]);
                frame[rustcan::FSS_RUSTCAN_OFF_bus_Locator..rustcan::FSS_RUSTCAN_OFF_bus_Locator + 4]
                    .copy_from_slice(&id.to_le_bytes());
                frame[rustcan::FSS_RUSTCAN_OFF_bus_Length..rustcan::FSS_RUSTCAN_OFF_bus_Length + 2]
                    .copy_from_slice(&(dlc as u16).to_le_bytes());
                frame[rustcan::FSS_RUSTCAN_OFF_can_Ide] = 0; // Standard
                frame[rustcan::FSS_RUSTCAN_OFF_bus_Operation] = 0; // rtr: Data-frame
                q.push_back(frame);
            }
            write_status(data, 1); // accepted
            FSS_SEND_OK
        });

        // Drain closure: runs on the clock thread every DRAIN_INTERVAL quanta.
        let ts = TimeSync::new(nm)
            .stop_on_peer_detach() // device detach → relay stops → opens window upstream
            .on_tick({
                let fifo_drain = fifo.clone();
                let can_tx = can_tx.0 as usize; // as usize so the closure is Send
                let mut tick_count: u64 = 0;
                move |_now_ps: u64| {
                    tick_count += 1;
                    if tick_count % DRAIN_INTERVAL != 0 {
                        return;
                    }
                    let frame_opt = {
                        let mut q = fifo_drain.lock().unwrap();
                        q.pop_front()
                    };
                    if let Some(mut frame) = frame_opt {
                        let mut fc = Carrier {
                            id: rustcan::FSS_RUSTCAN_FLAT_ID,
                            data_ptr: frame.as_mut_ptr() as *mut c_void,
                            data_size: frame.len() as u64,
                            meta_ptr: rustcan::FSS_RUSTCAN_FLAT_META.as_ptr() as *mut c_void,
                            meta_size: rustcan::FSS_RUSTCAN_FLAT_META.len() as u64,
                        };
                        unsafe {
                            fss::send_on(can_tx as *mut Endpoint, &mut fc);
                        }
                    }
                }
            });

        // Two timing peers: upstream initiator ("" = fss/time), downstream device ("dev").
        ts.install(&mut node, &["", "dev"]);

        CanControllerQueuedRust { _ts: ts, node }
    }

    fn get_endpoint(&mut self, name: &str) -> *mut Endpoint {
        self.node.endpoint_ptr(name)
    }
}

fss::fss_export_artefact!(CanControllerQueuedRust);
