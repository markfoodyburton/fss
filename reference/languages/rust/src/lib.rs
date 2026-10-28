/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

//! # fss — Rust language binding for the Federated Simulation Standard
//!
//! This is a **language binding** (per `languages/README.md`): the
//! carrier-agnostic glue that lets a model written in Rust be an FSS node. It
//! mirrors the reference C ABI (`abi/include/fss/*.h`) as `#[repr(C)]` types and
//! provides the scaffolding to author an **artefact** — a `cdylib` the
//! binary-loader transports (`transports/inproc`, …) `dlopen` and drive.
//!
//! It is deliberately NOT a self-contained loopback demo (those were removed):
//! an artefact built with this binding is loaded by a real transport and
//! federated against models in other languages. See `examples/` for consumers.
//!
//! ## Authoring an artefact
//!
//! ```ignore
//! use fss::{Artefact, Carrier, Endpoint, Node, FSS_SEND_OK};
//!
//! struct MyTarget { node: Node }
//!
//! impl Artefact for MyTarget {
//!     fn create(_name: &str) -> Self {
//!         let mut node = Node::new();
//!         node.add_endpoint("in", |c: &mut Carrier| {
//!             // mutate c.data() in place, return a status
//!             FSS_SEND_OK
//!         });
//!         MyTarget { node }
//!     }
//!     fn get_endpoint(&mut self, name: &str) -> *mut Endpoint {
//!         self.node.endpoint_ptr(name)
//!     }
//! }
//!
//! fss::fss_export_artefact!(MyTarget);   // emits fss_create / fss_get_endpoint / fss_destroy
//! ```
//!
//! ## Authoring an initiator (outbound + start hook)
//!
//! An initiator holds an OUTBOUND endpoint (the transport installs its `send` at
//! bind) and a START hook on the standard `fss/sim` port; on `Started` it emits
//! its carriers autonomously — the "send N on start" the central-deploy host
//! relies on:
//!
//! ```ignore
//! let mut node = Node::new();
//! node.add_outbound("socket");
//! let socket = node.endpoint_ptr("socket");          // stable; send filled at bind
//! node.on_start(move || {
//!     for i in 0..16 {
//!         let mut c = /* build the carrier */;
//!         unsafe { fss::send_on(socket, &mut c); }   // out to the bound peer
//!     }
//! });
//! ```

use std::collections::HashMap;
use std::ffi::c_void;
use std::os::raw::c_int;

/// Transport status codes (mirror `enum` in `abi/include/fss/transport_abi.h`).
pub const FSS_SEND_OK: c_int = 0;
pub const FSS_SEND_UNBOUND: c_int = 1;
pub const FSS_SEND_NO_RECEIVER: c_int = 2;

/// sim::EventCode values (atoms/sim/EventCode.libsonnet default_encoding) at
/// byte 0 of a sim::event carrier.
pub const SIM_BOUND: u32 = 0;
pub const SIM_ELABORATED: u32 = 1;
pub const SIM_STARTED: u32 = 2;
pub const SIM_STOPPED: u32 = 5;

/// Mirror of `fss_carrier_t` (`abi/include/fss/carrier.h`). `data_ptr`/`meta_ptr`
/// are process pointers in memory (offsets on the wire). Strict 40-byte layout.
#[repr(C)]
pub struct Carrier {
    pub id: u64,
    pub data_ptr: *mut c_void,
    pub data_size: u64,
    pub meta_ptr: *mut c_void,
    pub meta_size: u64,
}

impl Carrier {
    /// The data region as a byte slice (empty if null/zero-length).
    ///
    /// # Safety
    /// The carrier must be valid for the duration of the call (the FSS carrier
    /// lifetime contract: valid only inside `receive`).
    pub unsafe fn data(&self) -> &[u8] {
        if self.data_ptr.is_null() || self.data_size == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(self.data_ptr as *const u8, self.data_size as usize)
        }
    }

    /// Mutable view of the data region (for in-place response / mutation).
    ///
    /// # Safety
    /// As [`Carrier::data`]; the caller must not retain the slice past `receive`.
    pub unsafe fn data_mut(&mut self) -> &mut [u8] {
        if self.data_ptr.is_null() || self.data_size == 0 {
            &mut []
        } else {
            std::slice::from_raw_parts_mut(self.data_ptr as *mut u8, self.data_size as usize)
        }
    }
}

// ===========================================================================
// Carrier-meta resolver — the binding's "get me the xyz atom" primitive.
//
// Per docs/spec/05-abi-conventions.md ("Consuming a carrier's fields"): a
// receiver resolves a field's (byte_offset, byte_size) by qname from the
// carrier's OWN CBOR meta.  The meta MUST be present by the time the carrier
// reaches the artefact (a transport may strip it on the wire, but restores it
// before delivery); the carrier id is only a cache key, never a gate.  Every
// interface goes through here EXCEPT the C++ TLM live fast path (which the Rust
// binding has no analogue for — a Rust node is always a flat consumer).
//
// This is a pure-Rust reader for the closed CBOR profile tools/fss_compile.py
// emits — a definite-length array of definite-length maps with tstr keys.  It
// mirrors fss/c/meta.c's `fss_meta_find` semantics; `core_meta_consts_match_ids`
// proves byte-for-byte agreement by re-hashing the generated metas.  Pure Rust
// keeps the binding C-free, like every Rust artefact.
// ===========================================================================

/// Bounded CBOR cursor (`ok` clears on any malformed/out-of-bounds read).
struct MetaCur<'a> {
    b: &'a [u8],
    i: usize,
    ok: bool,
}

impl<'a> MetaCur<'a> {
    fn byte(&mut self) -> u8 {
        if self.i >= self.b.len() {
            self.ok = false;
            0
        } else {
            let v = self.b[self.i];
            self.i += 1;
            v
        }
    }

    /// Read a CBOR head: (major type, argument) — definite forms only.
    fn head(&mut self) -> (u8, u64) {
        let ib = self.byte();
        if !self.ok {
            return (0, 0);
        }
        let major = (ib >> 5) & 0x07;
        let ai = ib & 0x1f;
        let arg = match ai {
            0..=23 => ai as u64,
            24 => self.byte() as u64,
            25 => {
                let hi = self.byte() as u64;
                let lo = self.byte() as u64;
                (hi << 8) | lo
            }
            26 => {
                let mut v = 0u64;
                for _ in 0..4 {
                    v = (v << 8) | self.byte() as u64;
                }
                v
            }
            27 => {
                let mut v = 0u64;
                for _ in 0..8 {
                    v = (v << 8) | self.byte() as u64;
                }
                v
            }
            _ => {
                self.ok = false; // indefinite / reserved — not in the profile
                0
            }
        };
        (major, arg)
    }

    /// Skip one complete data item (any profile type).
    fn skip(&mut self) {
        let (major, arg) = self.head();
        if !self.ok {
            return;
        }
        match major {
            0 | 1 | 7 => {} // uint / negint / simple — value (or none) in the head
            2 | 3 => {
                if self.b.len() - self.i < arg as usize {
                    self.ok = false;
                    return;
                }
                self.i += arg as usize; // bstr / tstr payload
            }
            4 => {
                for _ in 0..arg {
                    if !self.ok {
                        break;
                    }
                    self.skip();
                }
            }
            5 => {
                for _ in 0..arg {
                    if !self.ok {
                        break;
                    }
                    self.skip();
                    self.skip();
                }
            }
            _ => self.ok = false,
        }
    }
}

/// Resolve `qname` in a CBOR `meta`, returning (byte_offset, byte_size, is_inline),
/// or None if absent / a sub-meta (no data bytes) / malformed.  `is_inline` is true
/// when the field carries its value DIRECTLY in the data region (a plain scalar or
/// inline bstr — no "layout" attribute), false when it is carried BY REFERENCE
/// (layout "fss:local-ptr" — the live TLM Data slot holds the payload's address).
/// Mirrors `fss_meta_find` + the C binding's `data_is_inline` (empty layout = inline).
fn meta_find_full(meta: &[u8], qname: &str) -> Option<(u32, u32, bool)> {
    let mut c = MetaCur { b: meta, i: 0, ok: true };
    let (major, n_entries) = c.head();
    if !c.ok || major != 4 {
        return None; // top level must be an array
    }
    for _ in 0..n_entries {
        if !c.ok {
            return None;
        }
        let (em, n_pairs) = c.head();
        if !c.ok || em != 5 {
            return None; // each entry is a map
        }
        let mut found = false;
        let mut off: Option<u64> = None;
        let mut size: Option<u64> = None;
        let mut layout_len: u64 = 0; // >0 → carried by reference (fss:local-ptr)
        for _ in 0..n_pairs {
            if !c.ok {
                return None;
            }
            let (km, klen) = c.head();
            if !c.ok || km != 3 {
                return None; // key is a tstr
            }
            if c.b.len() - c.i < klen as usize {
                return None;
            }
            let key = &c.b[c.i..c.i + klen as usize];
            c.i += klen as usize;
            match key {
                b"qname" => {
                    let (vm, vlen) = c.head();
                    if !c.ok || vm != 3 || c.b.len() - c.i < vlen as usize {
                        return None;
                    }
                    found = vlen as usize == qname.len()
                        && &c.b[c.i..c.i + vlen as usize] == qname.as_bytes();
                    c.i += vlen as usize;
                }
                b"byte_offset" => {
                    let (vm, v) = c.head();
                    if !c.ok || vm != 0 {
                        return None;
                    }
                    off = Some(v);
                }
                b"byte_size" => {
                    let (vm, v) = c.head();
                    if !c.ok || vm != 0 {
                        return None;
                    }
                    size = Some(v);
                }
                b"layout" => {
                    // A tstr layout ("fss:local-ptr[-nul]" / "sub-meta"); its presence
                    // (non-empty) means the value is NOT carried inline.
                    let (vm, vlen) = c.head();
                    if !c.ok || vm != 3 || c.b.len() - c.i < vlen as usize {
                        return None;
                    }
                    layout_len = vlen;
                    c.i += vlen as usize;
                }
                _ => c.skip(), // wire / version / encoding / attrs / sub-meta
            }
        }
        if c.ok && found {
            // A sub-meta entry has no byte_offset/byte_size — not a data field.
            return match (off, size) {
                (Some(o), Some(s)) => Some((o as u32, s as u32, layout_len == 0)),
                _ => None,
            };
        }
    }
    None
}

