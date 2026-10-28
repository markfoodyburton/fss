# This file is part of fss
# Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
# Author: Mark Burton
#
# SPDX-License-Identifier: BSD-3-Clause

"""fss — Python language binding for the Federated Simulation Standard.

The carrier-agnostic glue that lets Python speak the reference C ABI
(``abi/include/fss/*.h``) via ``ctypes``. A Python interpreter is not itself a
``cdylib``, so it is not directly ``dlopen``-able; via this binding Python
participates as either:

* a **host/driver**: construct artefact nodes directly via the artefact ABI
  (``fss_create`` / ``fss_get_endpoint`` / ``fss_destroy`` — the placement role,
  see :class:`Artefact`), load a transport plugin (.so/.dylib), and use its
  vtable (``channel`` / ``attach`` / ``send_to``) to wire and drive carriers; or
* a **receiver process**: register a Python ``receive`` callback as an endpoint
  sink (e.g. the SHM transport's ``fss_shm_set_sink``) and consume carriers
  cross-process.

(A Python *model* can also be a fully ``dlopen``'d artefact — see
``languages/python-loader``, a C cdylib that embeds CPython and bridges a ``.py``
model's endpoints, so a transport's ``open()`` instantiates it like any other.)

It is a language binding, not a carrier binding: it knows nothing about any
particular carrier. Carrier-specific parsing lives in the model/example.
"""

import ctypes as C

# Transport status codes (abi/include/fss/transport_abi.h).
FSS_SEND_OK = 0
FSS_SEND_UNBOUND = 1
FSS_SEND_NO_RECEIVER = 2

# Transport plugin negotiation (abi/include/fss/transport_abi.h).  The vtable is
# fixed; a plugin must declare the channel ABI before Python reads it.
FSS_TRANSPORT_ABI_VERSION_MAJOR = 1
FSS_TRANSPORT_ABI_VERSION_MINOR = 0
FSS_TRANSPORT_CAP_CHANNEL = 1
FSS_TRANSPORT_CAP_ATTACH = 2
FSS_TRANSPORT_CAP_SEND_TO = 4
FSS_TRANSPORT_CAP_CHANNEL_ABI = (FSS_TRANSPORT_CAP_CHANNEL |
                                 FSS_TRANSPORT_CAP_ATTACH |
                                 FSS_TRANSPORT_CAP_SEND_TO)

# The role an endpoint takes when it attaches to a channel (fss_role, D2).
FSS_ROLE_INITIATOR = 0   # sends into the channel (installs local->send)
FSS_ROLE_TARGET = 1      # receives from the channel (installed as its sink)


class Carrier(C.Structure):
    """Mirror of fss_carrier_t (strict 40-byte layout)."""
    _fields_ = [
        ("id", C.c_uint64),
        ("data_ptr", C.c_void_p),
        ("data_size", C.c_uint64),
        ("meta_ptr", C.c_void_p),
        ("meta_size", C.c_uint64),
    ]


# Delivery callback — both inbound (receive) and outbound (send): fss_carrier_fn.
CARRIER_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(Carrier))


class Endpoint(C.Structure):
    """Mirror of fss_endpoint_t (32-byte layout)."""
    _fields_ = [
        ("self", C.c_void_p),
        ("receive", CARRIER_FN),
        ("send", CARRIER_FN),
        ("send_ctx", C.c_void_p),
    ]


class _Channel(C.Structure):
    """Opaque mirror of fss_channel_t — a transport-private channel handle."""
    pass


class _Transport(C.Structure):
    """Mirror of fss_transport struct (self + the channel/attach/send_to vtable)."""
    pass


# channel(self, name, round_trip, owner, carrier_hints, n) -> fss_channel_t*
_CHANNEL_FN = C.CFUNCTYPE(C.POINTER(_Channel), C.c_void_p, C.c_char_p,
                          C.c_int, C.c_int, C.POINTER(C.c_uint64), C.c_size_t)
# attach(self, channel, local, role) -> int
_ATTACH_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(_Channel),
                         C.POINTER(Endpoint), C.c_int)
# send_to(self, channel, carrier) -> int
_SEND_TO_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(_Channel), C.POINTER(Carrier))

_Transport._fields_ = [
    ("self", C.c_void_p),
    ("channel", _CHANNEL_FN),
    ("attach", _ATTACH_FN),
    ("send_to", _SEND_TO_FN),
]


def fss_send(ep, carrier):
    """Deliver on an attached endpoint: ep.send(ep.send_ctx, &carrier).

    `ep` is an Endpoint whose send/send_ctx an INITIATOR attach installed.
    Returns the peer receive's status, or FSS_SEND_UNBOUND if send is unset.
    """
    if not ep.send:
        return FSS_SEND_UNBOUND
    return ep.send(ep.send_ctx, C.byref(carrier))