/// Resolve `qname` in a CBOR `meta`, returning (byte_offset, byte_size), or None.
/// Thin wrapper over [`meta_find_full`] dropping the layout flag (the common case).
fn meta_find(meta: &[u8], qname: &str) -> Option<(u32, u32)> {
    meta_find_full(meta, qname).map(|(o, s, _)| (o, s))
}

/// The binding's field-resolution primitive: locate `qname` in the carrier's own
/// meta.  Returns (byte_offset, byte_size), or None if the meta is absent (the
/// carrier contract requires it present at the artefact), foreign, or malformed.
///
/// # Safety
/// `c.meta_ptr`/`meta_size` must describe a valid buffer for the call (the FSS
/// carrier lifetime contract: valid only inside `receive`).
pub unsafe fn carrier_field(c: &Carrier, qname: &str) -> Option<(u32, u32)> {
    if c.meta_ptr.is_null() || c.meta_size == 0 {
        return None; // meta MUST be present by the time it reaches the artefact
    }
    let meta = std::slice::from_raw_parts(c.meta_ptr as *const u8, c.meta_size as usize);
    meta_find(meta, qname)
}

/// As [`carrier_field`], but also reports whether the field is carried INLINE
/// (value in the data region) or BY REFERENCE (the slot holds a pointer —
/// layout "fss:local-ptr", e.g. the live TLM `bus::Data`).  The receive shim of a
/// non-C++ consumer needs this to read a foreign carrier's payload the right way:
/// an inline `bus::Data` (a register carrier's ≤8 bytes) is the value; a by-pointer
/// `bus::Data` (a live TLM GP) is the payload's address to dereference.  This is the
/// Rust analogue of the C binding's `fss_meta_field_at().layout` / `data_is_inline`.
///
/// # Safety
/// As [`carrier_field`].
pub unsafe fn carrier_field_inline(c: &Carrier, qname: &str) -> Option<(u32, u32, bool)> {
    if c.meta_ptr.is_null() || c.meta_size == 0 {
        return None; // meta MUST be present by the time it reaches the artefact
    }
    let meta = std::slice::from_raw_parts(c.meta_ptr as *const u8, c.meta_size as usize);
    meta_find_full(meta, qname)
}

/// A small (id, qname) → (offset, size) cache over [`carrier_field`], so a hot
/// receiver (e.g. the time-window port) resolves each field once then O(1).  The
/// id is the cache key — exactly the "hash optimises the meta-walk" of the spec.
/// One per consuming thread/endpoint (the C resolver's contract).
#[derive(Default)]
struct FieldCache {
    slots: Vec<(u64, &'static str, u32, u32)>,
}

impl FieldCache {
    /// Resolve `qname` (a static atom name) against the carrier's meta, caching by
    /// (carrier id, qname).
    ///
    /// # Safety
    /// As [`carrier_field`].
    unsafe fn resolve(&mut self, c: &Carrier, qname: &'static str) -> Option<(u32, u32)> {
        for &(id, q, o, s) in &self.slots {
            if id == c.id && q == qname {
                return Some((o, s));
            }
        }
        let (o, s) = carrier_field(c, qname)?;
        if self.slots.len() < 64 {
            self.slots.push((c.id, qname, o, s));
        }
        Some((o, s))
    }
}

/// Read a little-endian u32 at `off` from `data`, if in bounds.
fn read_u32_le(data: &[u8], off: usize) -> Option<u32> {
    data.get(off..off + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()))
}

/// Read a little-endian u64 at `off` from `data`, if in bounds.
fn read_u64_le(data: &[u8], off: usize) -> Option<u64> {
    data.get(off..off + 8).map(|b| u64::from_le_bytes(b.try_into().unwrap()))
}

/// Delivery callback — both inbound (`receive`) and outbound (`send`).
/// `Option<extern "C" fn ...>` is FFI-identical to the C function pointer
/// (`NULL` = `None`), matching `fss_carrier_fn`.
pub type CarrierFn = unsafe extern "C" fn(self_: *mut c_void, carrier: *mut Carrier) -> c_int;

/// Mirror of `fss_endpoint_t` (`abi/include/fss/endpoint.h`). 32-byte layout.
/// `send`/`send_ctx` are installed by the transport at bind; the binding only
/// ever fills `self_`/`receive`.
#[repr(C)]
pub struct Endpoint {
    pub self_: *mut c_void,
    pub receive: Option<CarrierFn>,
    pub send: Option<CarrierFn>,
    pub send_ctx: *mut c_void,
}

/// A Rust receive handler: invoked with the inbound carrier, returns a status.
type Handler = Box<dyn FnMut(&mut Carrier) -> c_int>;

/// One endpoint plus its Rust handler, heap-pinned (stable address) so the C
/// `Endpoint.self_` pointer we hand the transport stays valid.
struct EndpointEntry {
    ep: Endpoint,
    handler: Handler,
}

/// Static trampoline installed as `Endpoint.receive`: recovers the entry from
/// `self_` and dispatches to the Rust handler.
unsafe extern "C" fn receive_trampoline(self_: *mut c_void, carrier: *mut Carrier) -> c_int {
    if self_.is_null() || carrier.is_null() {
        return FSS_SEND_NO_RECEIVER;
    }
    let entry = &mut *(self_ as *mut EndpointEntry);
    (entry.handler)(&mut *carrier)
}

/// Resolves a reserved endpoint name the model never declared (e.g. an
/// `fss/sim/<tag>` downstream sink) into a fresh inbound handler on first request,
/// or `None` if the name is none of its business.  Installed via [`Node::set_fallback`].
type EndpointResolver = Box<dyn FnMut(&str) -> Option<Handler>>;

/// An FSS node: a set of named endpoints, each with a Rust receive handler.
/// An artefact owns a `Node` and resolves endpoints from it by name.
#[derive(Default)]
pub struct Node {
    endpoints: HashMap<String, Box<EndpointEntry>>,
    /// Consulted by [`endpoint_ptr`](Node::endpoint_ptr) for an unknown name, so a
    /// binding (e.g. [`TimeSync`]) can materialise reserved control sinks on demand.
    fallback: Option<EndpointResolver>,
}

impl Node {
    pub fn new() -> Self {
        Node::default()
    }

    /// Insert an inbound endpoint carrying `handler`, heap-pinned so the
    /// `Endpoint.self_` pointer we hand the transport stays valid.
    fn insert_handler(&mut self, name: &str, handler: Handler) {
        let mut entry = Box::new(EndpointEntry {
            ep: Endpoint {
                self_: std::ptr::null_mut(),
                receive: Some(receive_trampoline),
                send: None,
                send_ctx: std::ptr::null_mut(),
            },
            handler,
        });
        // self_ points at the heap-pinned entry (stable across the Box move into
        // the map: only the Box pointer moves, not the EndpointEntry).
        entry.ep.self_ = (&mut *entry as *mut EndpointEntry) as *mut c_void;
        self.endpoints.insert(name.to_string(), entry);
    }

    /// Register an inbound endpoint named `name` whose `receive` runs `handler`.
    pub fn add_endpoint<F>(&mut self, name: &str, handler: F)
    where
        F: FnMut(&mut Carrier) -> c_int + 'static,
    {
        self.insert_handler(name, Box::new(handler));
    }

    /// Install a resolver for reserved endpoint names the model didn't declare.
    /// [`endpoint_ptr`](Node::endpoint_ptr) consults it once per unknown name and
    /// caches whatever inbound handler it returns.
    pub fn set_fallback<F>(&mut self, f: F)
    where
        F: FnMut(&str) -> Option<Handler> + 'static,
    {
        self.fallback = Some(Box::new(f));
    }

    /// Register an OUTBOUND endpoint named `name`: it carries no receive handler
    /// (nothing is delivered inbound on an outbound endpoint); the transport
    /// installs `send`/`send_ctx` on it at bind, and the model sends on it via
    /// [`send_on`] (typically from a [`Node::on_start`] hook).
    pub fn add_outbound(&mut self, name: &str) {
        let entry = Box::new(EndpointEntry {
            ep: Endpoint {
                self_: std::ptr::null_mut(),
                receive: None, // outbound: no inbound delivery
                send: None,    // installed by the transport at bind
                send_ctx: std::ptr::null_mut(),
            },
            handler: Box::new(|_| FSS_SEND_NO_RECEIVER), // never invoked (receive = None)
        });
        self.endpoints.insert(name.to_string(), entry);
    }

    /// Register a start hook on the standard `fss/sim` control port: `f` runs
    /// once a `sim::event{Started}` carrier is delivered there (the host closing
    /// the bind window).  An initiator captures its outbound endpoint pointer(s)
    /// by move and emits its frames from `f` — the autonomous "send N on start"
    /// the central-deploy host relies on.
    pub fn on_start<F>(&mut self, mut f: F)
    where
        F: FnMut() + 'static,
    {
        self.add_endpoint("fss/sim", move |c: &mut Carrier| {
            // SAFETY: the carrier is valid for the duration of this call.  Walk the
            // carrier's meta for sim::EventCode (the sender attaches it).
            if let Some((off, _)) = unsafe { carrier_field(c, "sim::EventCode") } {
                let data = unsafe { c.data() };
                if read_u32_le(data, off as usize) == Some(SIM_STARTED) {
                    f();
                }
            }
            FSS_SEND_OK
        });
    }

    /// Resolve a named endpoint to a stable `*mut Endpoint` (or null).
    /// The transport installs `send`/`send_ctx` on the returned struct at bind.
    /// An unknown name is offered to the fallback resolver once (which may
    /// materialise a reserved control sink, e.g. `fss/sim/<tag>`).
    pub fn endpoint_ptr(&mut self, name: &str) -> *mut Endpoint {
        if !self.endpoints.contains_key(name) {
            if let Some(mut f) = self.fallback.take() {
                let made = f(name);
                self.fallback = Some(f);
                if let Some(handler) = made {
                    self.insert_handler(name, handler);
                }
            }
        }
        match self.endpoints.get_mut(name) {
            Some(e) => &mut e.ep as *mut Endpoint,
            None => std::ptr::null_mut(),
        }
    }
}

/// Send a carrier on an outbound endpoint whose `send`/`send_ctx` the transport
/// installed at bind.  Returns the transport status, or `FSS_SEND_UNBOUND` if
/// nothing is bound yet.
///
/// # Safety
/// `ep` must be a valid endpoint pointer (e.g. from [`Node::endpoint_ptr`]) and
/// `carrier` a valid carrier for the duration of the call.
pub unsafe fn send_on(ep: *mut Endpoint, carrier: *mut Carrier) -> c_int {
    if ep.is_null() {
        return FSS_SEND_UNBOUND;
    }
    let e = &*ep;
    match e.send {
        Some(send) => send(e.send_ctx, carrier),
        None => FSS_SEND_UNBOUND,
    }
}

/// Implemented by an artefact type; `fss_export_artefact!` wires it to the C ABI.
pub trait Artefact {
    /// Construct the artefact (the transport passes its instance name).
    fn create(name: &str) -> Self
    where
        Self: Sized;
    /// Resolve a named endpoint (typically `self.node.endpoint_ptr(name)`).
    fn get_endpoint(&mut self, name: &str) -> *mut Endpoint;
}

/// Emit the C artefact ABI (`fss_create` / `fss_get_endpoint` / `fss_destroy`)
/// for a type implementing [`Artefact`]. Place once in an artefact `cdylib`.
#[macro_export]
macro_rules! fss_export_artefact {
    ($t:ty) => {
        /// # Safety: called by the transport with a valid name (or null).
        #[no_mangle]
        pub unsafe extern "C" fn fss_create(
            _transport: *mut ::core::ffi::c_void,
            name: *const ::core::ffi::c_char,
        ) -> *mut ::core::ffi::c_void {
            let n = if name.is_null() {
                String::new()
            } else {
                ::core::ffi::CStr::from_ptr(name).to_string_lossy().into_owned()
            };
            let boxed: Box<$t> = Box::new(<$t as $crate::Artefact>::create(&n));
            Box::into_raw(boxed) as *mut ::core::ffi::c_void
        }

        /// # Safety: `node` is a handle returned by `fss_create`.
        #[no_mangle]
        pub unsafe extern "C" fn fss_get_endpoint(
            node: *mut ::core::ffi::c_void,
            name: *const ::core::ffi::c_char,
        ) -> *mut $crate::Endpoint {
            if node.is_null() || name.is_null() {
                return ::core::ptr::null_mut();
            }
            let art = &mut *(node as *mut $t);
            let nm = ::core::ffi::CStr::from_ptr(name).to_string_lossy();
            <$t as $crate::Artefact>::get_endpoint(art, &nm)
        }

        /// # Safety: `node` is a handle returned by `fss_create`.
        #[no_mangle]
        pub unsafe extern "C" fn fss_destroy(node: *mut ::core::ffi::c_void) {
            if !node.is_null() {
                drop(Box::from_raw(node as *mut $t));
            }
        }
    };
}

// ===========================================================================
// Time-sync: a native-Rust windowed invented clock (the non-C++ analogue of the
// SystemC binding's sc_sync_window).  A Rust model has no kernel, so its clock is
// INVENTED — a `now` counter a background thread advances.  To be a first-class
// federation timing peer it must HONOUR the time::window protocol on `fss/time`:
//
//   * receive a peer's window (time::Unit, time::Lower, time::Upper): "run at
//     least to Lower, do NOT pass Upper".  We track the min Upper over peers as
//     our `granted_upper` and never advance past it.
//   * advance `now` toward granted_upper in quantum steps, and after each step
//     send the peer OUR window {now, now+quantum} so it can extend — the
//     symmetric one-way exchange.  When `now` reaches granted_upper we block
//     until a peer extends it.
//   * on stop, send an open_window (Upper = UINT64_MAX sentinel) to every peer —
//     the detach that cascades teardown (a peer whose producer/consumer is gone
//     stops too).
//
// The per-hop skew stays within one quantum (worst-case bounded by cross-thread
// window-delivery lag); each node prints the same machine-checkable line the
// SystemC binding does, asserted by the federation test.
//
// Carrier: time::window (protocols/time-sync/window.jsonnet) — FSS_TIME_ID,
// wire layout int32 Unit@0, uint64 Lower@8, uint64 Upper@16 (24 bytes).  These
// mirror the generated fss/protocols/time_sync.h; a layout test pins them.

use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;

/// The time::window carrier id (FSS_TIME_ID, from window.jsonnet).  A fixed core
/// protocol the binding owns, like [`SIM_STARTED`]; must match the generated
/// `fss/protocols/time_sync.h`.
pub const FSS_TIME_ID: u64 = 0xf13a_7a68_c87a_6127;

/// Canonical CBOR meta for time::window — mirrors the generated
/// `fss/protocols/time_sync.h` FSS_TIME_META, attached on every window send
/// so a peer can walk it (FNV-1a-64 == [`FSS_TIME_ID`]; pinned by
/// `core_meta_consts_match_ids`).
pub const FSS_TIME_META: &[u8] = &[
    0x84, 0xa6, 0x64, 0x77, 0x69, 0x72, 0x65, 0x6b, 0x69, 0x6e, 0x74, 0x20,
    0x2e, 0x73, 0x69, 0x7a, 0x65, 0x20, 0x34, 0x65, 0x71, 0x6e, 0x61, 0x6d,
    0x65, 0x6a, 0x74, 0x69, 0x6d, 0x65, 0x3a, 0x3a, 0x55, 0x6e, 0x69, 0x74,
    0x67, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x01, 0x68, 0x65, 0x6e,
    0x63, 0x6f, 0x64, 0x69, 0x6e, 0x67, 0xa6, 0x66, 0x53, 0x65, 0x63, 0x6f,
    0x6e, 0x64, 0x05, 0x6a, 0x4e, 0x61, 0x6e, 0x6f, 0x73, 0x65, 0x63, 0x6f,
    0x6e, 0x64, 0x02, 0x6a, 0x50, 0x69, 0x63, 0x6f, 0x73, 0x65, 0x63, 0x6f,
    0x6e, 0x64, 0x01, 0x6b, 0x46, 0x65, 0x6d, 0x74, 0x6f, 0x73, 0x65, 0x63,
    0x6f, 0x6e, 0x64, 0x00, 0x6b, 0x4d, 0x69, 0x63, 0x72, 0x6f, 0x73, 0x65,
    0x63, 0x6f, 0x6e, 0x64, 0x03, 0x6b, 0x4d, 0x69, 0x6c, 0x6c, 0x69, 0x73,
    0x65, 0x63, 0x6f, 0x6e, 0x64, 0x04, 0x69, 0x62, 0x79, 0x74, 0x65, 0x5f,
    0x73, 0x69, 0x7a, 0x65, 0x04, 0x6b, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x6f,
    0x66, 0x66, 0x73, 0x65, 0x74, 0x00, 0xa5, 0x64, 0x77, 0x69, 0x72, 0x65,
    0x6c, 0x75, 0x69, 0x6e, 0x74, 0x20, 0x2e, 0x73, 0x69, 0x7a, 0x65, 0x20,
    0x38, 0x65, 0x71, 0x6e, 0x61, 0x6d, 0x65, 0x6b, 0x74, 0x69, 0x6d, 0x65,
    0x3a, 0x3a, 0x4c, 0x6f, 0x77, 0x65, 0x72, 0x67, 0x76, 0x65, 0x72, 0x73,
    0x69, 0x6f, 0x6e, 0x01, 0x69, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x73, 0x69,
    0x7a, 0x65, 0x08, 0x6b, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x6f, 0x66, 0x66,
    0x73, 0x65, 0x74, 0x08, 0xa5, 0x64, 0x77, 0x69, 0x72, 0x65, 0x6c, 0x75,
    0x69, 0x6e, 0x74, 0x20, 0x2e, 0x73, 0x69, 0x7a, 0x65, 0x20, 0x38, 0x65,
    0x71, 0x6e, 0x61, 0x6d, 0x65, 0x6b, 0x74, 0x69, 0x6d, 0x65, 0x3a, 0x3a,
    0x55, 0x70, 0x70, 0x65, 0x72, 0x67, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f,
    0x6e, 0x01, 0x69, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x73, 0x69, 0x7a, 0x65,
    0x08, 0x6b, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x6f, 0x66, 0x66, 0x73, 0x65,
    0x74, 0x10, 0xa3, 0x65, 0x71, 0x6e, 0x61, 0x6d, 0x65, 0x70, 0x63, 0x6f,
    0x72, 0x65, 0x3a, 0x3a, 0x45, 0x78, 0x74, 0x65, 0x6e, 0x73, 0x69, 0x6f,
    0x6e, 0x73, 0x66, 0x6c, 0x61, 0x79, 0x6f, 0x75, 0x74, 0x68, 0x73, 0x75,
    0x62, 0x2d, 0x6d, 0x65, 0x74, 0x61, 0x67, 0x76, 0x65, 0x72, 0x73, 0x69,
    0x6f, 0x6e, 0x01,
];