class Artefact:
    """A dlopen'd FSS artefact, constructed via the artefact ABI.

    Node construction is the *placement* role (fss_create / fss_get_endpoint /
    fss_destroy — abi/include/fss/artefact_abi.h), no longer a transport verb.
    A Python host constructs a node here, resolves its endpoints, then wires them
    with a :class:`Transport`.
    """

    def __init__(self, module_path, name=""):
        self._lib = C.CDLL(module_path)
        self._lib.fss_create.restype = C.c_void_p
        self._lib.fss_create.argtypes = [C.c_void_p, C.c_char_p]
        self._lib.fss_get_endpoint.restype = C.POINTER(Endpoint)
        self._lib.fss_get_endpoint.argtypes = [C.c_void_p, C.c_char_p]
        self._lib.fss_destroy.restype = None
        self._lib.fss_destroy.argtypes = [C.c_void_p]
        self._node = self._lib.fss_create(None, name.encode())
        if not self._node:
            raise RuntimeError("fss_create returned NULL")

    def endpoint(self, name):
        """Resolve a named endpoint to a POINTER(Endpoint) (or a null pointer)."""
        return self._lib.fss_get_endpoint(self._node, name.encode())

    def destroy(self):
        if self._node:
            self._lib.fss_destroy(self._node)
            self._node = None


class Transport:
    """A loaded transport plugin: dlopen + the factory + the vtable.

    Mirrors how every FSS host uses a transport — resolve the well-known factory
    symbols, create an instance, then call only the vtable (channel/attach/send_to).
    """

    def __init__(self, plugin_path, config=None):
        self._lib = C.CDLL(plugin_path)
        try:
            version = self._lib.fss_transport_abi_version
            capabilities = self._lib.fss_transport_capabilities
        except AttributeError as exc:
            raise RuntimeError("transport lacks required ABI query symbols") from exc
        version.restype = C.c_uint32
        version.argtypes = []
        capabilities.restype = C.c_uint64
        capabilities.argtypes = []
        got_version = version()
        got_capabilities = capabilities()
        got_major = got_version >> 16
        got_minor = got_version & 0xffff
        if (got_major != FSS_TRANSPORT_ABI_VERSION_MAJOR or
                got_minor < FSS_TRANSPORT_ABI_VERSION_MINOR or
                (got_capabilities & FSS_TRANSPORT_CAP_CHANNEL_ABI) !=
                FSS_TRANSPORT_CAP_CHANNEL_ABI):
            raise RuntimeError("transport has incompatible ABI or lacks channel capabilities")
        self._lib.fss_transport_create.restype = C.POINTER(_Transport)
        self._lib.fss_transport_create.argtypes = [C.c_char_p]
        self._lib.fss_transport_destroy.restype = None
        self._lib.fss_transport_destroy.argtypes = [C.POINTER(_Transport)]
        cfg = config.encode() if isinstance(config, str) else config
        self._tp = self._lib.fss_transport_create(cfg)
        if not self._tp:
            raise RuntimeError("fss_transport_create returned NULL")

    @property
    def _t(self):
        return self._tp.contents

    def channel(self, name, round_trip=1, owner=1, hints=None):
        """Create or find channel `name`. Returns a channel handle (may be null
        for owner=0 before the owner is up — retry). `hints` is an optional list
        of indicative carrier ids."""
        n = len(hints) if hints else 0
        arr = (C.c_uint64 * n)(*hints) if hints else None
        return self._t.channel(self._t.self, name.encode(), int(round_trip),
                               int(owner), arr, n)

    def attach(self, channel, endpoint, role):
        """Join `endpoint` (an Endpoint, or a POINTER(Endpoint) from
        Artefact.endpoint) to `channel` in `role`. An INITIATOR attach installs
        the endpoint's send; a TARGET attach installs it as the channel's sink."""
        ep = endpoint if isinstance(endpoint, C._Pointer) else C.byref(endpoint)
        return self._t.attach(self._t.self, channel, ep, int(role))

    def send_to(self, channel, carrier):
        """One-shot deliver `carrier` to `channel` (no standing attach)."""
        return self._t.send_to(self._t.self, channel, C.byref(carrier))

    def set_sink(self, endpoint):
        """Register a receive sink (transports that expose fss_shm_set_sink).

        `endpoint` is an Endpoint whose `receive` is a Python callback made with
        make_receiver(); keep it alive for the transport's lifetime.
        """
        fn = self._lib.fss_shm_set_sink
        fn.restype = None
        fn.argtypes = [C.POINTER(_Transport), C.POINTER(Endpoint)]
        fn(self._tp, C.byref(endpoint))

    def destroy(self):
        if self._tp:
            self._lib.fss_transport_destroy(self._tp)
            self._tp = None