/// The sim::event carrier id (FSS_SIM_ID, from sim/event.jsonnet).
pub const FSS_SIM_ID: u64 = 0x893d_9179_e7c9_9450;

/// Canonical CBOR meta for sim::event — mirrors the generated
/// `fss/protocols/sim.h` FSS_SIM_META, attached on every sim send so the
/// receiver can walk it (FNV-1a-64 == [`FSS_SIM_ID`]; pinned by
/// `core_meta_consts_match_ids`).
pub const FSS_SIM_META: &[u8] = &[
    0x83, 0xa6, 0x64, 0x77, 0x69, 0x72, 0x65, 0x6c, 0x75, 0x69, 0x6e, 0x74,
    0x20, 0x2e, 0x73, 0x69, 0x7a, 0x65, 0x20, 0x34, 0x65, 0x71, 0x6e, 0x61,
    0x6d, 0x65, 0x6e, 0x73, 0x69, 0x6d, 0x3a, 0x3a, 0x45, 0x76, 0x65, 0x6e,
    0x74, 0x43, 0x6f, 0x64, 0x65, 0x67, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f,
    0x6e, 0x01, 0x68, 0x65, 0x6e, 0x63, 0x6f, 0x64, 0x69, 0x6e, 0x67, 0xa6,
    0x65, 0x42, 0x6f, 0x75, 0x6e, 0x64, 0x00, 0x66, 0x50, 0x61, 0x75, 0x73,
    0x65, 0x64, 0x03, 0x67, 0x52, 0x65, 0x73, 0x75, 0x6d, 0x65, 0x64, 0x04,
    0x67, 0x53, 0x74, 0x61, 0x72, 0x74, 0x65, 0x64, 0x02, 0x67, 0x53, 0x74,
    0x6f, 0x70, 0x70, 0x65, 0x64, 0x05, 0x6a, 0x45, 0x6c, 0x61, 0x62, 0x6f,
    0x72, 0x61, 0x74, 0x65, 0x64, 0x01, 0x69, 0x62, 0x79, 0x74, 0x65, 0x5f,
    0x73, 0x69, 0x7a, 0x65, 0x04, 0x6b, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x6f,
    0x66, 0x66, 0x73, 0x65, 0x74, 0x00, 0xa6, 0x64, 0x77, 0x69, 0x72, 0x65,
    0x6c, 0x75, 0x69, 0x6e, 0x74, 0x20, 0x2e, 0x73, 0x69, 0x7a, 0x65, 0x20,
    0x34, 0x65, 0x71, 0x6e, 0x61, 0x6d, 0x65, 0x6b, 0x73, 0x69, 0x6d, 0x3a,
    0x3a, 0x53, 0x74, 0x61, 0x74, 0x75, 0x73, 0x67, 0x76, 0x65, 0x72, 0x73,
    0x69, 0x6f, 0x6e, 0x01, 0x68, 0x65, 0x6e, 0x63, 0x6f, 0x64, 0x69, 0x6e,
    0x67, 0xa2, 0x62, 0x4f, 0x6b, 0x00, 0x66, 0x46, 0x61, 0x69, 0x6c, 0x65,
    0x64, 0x01, 0x69, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x73, 0x69, 0x7a, 0x65,
    0x04, 0x6b, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x6f, 0x66, 0x66, 0x73, 0x65,
    0x74, 0x04, 0xa3, 0x65, 0x71, 0x6e, 0x61, 0x6d, 0x65, 0x70, 0x63, 0x6f,
    0x72, 0x65, 0x3a, 0x3a, 0x45, 0x78, 0x74, 0x65, 0x6e, 0x73, 0x69, 0x6f,
    0x6e, 0x73, 0x66, 0x6c, 0x61, 0x79, 0x6f, 0x75, 0x74, 0x68, 0x73, 0x75,
    0x62, 0x2d, 0x6d, 0x65, 0x74, 0x61, 0x67, 0x76, 0x65, 0x72, 0x73, 0x69,
    0x6f, 0x6e, 0x01,
];

/// Canonical CBOR meta for config::param_request — mirrors the generated
/// `fss/protocols/config.h` FSS_CONFIG_META, attached on the `{Get}` carrier a
/// node sends to PULL its config so the responder/transport can walk it (FNV-1a-64 ==
/// [`FSS_CONFIG_ID`]; pinned by `core_meta_consts_match_ids`).
pub const FSS_CONFIG_META: &[u8] = &[
    0x85, 0xa6, 0x64, 0x77, 0x69, 0x72, 0x65, 0x6c, 0x75, 0x69, 0x6e, 0x74,
    0x20, 0x2e, 0x73, 0x69, 0x7a, 0x65, 0x20, 0x34, 0x65, 0x71, 0x6e, 0x61,
    0x6d, 0x65, 0x6e, 0x63, 0x6f, 0x6e, 0x66, 0x69, 0x67, 0x3a, 0x3a, 0x4f,
    0x70, 0x43, 0x6f, 0x64, 0x65, 0x67, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f,
    0x6e, 0x01, 0x68, 0x65, 0x6e, 0x63, 0x6f, 0x64, 0x69, 0x6e, 0x67, 0xa3,
    0x63, 0x47, 0x65, 0x74, 0x00, 0x63, 0x53, 0x65, 0x74, 0x01, 0x68, 0x44,
    0x65, 0x73, 0x63, 0x72, 0x69, 0x62, 0x65, 0x02, 0x69, 0x62, 0x79, 0x74,
    0x65, 0x5f, 0x73, 0x69, 0x7a, 0x65, 0x04, 0x6b, 0x62, 0x79, 0x74, 0x65,
    0x5f, 0x6f, 0x66, 0x66, 0x73, 0x65, 0x74, 0x00, 0xa6, 0x64, 0x77, 0x69,
    0x72, 0x65, 0x64, 0x74, 0x73, 0x74, 0x72, 0x65, 0x71, 0x6e, 0x61, 0x6d,
    0x65, 0x71, 0x63, 0x6f, 0x6e, 0x66, 0x69, 0x67, 0x3a, 0x3a, 0x50, 0x61,
    0x72, 0x61, 0x6d, 0x4e, 0x61, 0x6d, 0x65, 0x66, 0x6c, 0x61, 0x79, 0x6f,
    0x75, 0x74, 0x71, 0x66, 0x73, 0x73, 0x3a, 0x6c, 0x6f, 0x63, 0x61, 0x6c,
    0x2d, 0x70, 0x74, 0x72, 0x2d, 0x6e, 0x75, 0x6c, 0x67, 0x76, 0x65, 0x72,
    0x73, 0x69, 0x6f, 0x6e, 0x01, 0x69, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x73,
    0x69, 0x7a, 0x65, 0x08, 0x6b, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x6f, 0x66,
    0x66, 0x73, 0x65, 0x74, 0x08, 0xa5, 0x64, 0x77, 0x69, 0x72, 0x65, 0x64,
    0x62, 0x73, 0x74, 0x72, 0x65, 0x71, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x63,
    0x6f, 0x6e, 0x66, 0x69, 0x67, 0x3a, 0x3a, 0x50, 0x61, 0x72, 0x61, 0x6d,
    0x56, 0x61, 0x6c, 0x75, 0x65, 0x67, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f,
    0x6e, 0x01, 0x69, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x73, 0x69, 0x7a, 0x65,
    0x18, 0x28, 0x6b, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x6f, 0x66, 0x66, 0x73,
    0x65, 0x74, 0x10, 0xa6, 0x64, 0x77, 0x69, 0x72, 0x65, 0x6c, 0x75, 0x69,
    0x6e, 0x74, 0x20, 0x2e, 0x73, 0x69, 0x7a, 0x65, 0x20, 0x34, 0x65, 0x71,
    0x6e, 0x61, 0x6d, 0x65, 0x6e, 0x63, 0x6f, 0x6e, 0x66, 0x69, 0x67, 0x3a,
    0x3a, 0x53, 0x74, 0x61, 0x74, 0x75, 0x73, 0x67, 0x76, 0x65, 0x72, 0x73,
    0x69, 0x6f, 0x6e, 0x01, 0x68, 0x65, 0x6e, 0x63, 0x6f, 0x64, 0x69, 0x6e,
    0x67, 0xa4, 0x62, 0x4f, 0x6b, 0x00, 0x68, 0x4e, 0x6f, 0x74, 0x46, 0x6f,
    0x75, 0x6e, 0x64, 0x01, 0x68, 0x52, 0x65, 0x61, 0x64, 0x4f, 0x6e, 0x6c,
    0x79, 0x03, 0x6c, 0x54, 0x79, 0x70, 0x65, 0x4d, 0x69, 0x73, 0x6d, 0x61,
    0x74, 0x63, 0x68, 0x02, 0x69, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x73, 0x69,
    0x7a, 0x65, 0x04, 0x6b, 0x62, 0x79, 0x74, 0x65, 0x5f, 0x6f, 0x66, 0x66,
    0x73, 0x65, 0x74, 0x18, 0x38, 0xa3, 0x65, 0x71, 0x6e, 0x61, 0x6d, 0x65,
    0x70, 0x63, 0x6f, 0x72, 0x65, 0x3a, 0x3a, 0x45, 0x78, 0x74, 0x65, 0x6e,
    0x73, 0x69, 0x6f, 0x6e, 0x73, 0x66, 0x6c, 0x61, 0x79, 0x6f, 0x75, 0x74,
    0x68, 0x73, 0x75, 0x62, 0x2d, 0x6d, 0x65, 0x74, 0x61, 0x67, 0x76, 0x65,
    0x72, 0x73, 0x69, 0x6f, 0x6e, 0x01,
];

/// The config::param_request carrier id (from config/param_request.jsonnet).  A
/// fixed core protocol the binding owns; must match the generated
/// `fss/protocols/config.h` (FSS_CONFIG_ID).
pub const FSS_CONFIG_ID: u64 = 0x1f3d_d8fa_e716_1678;

/// Extract an unsigned-integer value by key from a config::param_request carrier
/// whose ParamValue is a flat JSON object (the CAN platform's config form).
/// Resolves config::ParamValue's offset from the carrier's own meta (the walk —
/// no hard-coded offset, no id gate); returns None if the meta is absent/foreign
/// or the key is missing.  Mirrors `fss/c/cfg_json.h`'s `fss_cfg_json_get_u64`.
pub fn config_get_u64(c: &Carrier, key: &str) -> Option<u64> {
    let (off, _sz) = unsafe { carrier_field(c, "config::ParamValue") }?;
    let data = unsafe { c.data() };
    if data.len() <= off as usize {
        return None;
    }
    let pv = &data[off as usize..];
    let end = pv.iter().position(|&b| b == 0).unwrap_or(pv.len());
    let json = std::str::from_utf8(&pv[..end]).ok()?;
    // Find "key" then the following decimal integer.
    let needle = format!("\"{key}\"");
    let after = &json[json.find(&needle)? + needle.len()..];
    let after = after.trim_start();
    let after = after.strip_prefix(':')?.trim_start();
    let digits: String = after.chars().take_while(|c| c.is_ascii_digit()).collect();
    if digits.is_empty() {
        None
    } else {
        digits.parse::<u64>().ok()
    }
}

/// Sync quantum in picoseconds (1 µs) — matches the SystemC binding's global
/// quantum, so a Rust node and an SC node stay within one quantum across a hop.
pub const TIME_QUANTUM_PS: u64 = 1_000_000;

const TIME_UNIT_PS: i32 = 1; // sc_time_unit exponent: Picosecond
const TIME_OPEN_UPPER: u64 = u64::MAX; // time::Upper sentinel = open window (detach)
const TW_OFF_UNIT: usize = 0;
const TW_OFF_LOWER: usize = 8;
const TW_OFF_UPPER: usize = 16;
const TW_SIZE: usize = 24;

/// Convert a (unit, value) tick count to picoseconds.  Absolute fs = value *
/// 1000^unit (per window.jsonnet); ps = fs / 1000.  SC sends unit=PS (1), so this
/// is the identity there; other units are converted for interop.
fn ticks_to_ps(unit: i32, value: u64) -> u64 {
    if unit >= 1 {
        // value * 1000^(unit-1) ps
        let mut v = value as u128;
        for _ in 0..(unit - 1) {
            v = v.saturating_mul(1000);
        }
        v.min(u64::MAX as u128) as u64
    } else {
        // unit 0 = femtoseconds: value/1000 ps
        value / 1000
    }
}

/// A raw `*mut Endpoint` we promise is heap-pinned (stable) and outlives the
/// clock thread (the Node owns it; `fss_destroy` joins the thread first).
struct SendPtr(*mut Endpoint);
unsafe impl Send for SendPtr {}

struct PeerSlot {
    ep: SendPtr,       // for sending OUR window (send installed by transport at bind)
    seen: bool,        // has sent us a real (bounded) window
    last_from_ps: u64, // its last-reported clock (for skew)
    upper_ps: u64,     // its latest Upper in ps (u64::MAX = detached/open); 0 = none yet
}

struct TsState {
    now_ps: u64,
    granted_upper_ps: u64,
    peers: Vec<PeerSlot>,
    max_skew_ps: u64,
    stop: bool,
    thread: Option<JoinHandle<()>>,
    // Wave state (START BARRIER) — sim topology, independent of the time peers
    wired: bool,
    downstream_ready: usize,
    n_sim_down: usize,             // count of materialised "fss/sim/<tag>" sinks
    n_downstream: usize,           // == n_sim_down once classified; one Elaborated each
    classified: bool,
    sim_ep: SendPtr,               // fss/sim UP-report port (Elaborated is sent here)
    cfg_ep: SendPtr,               // fss/config port (null if the model has none); PULLed at wired
    config_pulled: bool,           // one-shot guard for the wired-time config pull
    field_cache: FieldCache,       // (id,qname)→offset for the time-window walk
}

struct TsInner {
    name: String,
    quantum_ps: u64,
    stop_on_detach: bool, // a peer detach stops us (relay/source teardown policy)
    // Per-quantum model work (a producer's pacing / a relay's drain), invoked once
    // per advanced quantum from the clock loop OFF the state lock with the new
    // `now` (ps).  None for a pure sink (its receive closure does the work).
    on_tick: Mutex<Option<Box<dyn FnMut(u64) + Send>>>,
    st: Mutex<TsState>,
    cv: Condvar,
}

/// A windowed invented clock for a Rust FSS node.  Create one, [`TimeSync::install`]
/// it onto the [`Node`] (adds the `fss/time` peer port(s) and the start hook), and
/// optionally signal it to stop via a [`TsHandle`].  Held by the artefact so its
/// `Drop` stops + joins the clock thread.
pub struct TimeSync {
    inner: Arc<TsInner>,
}

/// A cloneable handle to signal the clock (e.g. a data endpoint that has received
/// everything signals stop, triggering the detach cascade).
#[derive(Clone)]
pub struct TsHandle(Arc<TsInner>);

impl TsHandle {
    /// Stop the clock: it sends an open_window to every peer (detach) and exits.
    pub fn signal_stop(&self) {
        let mut st = self.0.st.lock().unwrap();
        st.stop = true;
        self.0.cv.notify_all();
    }
}

impl TimeSync {
    /// Create a clock named `name` (used in the skew report line).
    pub fn new(name: &str) -> TimeSync {
        let q = TIME_QUANTUM_PS;
        TimeSync {
            inner: Arc::new(TsInner {
                name: name.to_string(),
                quantum_ps: q,
                stop_on_detach: false,
                on_tick: Mutex::new(None),
                st: Mutex::new(TsState {
                    now_ps: 0,
                    // bootstrap: free to run to one quantum before any peer window
                    granted_upper_ps: q,
                    peers: Vec::new(),
                    max_skew_ps: 0,
                    stop: false,
                    thread: None,
                    wired: false,
                    downstream_ready: 0,
                    n_sim_down: 0,
                    n_downstream: 0,
                    classified: false,
                    sim_ep: SendPtr(std::ptr::null_mut()),
                    cfg_ep: SendPtr(std::ptr::null_mut()),
                    config_pulled: false,
                    field_cache: FieldCache::default(),
                }),
                cv: Condvar::new(),
            }),
        }
    }