# ===========================================================================
# Carrier-meta resolver — the binding's "get me the xyz atom" primitive.
#
# Per docs/spec/05-abi-conventions.md ("Consuming a carrier's fields"): a receiver
# resolves a field's (byte_offset, byte_size) by qname from the carrier's OWN CBOR
# meta.  The meta MUST be present by the time the carrier reaches the artefact (a
# transport may strip it on the wire, but restores it before delivery); the carrier
# id is never a gate.  This is a pure-Python reader for the closed CBOR profile
# tools/fss_compile.py emits — a definite-length array of definite-length maps with
# tstr keys — mirroring fss/c/meta.c's fss_meta_find and the Rust binding's
# meta_find.  It stays carrier-AGNOSTIC (a generic "get me atom X"); bus::/can::
# knowledge lives in the model/example that calls it.
# ===========================================================================


def _cbor_head(b, i):
    """Read one CBOR head at offset i: return (major, arg, next_i) — definite
    forms only.  On a malformed/out-of-bounds read raises IndexError."""
    ib = b[i]
    i += 1
    major = (ib >> 5) & 0x07
    ai = ib & 0x1F
    if ai < 24:
        return major, ai, i
    if ai == 24:
        return major, b[i], i + 1
    if ai == 25:
        return major, (b[i] << 8) | b[i + 1], i + 2
    if ai == 26:
        v = 0
        for k in range(4):
            v = (v << 8) | b[i + k]
        return major, v, i + 4
    if ai == 27:
        v = 0
        for k in range(8):
            v = (v << 8) | b[i + k]
        return major, v, i + 8
    raise IndexError("indefinite/reserved CBOR not in the profile")


def _cbor_skip(b, i):
    """Skip one complete data item; return the next offset."""
    major, arg, i = _cbor_head(b, i)
    if major in (0, 1, 7):
        return i                     # uint / negint / simple
    if major in (2, 3):
        return i + arg               # bstr / tstr payload
    if major == 4:
        for _ in range(arg):
            i = _cbor_skip(b, i)
        return i
    if major == 5:
        for _ in range(arg):
            i = _cbor_skip(b, i)
            i = _cbor_skip(b, i)
        return i
    raise IndexError("bad CBOR major")


def meta_find(meta, qname):
    """Resolve `qname` in a CBOR `meta` (bytes/memoryview), returning
    (byte_offset, byte_size, is_inline) or None if absent / a sub-meta (no data
    bytes) / malformed.  `is_inline` is True when the field carries its value
    DIRECTLY in the data region (a plain scalar or inline bstr — no "layout"
    attribute), False when carried BY REFERENCE (layout "fss:local-ptr" — the live
    TLM Data slot holds the payload's address).  Mirrors fss_meta_find + the C
    binding's data_is_inline (empty layout = inline)."""
    b = bytes(meta) if not isinstance(meta, (bytes, bytearray)) else meta
    qn = qname.encode()
    try:
        major, n_entries, i = _cbor_head(b, 0)
        if major != 4:
            return None                          # top level must be an array
        for _ in range(n_entries):
            em, n_pairs, i = _cbor_head(b, i)
            if em != 5:
                return None                      # each entry is a map
            found = False
            off = size = None
            layout_len = 0                       # >0 → carried by reference
            for _ in range(n_pairs):
                km, klen, i = _cbor_head(b, i)
                if km != 3:
                    return None                  # key is a tstr
                key = b[i:i + klen]
                i += klen
                if key == b"qname":
                    vm, vlen, i = _cbor_head(b, i)
                    if vm != 3:
                        return None
                    found = b[i:i + vlen] == qn
                    i += vlen
                elif key == b"byte_offset":
                    vm, off, i = _cbor_head(b, i)
                    if vm != 0:
                        return None
                elif key == b"byte_size":
                    vm, size, i = _cbor_head(b, i)
                    if vm != 0:
                        return None
                elif key == b"layout":
                    vm, layout_len, i = _cbor_head(b, i)
                    if vm != 3:
                        return None
                    i += layout_len              # skip the tstr payload
                else:
                    i = _cbor_skip(b, i)         # wire / version / encoding / attrs
            if found:
                if off is None or size is None:
                    return None                  # a sub-meta entry — not a data field
                return (off, size, layout_len == 0)
    except (IndexError, KeyError):
        return None
    return None


# Keep Python receive trampolines alive (ctypes callbacks must outlive the call).
_receivers = []


def make_receiver(py_receive):
    """Build an inbound Endpoint whose `receive` runs `py_receive(carrier)`.

    `py_receive` takes a Carrier and returns an int status. Used as a sink for
    cross-process transports where Python is the receiving peer.
    """
    def _trampoline(_self, cptr):
        if not cptr:
            return FSS_SEND_NO_RECEIVER
        return py_receive(cptr.contents)

    cb = CARRIER_FN(_trampoline)
    _receivers.append(cb)  # prevent GC
    ep = Endpoint()
    ep.self = None
    ep.receive = cb
    ep.send = C.cast(None, CARRIER_FN)
    ep.send_ctx = None
    return ep