    /// If set, a peer DETACHING (open_window) stops this node too — the teardown
    /// cascade for a relay (consumer gone) or source (only sink gone).  A pure
    /// sink leaves this false and stops itself on completion.
    pub fn stop_on_peer_detach(mut self) -> TimeSync {
        Arc::get_mut(&mut self.inner)
            .expect("call before install")
            .stop_on_detach = true;
        self
    }

    /// Register per-quantum model work (a producer's pace+poll, a relay's drain).
    /// `f` is called once per advanced quantum from the clock thread, OFF the state
    /// lock, with the new `now` in ps — so it may itself `send_on` (e.g. a poll
    /// round-trip).  A pure sink omits this (its receive closure does the work).
    pub fn on_tick<F>(self, f: F) -> TimeSync
    where
        F: FnMut(u64) + Send + 'static,
    {
        *self.inner.on_tick.lock().unwrap() = Some(Box::new(f));
        self
    }

    pub fn handle(&self) -> TsHandle {
        TsHandle(self.inner.clone())
    }

    /// Add the timing-peer port(s) (`""` → `fss/time`, `"dev"` → `fss/time/dev`)
    /// and the `fss/sim` control port that drives the upward readiness wave.
    /// Call after the model's own data endpoints are added (endpoint addresses
    /// are stable).  The `fss/time` peers are SYMMETRIC and directionless; the
    /// sim chain (who is downstream of whom) is described entirely by the
    /// `fss/sim` / `fss/sim/<tag>` wiring, not by these tags.
    pub fn install(&self, node: &mut Node, peer_tags: &[&str]) {
        for (i, tag) in peer_tags.iter().enumerate() {
            let name = if tag.is_empty() {
                "fss/time".to_string()
            } else {
                format!("fss/time/{}", tag)
            };
            let inner = self.inner.clone();
            let idx = i;
            node.add_endpoint(&name, move |c: &mut Carrier| {
                ts_on_window(&inner, idx, c);
                FSS_SEND_OK
            });
            let ep = node.endpoint_ptr(&name);
            self.inner.st.lock().unwrap().peers.push(PeerSlot {
                ep: SendPtr(ep),
                seen: false,
                last_from_ps: 0,
                upper_ps: 0,
            });
        }
        // fss/sim — the node's UP-report port: its send is bound by the orchestrator
        // to our upstream (a peer's fss/sim/<tag> sink, or the orchestrator if we are
        // the source), and it also receives the local SIM_BOUND kick.
        let inner = self.inner.clone();
        node.add_endpoint("fss/sim", move |c: &mut Carrier| {
            // Walk the carrier's meta for sim::EventCode (the sender attaches it).
            if let Some((off, _)) = unsafe { carrier_field(c, "sim::EventCode") } {
                let data = unsafe { c.data() };
                if let Some(code) = read_u32_le(data, off as usize) {
                    ts_on_sim(&inner, code);
                }
            }
            FSS_SEND_OK
        });
        // Capture the fss/sim endpoint ptr — Elaborated is always reported UP here.
        let sim_ep = node.endpoint_ptr("fss/sim");
        self.inner.st.lock().unwrap().sim_ep = SendPtr(sim_ep);

        // fss/config — the model's config port (if it declared one).  Captured here
        // (stable address); at wired we PULL on it: send a {Get} to the orchestrator's
        // single responder and feed the in-place reply to this port's own receive,
        // which applies it (the in-band replacement for the old config push).  Null
        // for a model that declares no config port — pull is then a no-op.  The
        // fallback declines a non-"fss/sim/" name, so an absent port resolves to null.
        let cfg_ep = node.endpoint_ptr("fss/config");
        self.inner.st.lock().unwrap().cfg_ep = SendPtr(cfg_ep);

        // fss/sim/<tag> — one inbound sink per DOWNSTREAM peer, materialised lazily
        // when the orchestrator resolves it (the source's downstream sink has no
        // time-peer tag, so sink names cannot be derived from peer_tags).  Each
        // counts toward n_downstream; its Elaborated feeds the run gate.  This
        // REPLACES head-detection + time-topology classify.
        let inner_dl = self.inner.clone();
        node.set_fallback(move |name: &str| -> Option<Handler> {
            if !name.starts_with("fss/sim/") {
                return None;
            }
            inner_dl.st.lock().unwrap().n_sim_down += 1;
            let inner_h = inner_dl.clone();
            Some(Box::new(move |c: &mut Carrier| {
                if let Some((off, _)) = unsafe { carrier_field(c, "sim::EventCode") } {
                    let data = unsafe { c.data() };
                    if let Some(code) = read_u32_le(data, off as usize) {
                        ts_on_sim(&inner_h, code);
                    }
                }
                FSS_SEND_OK
            }))
        });
    }
}

impl Drop for TimeSync {
    fn drop(&mut self) {
        let handle = {
            let mut st = self.inner.st.lock().unwrap();
            st.stop = true;
            self.inner.cv.notify_all();
            st.thread.take()
        };
        if let Some(h) = handle {
            let _ = h.join();
        }
    }
}

/// Inbound on an `fss/time` port: fold peer `idx`'s window into the conjunction.
fn ts_on_window(inner: &Arc<TsInner>, idx: usize, c: &mut Carrier) {
    let mut st = inner.st.lock().unwrap();
    // Walk the carrier's own meta for the window fields (the sender attaches it);
    // the (id,qname) cache makes the repeat resolves O(1).  No hard-coded offset,
    // no id gate — a foreign/meta-less window simply fails to resolve and is dropped.
    let (uo, lo, upo) = match (
        unsafe { st.field_cache.resolve(c, "time::Unit") },
        unsafe { st.field_cache.resolve(c, "time::Lower") },
        unsafe { st.field_cache.resolve(c, "time::Upper") },
    ) {
        (Some(u), Some(l), Some(up)) => (u.0 as usize, l.0 as usize, up.0 as usize),
        _ => return,
    };
    let data = unsafe { c.data() };
    let (unit, lower, upper) =
        match (read_u32_le(data, uo), read_u64_le(data, lo), read_u64_le(data, upo)) {
            (Some(u), Some(l), Some(up)) => (u as i32, l, up),
            _ => return,
        };

    let open = upper == TIME_OPEN_UPPER;
    if !open {
        st.peers[idx].seen = true;
        st.peers[idx].last_from_ps = ticks_to_ps(unit, lower);
        st.peers[idx].upper_ps = ticks_to_ps(unit, upper);
    } else {
        st.peers[idx].upper_ps = u64::MAX; // detached: drops out of the min
        if inner.stop_on_detach {
            st.stop = true;
        }
    }
    recompute_granted(&mut st, inner.quantum_ps);
    inner.cv.notify_all();
}

fn recompute_granted(st: &mut TsState, quantum_ps: u64) {
    // granted_upper = min over ALL configured peers (SC "anding": every peer must
    // grant).  A peer caps the clock by:
    //   • seen + bounded upper  → its Upper (normal lockstep);
    //   • detached (Upper=MAX)   → MAX, i.e. drops out of the min;
    //   • UNSEEN (not yet joined)→ the bootstrap ceiling (one quantum of absolute
    //     time): we may take the single step needed to emit our first window, but
    //     MUST NOT advance past it until the peer joins.  This is what stops the
    //     federation from racing ahead of — lapping — a late wall-clock joiner
    //     (e.g. a cross-process initiator started after a connect/settle delay);
    //     without it, the rest of the federation runs to teardown before the
    //     joiner ever delivers a frame (observed: data arriving ~2 ms of sim-time
    //     late, FIFO never drained, device rx=0).
    if st.peers.is_empty() {
        st.granted_upper_ps = st.now_ps + quantum_ps;
        return;
    }
    let mut lo = u64::MAX;
    for p in &st.peers {
        let cap = if p.seen {
            p.upper_ps // real bounded upper, or MAX once it has detached
        } else if p.upper_ps == u64::MAX {
            u64::MAX // detached before ever being seen → drops out
        } else {
            quantum_ps // unseen & live → hold at the bootstrap ceiling
        };
        if cap < lo {
            lo = cap;
        }
    }
    st.granted_upper_ps = lo;
}

/// The clock thread: advance `now` toward granted_upper in quantum steps, sending
/// our window after each step; block at the bound; on stop send open_window +
/// report skew.
fn ts_clock_loop(inner: Arc<TsInner>) {
    let q = inner.quantum_ps;
    // Announce Elaborated UP (replaces the old ts_confirm_peers first-window retry).
    ts_send_elaborated_up(&inner);
    loop {
        let now = {
            let mut st = inner.st.lock().unwrap();
            // Wait for a grant beyond `now`.  The channels are confirmed live, so a
            // plain wait suffices: the peer's window WILL arrive and broadcast cv.
            while !st.stop && st.granted_upper_ps <= st.now_ps {
                st = inner.cv.wait(st).unwrap();
            }
            if st.stop {
                break;
            }
            let next = (st.now_ps + q).min(st.granted_upper_ps);
            st.now_ps = next;
            // skew: gap to each active peer's last-known clock
            let mut worst = st.max_skew_ps;
            for p in &st.peers {
                if p.seen && p.upper_ps != u64::MAX {
                    let gap = next.abs_diff(p.last_from_ps);
                    if gap > worst {
                        worst = gap;
                    }
                }
            }
            st.max_skew_ps = worst;
            next
        };
        // Per-quantum model work (off the state lock; may send_on).
        if let Some(cb) = inner.on_tick.lock().unwrap().as_mut() {
            cb(now);
        }
        ts_send_window_all(&inner, now, now + q);
    }
    // Stopped: release every peer with an open_window, then report.
    {
        let st = inner.st.lock().unwrap();
        let now = st.now_ps;
        drop(st);
        ts_send_open_all(&inner, now);
    }
    ts_report(&inner);
}

fn ts_pack_window(buf: &mut [u8; TW_SIZE], lower_ps: u64, upper: u64) {
    buf[TW_OFF_UNIT..TW_OFF_UNIT + 4].copy_from_slice(&TIME_UNIT_PS.to_le_bytes());
    buf[4..8].copy_from_slice(&0u32.to_le_bytes());
    buf[TW_OFF_LOWER..TW_OFF_LOWER + 8].copy_from_slice(&lower_ps.to_le_bytes());
    buf[TW_OFF_UPPER..TW_OFF_UPPER + 8].copy_from_slice(&upper.to_le_bytes());
}

fn ts_send_one(ep: *mut Endpoint, buf: &mut [u8; TW_SIZE]) -> c_int {
    // Attach the meta so the receiver can walk it (the carrier contract: meta
    // present at the artefact; a transport may strip/restore it in flight).
    let mut c = Carrier {
        id: FSS_TIME_ID,
        data_ptr: buf.as_mut_ptr() as *mut c_void,
        data_size: TW_SIZE as u64,
        meta_ptr: FSS_TIME_META.as_ptr() as *mut c_void,
        meta_size: FSS_TIME_META.len() as u64,
    };
    unsafe { send_on(ep, &mut c) }
}

/// Fix this node's n_downstream once, when wired.  The fallback resolver allocated
/// one fss/sim/<tag> sink per downstream peer (all get_endpoint calls are done by the
/// time we are told we are wired), so n_sim_down IS the number of Elaborated the run
/// gate must await.  The time peers are symmetric, so there is no head to detect and
/// no peer to classify.  Called under st lock.
fn ts_classify_locked(st: &mut TsState) {
    if st.classified { return; }
    st.n_downstream = st.n_sim_down;
    st.classified = true;
}

/// Dispatch a sim::event received on fss/sim.
fn ts_on_sim(inner: &Arc<TsInner>, code: u32) {
    match code {
        SIM_BOUND | SIM_STARTED => {
            // "you are wired" — classify, PULL config, then arm the start gate.  The
            // pull runs OFF the lock and BEFORE the clock spawns, so the value is
            // applied pre-run (the in-band replacement for the old config push).
            {
                let mut st = inner.st.lock().unwrap();
                ts_classify_locked(&mut st);
                st.wired = true;
            }
            ts_pull_config(inner);
            ts_try_start(inner);
        }
        SIM_ELABORATED => {
            // a downstream peer is running
            {
                let mut st = inner.st.lock().unwrap();
                st.downstream_ready += 1;
            }
            ts_try_start(inner);
        }
        SIM_STOPPED => {
            let mut st = inner.st.lock().unwrap();
            st.stop = true;
            inner.cv.notify_all();
        }
        _ => {}
    }
}

/// PULL this node's config at wired time.  The orchestrator installed our config
/// port's `send` at bind (its single responder is the fan-in sink); we send a
/// `config::param_request{Get}` and the round-trip reply lands in-place (synchronous
/// for inproc; the transport blocks for shm/zenoh).  We then feed the reply to the
/// port's OWN `receive`, which applies it (the model's `config_get_u64` → store) —
/// exactly the path the old push used, just inverted to pull.  One-shot; no-op if the
/// model has no config port or nothing bound its send (a node nobody configures runs
/// on model defaults).  Mirrors the SC artefact's `pull_config`.
fn ts_pull_config(inner: &Arc<TsInner>) {
    let cfg = {
        let mut st = inner.st.lock().unwrap();
        if st.config_pulled {
            return;
        }
        st.config_pulled = true;
        st.cfg_ep.0
    };
    if cfg.is_null() {
        return;
    }
    // SAFETY: `cfg` is a stable Endpoint ptr from the Node (outlives the run); the
    // transport installed send/send_ctx at bind.  `buf`/`c` are valid for the
    // duration of the synchronous round-trip.
    let ep = unsafe { &*cfg };
    let send = match ep.send {
        Some(s) => s,
        None => return, // nobody configures us → keep model defaults
    };
    // RETRY across an async transport's route setup: the in-place round-trip is
    // immediate for inproc/shm (succeeds on the first pass, no sleep), but a freshly
    // bound zenoh querier may fire before its peer queryable is discovered.  Bounded;
    // if the responder is genuinely unreachable we fall back to model defaults.
    for _ in 0..300 {
        let mut buf = [0u8; 60]; // OpCode u32@0 = Get(0); ParamValue@16; Status u32@56
        let mut c = Carrier {
            id: FSS_CONFIG_ID,
            data_ptr: buf.as_mut_ptr() as *mut c_void,
            data_size: buf.len() as u64,
            meta_ptr: FSS_CONFIG_META.as_ptr() as *mut c_void,
            meta_size: FSS_CONFIG_META.len() as u64,
        };
        if unsafe { send(ep.send_ctx, &mut c) } == FSS_SEND_OK {
            // A valid reply has a JSON object in config::ParamValue (walk the meta).
            if let Some((off, _)) = unsafe { carrier_field(&c, "config::ParamValue") } {
                if buf.get(off as usize).copied() == Some(b'{') {
                    if let Some(recv) = ep.receive {
                        unsafe { recv(ep.self_, &mut c); } // applies via the model's store closure
                    }
                    return;
                }
            }
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
}

/// Try-start gate: spawn the clock if wired AND all downstream peers ready.
fn ts_try_start(inner: &Arc<TsInner>) {
    let mut st = inner.st.lock().unwrap();
    if st.thread.is_some() || st.stop {
        return;
    }
    if st.wired && st.downstream_ready >= st.n_downstream {
        let ci = inner.clone();
        st.thread = Some(std::thread::spawn(move || ts_clock_loop(ci)));
    }
}

/// Announce Elaborated UP the chain on our dedicated fss/sim port, whose send the
/// orchestrator bound to our upstream (a peer's fss/sim/<tag> sink, or the
/// orchestrator if we are the source).  Retry until FSS_SEND_OK (readiness
/// handshake — NOT a data window): the upstream won't send us a window until we do.
fn ts_send_elaborated_up(inner: &Arc<TsInner>) {
    let ep: *mut Endpoint = { inner.st.lock().unwrap().sim_ep.0 };
    if ep.is_null() { return; }
    // Build sim::event carrier {event_code = SIM_ELABORATED (1) LE @0}.
    let mut buf = [0u8; 4];
    buf[0..4].copy_from_slice(&SIM_ELABORATED.to_le_bytes());
    let mut c = Carrier {
        id: FSS_SIM_ID,
        data_ptr: buf.as_mut_ptr() as *mut c_void,
        data_size: 4,
        meta_ptr: FSS_SIM_META.as_ptr() as *mut c_void,
        meta_size: FSS_SIM_META.len() as u64,
    };
    // Retry until delivered — the upstream won't send us a window until we do.
    loop {
        if unsafe { send_on(ep, &mut c) } == FSS_SEND_OK { break; }
        if inner.st.lock().unwrap().stop { return; }
        std::thread::sleep(std::time::Duration::from_millis(2));
    }
}

fn ts_send_window_all(inner: &Arc<TsInner>, lower_ps: u64, upper_ps: u64) {
    // Collect the peer ptrs under the lock, send outside it.
    let ptrs: Vec<*mut Endpoint> = {
        let st = inner.st.lock().unwrap();
        st.peers.iter().map(|p| p.ep.0).collect()
    };
    let mut buf = [0u8; TW_SIZE];
    ts_pack_window(&mut buf, lower_ps, upper_ps);
    for p in ptrs {
        ts_send_one(p, &mut buf);
    }
}

fn ts_send_open_all(inner: &Arc<TsInner>, now_ps: u64) {
    let ptrs: Vec<*mut Endpoint> = {
        let st = inner.st.lock().unwrap();
        st.peers.iter().map(|p| p.ep.0).collect()
    };
    let mut buf = [0u8; TW_SIZE];
    ts_pack_window(&mut buf, now_ps, TIME_OPEN_UPPER);
    for p in ptrs {
        ts_send_one(p, &mut buf);
    }
}

fn ts_report(inner: &Arc<TsInner>) {
    let st = inner.st.lock().unwrap();
    let any = st.peers.iter().any(|p| p.seen);
    if !any {
        return;
    }
    let gap_ns = st.max_skew_ps as f64 / 1000.0;
    // Diagnostic, NOT a verdict: the bounded multi-quantum reading is real processing-
    // latency skew (a pacing wait lands a few quanta past the peer's last checkpoint),
    // not a lockstep violation, so there is no fixed bound to PASS/FAIL against.  An
    // unbounded (run-growing) gap would be the signal something broke.  See
    // fss-skew-bounded-transient.
    println!(
        "FSS TIMESYNC {}: max_clock_gap={} ns",
        inner.name, gap_ns
    );
    use std::io::Write;
    let _ = std::io::stdout().flush();
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicU32;

    /// The mirrored meta consts MUST be the exact canonical CBOR the compiler
    /// hashed to produce the carrier ids — so a peer that walks them sees the same
    /// layout the id promises.  FNV-1a-64 over the const == the id proves it
    /// byte-for-byte (the bit-identical guarantee, without linking the C core).
    #[test]
    fn core_meta_consts_match_ids() {
        fn fnv1a64(b: &[u8]) -> u64 {
            let mut h: u64 = 0xcbf2_9ce4_8422_2325;
            for &x in b {
                h ^= x as u64;
                h = h.wrapping_mul(0x0000_0100_0000_01b3);
            }
            h
        }
        assert_eq!(fnv1a64(FSS_SIM_META), FSS_SIM_ID, "FSS_SIM_META drifted from sim.h");
        assert_eq!(fnv1a64(FSS_TIME_META), FSS_TIME_ID, "FSS_TIME_META drifted from time_sync.h");
        assert_eq!(fnv1a64(FSS_CONFIG_META), FSS_CONFIG_ID, "FSS_CONFIG_META drifted from config.h");
    }

    /// The resolver walks the real generated metas and returns the published
    /// offsets — pinning both the reader and the const layout (sim::EventCode@0/4,
    /// time::{Unit@0/4, Lower@8/8, Upper@16/8}); a sub-meta (core::Extensions) is
    /// not a data field.
    #[test]
    fn resolver_walks_core_metas() {
        assert_eq!(meta_find(FSS_SIM_META, "sim::EventCode"), Some((0, 4)));
        assert_eq!(meta_find(FSS_SIM_META, "sim::Status"), Some((4, 4)));
        assert_eq!(meta_find(FSS_TIME_META, "time::Unit"), Some((0, 4)));
        assert_eq!(meta_find(FSS_TIME_META, "time::Lower"), Some((8, 8)));
        assert_eq!(meta_find(FSS_TIME_META, "time::Upper"), Some((16, 8)));
        assert_eq!(meta_find(FSS_TIME_META, "core::Extensions"), None); // sub-meta
        assert_eq!(meta_find(FSS_SIM_META, "no::Such"), None);
    }

    /// The inline flag distinguishes a value carried in the data region from one
    /// carried by pointer (layout "fss:local-ptr[-nul]").  A plain scalar is inline;
    /// config::ParamName (an fss:local-ptr-nul char*) is not.  This is what lets a
    /// consumer read a foreign carrier's bus::Data correctly (inline reg bytes vs a
    /// live-TLM payload pointer).
    #[test]
    fn field_layout_inline_flag() {
        assert_eq!(meta_find_full(FSS_TIME_META, "time::Lower"), Some((8, 8, true)));
        // config::ParamName is a by-reference char* (fss:local-ptr-nul).
        let (_, _, inline) = meta_find_full(FSS_CONFIG_META, "config::ParamName").unwrap();
        assert!(!inline, "ParamName is carried by pointer, not inline");
        // config::ParamValue is an inline blob.
        let (_, _, inline_pv) = meta_find_full(FSS_CONFIG_META, "config::ParamValue").unwrap();
        assert!(inline_pv, "ParamValue is carried inline");
    }

    // The ABI layout must match the C headers exactly (64-bit).
    #[test]
    fn abi_layout() {
        assert_eq!(std::mem::size_of::<Carrier>(), 40);
        assert_eq!(std::mem::size_of::<Endpoint>(), 32);
        // Option<extern "C" fn> must be pointer-sized (null-niche = None).
        assert_eq!(std::mem::size_of::<Option<CarrierFn>>(), std::mem::size_of::<*const c_void>());
    }

    // Validate the endpoint trampoline dispatches to the Rust handler and that
    // the handler mutates the carrier's data region in place — the mechanics an
    // artefact relies on. (Interop with a real transport is proven by the
    // examples; this only exercises the binding's own glue.)
    #[test]
    fn endpoint_receive_dispatches_and_mutates() {
        let mut node = Node::new();
        node.add_endpoint("in", |c: &mut Carrier| {
            let d = unsafe { c.data_mut() };
            for b in d.iter_mut() {
                *b = b.wrapping_mul(2);
            }
            FSS_SEND_OK
        });

        let ep = node.endpoint_ptr("in");
        assert!(!ep.is_null());
        assert!(node.endpoint_ptr("missing").is_null());

        let mut buf: [u8; 4] = [1, 2, 3, 4];
        let mut c = Carrier {
            id: 0,
            data_ptr: buf.as_mut_ptr() as *mut c_void,
            data_size: 4,
            meta_ptr: std::ptr::null_mut(),
            meta_size: 0,
        };

        // Invoke exactly as a transport would: ep.receive(ep.self, &c).
        unsafe {
            let e = &*ep;
            let recv = e.receive.expect("receive installed");
            assert_eq!(recv(e.self_, &mut c), FSS_SEND_OK);
        }
        assert_eq!(buf, [2, 4, 6, 8]);
    }

    // A recording sink: counts carriers and remembers data[0].  Used to stand in
    // for the downstream peer a transport would bind to an outbound endpoint.
    static SINK_COUNT: AtomicU32 = AtomicU32::new(0);
    static SINK_LAST_B0: AtomicU32 = AtomicU32::new(0);
    unsafe extern "C" fn recording_sink(_self: *mut c_void, c: *mut Carrier) -> c_int {
        let d = (*c).data();
        if !d.is_empty() {
            SINK_LAST_B0.store(d[0] as u32, std::sync::atomic::Ordering::SeqCst);
        }
        SINK_COUNT.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        FSS_SEND_OK
    }

    // Exercise the outbound + start-hook glue an initiator relies on: an outbound
    // endpoint, a start hook that sends N carriers on it, and the fss/sim port
    // that fires the hook on Started.  (Real transport interop is in examples;
    // this binds the outbound's send to a local sink, as a transport would.)
    #[test]
    fn outbound_start_hook_emits_on_started() {
        let mut node = Node::new();
        node.add_outbound("socket");
        let socket = node.endpoint_ptr("socket");
        assert!(!socket.is_null());

        // The start hook captures the outbound endpoint pointer and emits 16
        // carriers whose data[0] cycles 0..15 — the initiator's "send N on start".
        node.on_start(move || {
            for i in 0u8..16 {
                let mut buf = [0u8; 4];
                buf[0] = i;
                let mut c = Carrier {
                    id: 0,
                    data_ptr: buf.as_mut_ptr() as *mut c_void,
                    data_size: 4,
                    meta_ptr: std::ptr::null_mut(),
                    meta_size: 0,
                };
                unsafe {
                    send_on(socket, &mut c);
                }
            }
        });

        // Before bind: send is unbound, so the hook's sends are no-ops.
        SINK_COUNT.store(0, std::sync::atomic::Ordering::SeqCst);

        // The transport installs send/send_ctx at bind — simulate it.
        unsafe {
            (*socket).send = Some(recording_sink);
            (*socket).send_ctx = std::ptr::null_mut();
        }

        // Fire Started on fss/sim exactly as the host would.
        let sim = node.endpoint_ptr("fss/sim");
        assert!(!sim.is_null());
        let mut code = SIM_STARTED.to_le_bytes();
        let mut ev = Carrier {
            id: FSS_SIM_ID,
            data_ptr: code.as_mut_ptr() as *mut c_void,
            data_size: 4,
            meta_ptr: FSS_SIM_META.as_ptr() as *mut c_void,
            meta_size: FSS_SIM_META.len() as u64,
        };
        unsafe {
            let e = &*sim;
            let recv = e.receive.expect("fss/sim receive installed");
            assert_eq!(recv(e.self_, &mut ev), FSS_SEND_OK);
        }

        assert_eq!(SINK_COUNT.load(std::sync::atomic::Ordering::SeqCst), 16);
        assert_eq!(SINK_LAST_B0.load(std::sync::atomic::Ordering::SeqCst), 15);

        // A non-Started code must NOT fire the hook.
        SINK_COUNT.store(0, std::sync::atomic::Ordering::SeqCst);
        let mut other = 0u32.to_le_bytes(); // Bound
        let mut ev2 = Carrier {
            id: FSS_SIM_ID,
            data_ptr: other.as_mut_ptr() as *mut c_void,
            data_size: 4,
            meta_ptr: FSS_SIM_META.as_ptr() as *mut c_void,
            meta_size: FSS_SIM_META.len() as u64,
        };
        unsafe {
            let e = &*sim;
            (e.receive.unwrap())(e.self_, &mut ev2);
        }
        assert_eq!(SINK_COUNT.load(std::sync::atomic::Ordering::SeqCst), 0);
    }
}
