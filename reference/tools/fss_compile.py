#!/usr/bin/env python3
# This file is part of fss
# Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
# Author: Mark Burton
#
# SPDX-License-Identifier: BSD-3-Clause

"""
fss_compile.py — compile an FSS carrier shape to its canonical live meta and
compute the carrier id.

Reads a carrier-shape .jsonnet file (e.g. known_carriers/tlm/transaction.jsonnet),
    resolves it via the `jsonnet` tool, builds the wire-format meta as a CBOR
    array per docs/spec/01-carrier.md, excludes nested sub-meta content from
    the base identity,
and computes the FNV-1a-64 hash to produce the carrier id.

Usage:
    fss_compile.py <shape.jsonnet>
"""

import json
import os
import re
import subprocess
import sys

# ---------------------------------------------------------------------------
# FNV-1a-64
# ---------------------------------------------------------------------------
FNV1A_64_OFFSET = 0xcbf29ce484222325
FNV1A_64_PRIME  = 0x100000001b3
_FNV_MASK       = (1 << 64) - 1

def fnv1a_64(data: bytes) -> int:
    h = FNV1A_64_OFFSET
    for b in data:
        h ^= b
        h  = (h * FNV1A_64_PRIME) & _FNV_MASK
    return h

# ---------------------------------------------------------------------------
# CDDL flat-layout profile (docs/spec/02-encodings.md)
#
# An atom's `wire` field is a CDDL fragment from the closed FSS profile.
# In the flat data region it maps to a fixed byte width:
#   uint .size N / int .size N  -> N bytes, little-endian
#   uint / int                  -> width-agnostic role type; byte_size REQUIRED
#                                  in the composition (the carrier fixes the
#                                  exact width — e.g. an address is 8 bytes in
#                                  TLM, 4 in a CAN frame).  Same role/qname,
#                                  different layout: shared across protocols.
#   float32 / float64           -> 4 / 8 bytes, IEEE-754 little-endian
#   bool                        -> 1 byte
#   bstr / tstr                 -> variable; byte_size REQUIRED in the entry
#
# Enumerated meaning / encoding (deferred encoding):
#   An atom MAY declare `default_encoding: { Meaning: wire_value, ... }` — the
#   shared *meaning vocabulary* plus a canonical wire encoding.  A composition
#   entry MAY override with `encoding: { ... }` to match a foreign struct's
#   exact byte values (e.g. CAN's rtr: Write=0/Read=1) while keeping the shared
#   meaning names.  The EFFECTIVE encoding is recorded in the meta, so the
#   carrier is self-describing and a bridge can translate between two carriers
#   that share an atom's meanings but differ in wire encoding.
# CDDL describes the data model only; byte offset, width-from-.size, and
# endianness are the FSS profile's contribution.
#
# Three flat-layout encodings are FSS constructs CDDL cannot express; they are
# carried in the entry's `layout` field rather than `wire`:
#   sub-meta                    -> no data bytes (a nested meta; its nested
#                                  content is excluded from the base id)
#   fss:local-ptr / -nul        -> 8 bytes (sizeof void* on 64-bit)
# ---------------------------------------------------------------------------
_WIRE_SIZED_INT = re.compile(r'^(?:u?int)\s+\.size\s+(\d+)$')

def wire_byte_size(wire: str):
    """Byte width a CDDL-profile `wire` fragment occupies in the flat data
    region, or None for variable-width types (bstr/tstr) that need an explicit
    byte_size.  Raises ValueError for fragments outside the profile."""
    w = wire.strip()
    m = _WIRE_SIZED_INT.match(w)
    if m:
        return int(m.group(1))
    if w == 'float32':
        return 4
    if w == 'float64':
        return 8
    if w == 'bool':
        return 1
    if w in ('bstr', 'tstr', 'uint', 'int'):
        return None   # width comes from the composition's byte_size
    raise ValueError(w)

# ---------------------------------------------------------------------------
# Canonical CBOR encoder (RFC 8949 §4.2)
#
# Canonical rules:
#   - Definite-length arrays and maps
#   - Smallest integer encoding (no padding)
#   - Map keys sorted by bytewise comparison of their CBOR-encoded form
#     (length-first: a 1-byte key encoding sorts before a 2-byte key encoding;
#      ties broken bytewise left-to-right — RFC 8949 §4.2.1 core determinism)
#   - Text strings as UTF-8
# ---------------------------------------------------------------------------

def _cbor_head(major: int, value: int) -> bytes:
    """Encode a CBOR initial byte + argument using the smallest possible form."""
    major_bits = (major & 0x07) << 5
    if value <= 23:
        return bytes([major_bits | value])
    elif value <= 0xFF:
        return bytes([major_bits | 24, value])
    elif value <= 0xFFFF:
        return bytes([major_bits | 25, (value >> 8) & 0xFF, value & 0xFF])
    elif value <= 0xFFFFFFFF:
        return bytes([major_bits | 26,
                      (value >> 24) & 0xFF, (value >> 16) & 0xFF,
                      (value >>  8) & 0xFF,  value        & 0xFF])
    else:
        return bytes([major_bits | 27,
                      (value >> 56) & 0xFF, (value >> 48) & 0xFF,
                      (value >> 40) & 0xFF, (value >> 32) & 0xFF,
                      (value >> 24) & 0xFF, (value >> 16) & 0xFF,
                      (value >>  8) & 0xFF,  value        & 0xFF])

def cbor_encode(value) -> bytes:
    """Recursively encode a Python value to canonical CBOR bytes."""
    if isinstance(value, bool):
        # bool must be checked before int (bool is subclass of int)
        return bytes([0xf5 if value else 0xf4])
    elif value is None:
        return bytes([0xf6])
    elif isinstance(value, int):
        if value >= 0:
            return _cbor_head(0, value)          # major type 0: unsigned int
        else:
            return _cbor_head(1, (-1 - value))   # major type 1: negative int
    elif isinstance(value, str):
        encoded = value.encode('utf-8')
        return _cbor_head(3, len(encoded)) + encoded   # major type 3: text string
    elif isinstance(value, bytes):
        return _cbor_head(2, len(value)) + value       # major type 2: byte string
    elif isinstance(value, list):
        head = _cbor_head(4, len(value))               # major type 4: array
        return head + b''.join(cbor_encode(item) for item in value)
    elif isinstance(value, dict):
        # major type 5: map
        # Keys must be encoded first so we can sort by their CBOR encoding
        pairs = []
        for k, v in value.items():
            k_enc = cbor_encode(k)
            v_enc = cbor_encode(v)
            pairs.append((k_enc, v_enc))
        # RFC 8949 §4.2.1 canonical: sort by encoded key bytes
        # shorter encoding sorts first; equal length sorts bytewise
        pairs.sort(key=lambda p: p[0])
        head = _cbor_head(5, len(pairs))
        return head + b''.join(k_enc + v_enc for k_enc, v_enc in pairs)
    else:
        raise TypeError(f"cbor_encode: unsupported type {type(value).__name__!r}: {value!r}")

# ---------------------------------------------------------------------------
# Resolve Jsonnet
# ---------------------------------------------------------------------------

def resolve_jsonnet(path: str) -> dict:
    """Run `jsonnet <path>` and return the parsed JSON object."""
    try:
        result = subprocess.run(
            ['jsonnet', path],
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        sys.stderr.write(
            "fss_compile.py: needs `jsonnet` on PATH (try `brew install jsonnet`)\n"
        )
        sys.exit(2)

    if result.returncode != 0:
        sys.stderr.write(f"fss_compile.py: jsonnet failed:\n{result.stderr}\n")
        sys.exit(2)

    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as e:
        sys.stderr.write(f"fss_compile.py: failed to parse jsonnet output: {e}\n")
        sys.exit(2)

# ---------------------------------------------------------------------------
# Build live meta entries
# ---------------------------------------------------------------------------

def build_meta_entry(entry: dict, source_path: str) -> dict:
    """
    Given a carrier-shape atom entry (from the resolved Jsonnet), build the
    live-meta dict.

    A normal entry describes one field in the carrier's single data region:
      qname / version / wire (CDDL type) / byte_offset / byte_size / [layout] / [attrs]

    A sub-meta entry (layout 'sub-meta', e.g. core::Extensions) is a
    meta-structural slot, NOT a data field: it carries a nested meta (the
    extension chain), not data-region bytes, and so has no wire / byte_offset
    / byte_size.  Its nested entries (if any) point into the SAME data region.
    The slot itself defines the base identity, but its nested content does not
    (see build_cbor_meta_for_id).
    """
    atom    = entry['atom']
    qname   = atom['qname']
    version = atom['version']

    # A composition entry MAY set `layout`; otherwise the atom's own layout
    # applies (e.g. core::Extensions is intrinsically 'sub-meta').
    layout = entry.get('layout', atom.get('layout'))

    # Sub-meta: meta-structural slot, no data-region bytes.
    if layout == 'sub-meta':
        meta = {'qname': qname, 'version': version, 'layout': 'sub-meta'}
        if 'submeta' in entry:
            meta['submeta'] = [m for e in entry['submeta']
                               if (m := build_meta_entry(e, source_path)) is not None]
        return meta

    # Normal data-region field.
    wire = atom.get('wire')
    if wire is None:
        sys.stderr.write(
            f"fss_compile.py: no wire (CDDL type) for atom {qname!r} in {source_path}\n"
        )
        sys.exit(2)

    byte_offset = entry['byte_offset']

    # Determine byte_size.  Resolution order:
    #  1. explicit byte_size in the entry (override; useful for debug)
    #  2. layout fss:local-ptr / -nul: implicit 8 (= sizeof(void*) on 64-bit)
    #  3. derived from the CDDL wire fragment (uint/int .size N, float*, bool)
    #  4. bstr / tstr: no implicit size — error
    if 'byte_size' in entry:
        byte_size = entry['byte_size']
    elif layout in ('fss:local-ptr', 'fss:local-ptr-nul'):
        byte_size = 8
    else:
        try:
            derived = wire_byte_size(wire)
        except ValueError:
            sys.stderr.write(
                f"fss_compile.py: wire {wire!r} for atom {qname!r} is outside "
                f"the CDDL flat-layout profile in {source_path}\n"
            )
            sys.exit(2)
        if derived is None:
            sys.stderr.write(
                f"fss_compile.py: wire {wire!r} for atom {qname!r} has no implicit "
                f"width; the composition entry requires an explicit byte_size in "
                f"{source_path}\n"
            )
            sys.exit(2)
        byte_size = derived

    meta = {
        'qname':       qname,
        'version':     version,
        'wire':        wire,
        'byte_offset': byte_offset,
        'byte_size':   byte_size,
    }

    if layout is not None:
        meta['layout'] = layout

    if 'attrs' in entry:
        meta['attrs'] = entry['attrs']

    # Per-atom attributes.  Defaults: serializable=True, in_hash=True.  The
    # live meta retains serializable:false atoms: they can define the identity
    # and layout of a live carrier (for example core::TypeId).  Flattening drops
    # them when it produces portable wire meta.  A composition entry overrides
    # the atom's own default.
    #   in_hash=False      → excluded from the carrier-id hash: transmitted but
    #                        not identity-defining.
    serializable = entry.get('serializable', atom.get('serializable', True))
    in_hash      = entry.get('in_hash',      atom.get('in_hash',      True))
    if not serializable:
        meta['serializable'] = False
    if not in_hash:
        meta['in_hash'] = False

    # Effective enum encoding: composition `encoding` override, else the atom's
    # canonical `default_encoding`.  Recorded in the meta (and thus the id) so
    # the carrier self-describes its value semantics; a bridge translates
    # between carriers that share the meaning names but differ in wire values.
    encoding = entry.get('encoding', atom.get('default_encoding'))
    if encoding is not None:
        meta['encoding'] = encoding

    return meta

# ---------------------------------------------------------------------------
# Canonical CBOR meta + id
# ---------------------------------------------------------------------------

def build_cbor_meta(meta_entries: list) -> bytes:
    """Encode the live meta as a canonical CBOR array."""
    return cbor_encode(meta_entries)

def build_cbor_meta_for_id(meta_entries: list) -> bytes:
    """
    Encode the meta for id computation.  A sub-meta slot contributes to the
    base carrier id, but its nested extension content does not.  Therefore a
    carrier with an extension hook differs from one without it, while changing
    the hook's nested extension chain leaves the base id unchanged.
    """
    return cbor_encode(meta_id_entries(meta_entries))

def meta_id_entries(meta_entries: list) -> list:
    """The entry list that defines the carrier id.

    Drop in_hash=False entries.  Retain each sub-meta slot as its base metadata
    and remove only its nested content: the extension hook is identity-defining,
    but its extension chain is not.
    """
    out = []
    for entry in meta_entries:
        if entry.get('in_hash') is False:
            continue
        if entry.get('layout') == 'sub-meta':
            out.append({key: value for key, value in entry.items()
                        if key != 'submeta'})
        else:
            out.append(entry)
    return out

# ---------------------------------------------------------------------------
# The compiler produces live meta.  Flattening produces portable wire meta by
# removing process-local entries and recalculating its identity.
# ---------------------------------------------------------------------------

def flat_meta_entries(meta_entries: list) -> list:
    """Portable wire entries: every serializable live entry, verbatim."""
    return [e for e in meta_entries if e.get('serializable') is not False]

def flat_id(meta_entries: list) -> int:
    """Portable wire id after serializable:false entries are dropped."""
    flat = [e for e in meta_id_entries(meta_entries) if e.get('serializable') is not False]
    return fnv1a_64(cbor_encode(flat))

# ---------------------------------------------------------------------------
# Compile a shape → (name, meta_entries, cbor_bytes, carrier_id)
# ---------------------------------------------------------------------------

def is_round_trip(atoms_list: list) -> bool:
    """A protocol is round-trip iff some atom is written by the target
    (`mutable_by == 'target'`) — that write-back is what requires a return
    path; otherwise the protocol is one-way (send-only).  Read from the RAW
    resolved shape: `mutable_by` is intrinsic atom documentation and is
    dropped from the wire-format meta (build_meta_entry), so it is not on
    meta_entries.  Invariant under flatten (no target-mutable atom is
    serializable:false), so the live and flat forms agree."""
    def target_writes(entry):
        if entry.get('atom', {}).get('mutable_by') == 'target':
            return True
        return any(target_writes(child) for child in entry.get('submeta', []))

    return any(target_writes(entry) for entry in atoms_list)

def compile_shape(shape_path: str):
    """Resolve a shape file and return its name, live meta entries, canonical
    CBOR meta bytes, carrier id, and round-trip flag."""
    shape = resolve_jsonnet(shape_path)
    carrier_name = shape.get('name', '<unnamed>')
    atoms_list   = shape.get('atoms', [])

    meta_entries = [m for entry in atoms_list
                    if (m := build_meta_entry(entry, shape_path)) is not None]
    cbor_bytes   = build_cbor_meta(meta_entries)
    carrier_id   = fnv1a_64(build_cbor_meta_for_id(meta_entries))
    round_trip   = is_round_trip(atoms_list)
    return carrier_name, meta_entries, cbor_bytes, carrier_id, round_trip

# ---------------------------------------------------------------------------
# C-header emitter — the carrier's id + canonical meta bytes as C, for a
# build-time generated header (e.g. fss/known_carriers/tlm_id.h).  The id and
# meta are the drift-prone values; emitting them keeps the C binding in lock-
# step with the composition (the single source of truth) without hand-copying.
# ---------------------------------------------------------------------------

def emit_c_header(carrier_name: str, cbor_bytes: bytes, id_cbor_bytes: bytes,
                  carrier_id: int, prefix: str, round_trip: bool) -> str:
    """Return C source defining:
       <PREFIX>_ID            the carrier id,
       <PREFIX>_ROUND_TRIP    1 iff the protocol is round-trip (derived),
        <PREFIX>_META[]/_LEN   the full canonical live meta,
        <PREFIX>_ID_META[]/_LEN  the exact canonical bytes hashed to get _ID.
     _ID_META excludes non-identity metadata such as nested extension content."""
    guard = f"FSS_GEN_{prefix}_H"
    def dump(bs):
        rows = []
        for i in range(0, len(bs), 12):
            chunk = bs[i:i+12]
            rows.append('    ' + ' '.join(f'0x{b:02x},' for b in chunk))
        return '\n'.join(rows)

    return (
        f"/* Generated by tools/fss_compile.py from the {carrier_name!r} composition.\n"
        f" * DO NOT EDIT — regenerate via the build.  Carrier id + canonical CBOR\n"
        f" * meta for the {carrier_name!r} carrier (the values that must track the\n"
        f" * Jsonnet composition exactly). */\n"
        f"#ifndef {guard}\n"
        f"#define {guard}\n\n"
        f"#include <stdint.h>\n"
        f"#include <stddef.h>\n\n"
        f"#define {prefix}_ID UINT64_C(0x{carrier_id:016x})\n\n"
        f"/* 1 iff the composition has a mutable_by:'target' atom (the target writes\n"
        f" * a value the initiator reads back) — so the channel needs a return path.\n"
        f" * Derived from the carrier, not configured per channel. */\n"
        f"#define {prefix}_ROUND_TRIP {1 if round_trip else 0}\n\n"
        f"static const uint8_t {prefix}_META[] = {{\n"
        f"{dump(cbor_bytes)}\n"
        f"}};\n"
        f"static const size_t {prefix}_META_LEN = sizeof({prefix}_META);\n\n"
        f"/* The exact canonical CBOR hashed to produce {prefix}_ID.  It excludes\n"
        f" * transmitted metadata that does not define base identity. */\n"
        f"static const uint8_t {prefix}_ID_META[] = {{\n"
        f"{dump(id_cbor_bytes)}\n"
        f"}};\n"
        f"static const size_t {prefix}_ID_META_LEN = sizeof({prefix}_ID_META);\n\n"
        f"#endif /* {guard} */\n"
    )

# ---------------------------------------------------------------------------
# Rust-const emitter — the FLAT (wire) form of a carrier as Rust constants, for
# a non-C++ artefact that authors/consumes the flat carrier without a CBOR meta
# parser.  A Rust initiator attaches <PREFIX>_FLAT_ID + <PREFIX>_FLAT_META to its
# carriers; a Rust consumer gates on <PREFIX>_FLAT_ID and reads fields at the
# generated <PREFIX>_OFF_* offsets (the flat layout == the live layout minus the
# dropped serializable:false atoms — see fss_meta_flatten).  Consequently, the
# live and flat ids can differ.
# ---------------------------------------------------------------------------

def emit_rust_flat(carrier_name: str, meta_entries: list, prefix: str) -> str:
    """Return Rust source defining the flat carrier's id, canonical meta bytes,
    and a byte-offset constant per field (qname `a::b` → `<PREFIX>_OFF_a_b`)."""
    flat_entries = flat_meta_entries(meta_entries)
    flat_bytes   = cbor_encode(flat_entries)
    fid          = flat_id(meta_entries)
    live_id      = fnv1a_64(build_cbor_meta_for_id(meta_entries))

    def dump(bs):
        rows = []
        for i in range(0, len(bs), 12):
            chunk = bs[i:i+12]
            rows.append('    ' + ' '.join(f'0x{b:02x},' for b in chunk))
        return '\n'.join(rows)

    offs = []
    for e in flat_entries:
        if e.get('layout') == 'sub-meta':
            continue
        ident = e['qname'].replace('::', '_')
        offs.append(f"pub const {prefix}_OFF_{ident}: usize = {e['byte_offset']};"
                    f"  // {e['qname']} ({e['wire']}{', '+e['layout'] if 'layout' in e else ''})")

    return (
        f"// Generated by tools/fss_compile.py — flat (wire) form of {carrier_name!r}.\n"
        f"// DO NOT EDIT — regenerate via the build.  The flat carrier drops the\n"
        f"// process-local atoms (e.g. core::TypeId) and keeps the rest VERBATIM\n"
        f"// (offsets unchanged).  These constants let a non-C++ artefact author and\n"
        f"// consume it directly.  Include within a module, e.g.\n"
        f"//   #[allow(dead_code)] mod {prefix.lower()}_flat {{ include!(concat!(env!(\"...\"))); }}\n"
        f"//\n"
        f"// Process-local atoms such as TypeId are absent from FLAT_META.  If one\n"
        f"// contributes to live identity, LIVE_ID and FLAT_ID differ.\n\n"
        f"pub const {prefix}_LIVE_ID: u64 = 0x{live_id:016x};\n"
        f"pub const {prefix}_FLAT_ID: u64 = 0x{fid:016x};\n\n"
        f"pub const {prefix}_FLAT_META: &[u8] = &[\n"
        f"{dump(flat_bytes)}\n"
        f"];\n\n"
        + '\n'.join(offs) + '\n'
    )

# ---------------------------------------------------------------------------
# Python-const emitter — the FLAT (wire) form of a carrier as Python constants,
# the analogue of emit_rust_flat for a Python-loader artefact.  The model builds
# the flat data region with `struct` and reads fields at the generated offsets.
# ---------------------------------------------------------------------------

def emit_py_flat(carrier_name: str, meta_entries: list, prefix: str) -> str:
    """Return Python source defining the flat carrier's id, canonical meta bytes,
    and a byte-offset constant per field (qname `a::b` → `<PREFIX>_OFF_a_b`)."""
    flat_entries = flat_meta_entries(meta_entries)
    flat_bytes   = cbor_encode(flat_entries)
    fid          = flat_id(meta_entries)
    live_id      = fnv1a_64(build_cbor_meta_for_id(meta_entries))

    offs = []
    for e in flat_entries:
        if e.get('layout') == 'sub-meta':
            continue
        ident = e['qname'].replace('::', '_')
        offs.append(f"{prefix}_OFF_{ident} = {e['byte_offset']}"
                    f"  # {e['qname']} ({e['wire']}{', '+e['layout'] if 'layout' in e else ''})")

    return (
        f"# Generated by tools/fss_compile.py — flat (wire) form of {carrier_name!r}.\n"
        f"# DO NOT EDIT — regenerate via the build.  The flat carrier drops the\n"
        f"# process-local atoms (e.g. core::TypeId) and keeps the rest VERBATIM\n"
        f"# (offsets unchanged).  These constants let a Python-loader artefact author\n"
        f"# and consume it directly.\n"
        f"#\n"
        f"# Process-local atoms such as TypeId are absent from FLAT_META.  If one\n"
        f"# contributes to live identity, LIVE_ID and FLAT_ID differ.\n\n"
        f"{prefix}_LIVE_ID = 0x{live_id:016x}\n"
        f"{prefix}_FLAT_ID = 0x{fid:016x}\n\n"
        f"{prefix}_FLAT_META = bytes.fromhex(\n"
        f"    {flat_bytes.hex()!r}\n"
        f")\n\n"
        + '\n'.join(offs) + '\n'
    )

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

_USAGE = (
    "usage: fss_compile.py <shape.jsonnet>\n"
    "       fss_compile.py --emit-c-header  <PREFIX> <shape.jsonnet> [out.h]\n"
    "       fss_compile.py --emit-rust-flat <PREFIX> <shape.jsonnet> [out.rs]\n"
    "       fss_compile.py --emit-py-flat   <PREFIX> <shape.jsonnet> [out.py]\n"
    "\n"
    "Compile an FSS carrier shape to its canonical wire-format meta and\n"
    "compute the carrier id.\n"
    "\n"
    "Default mode prints a human-readable summary (name, id, layout, CBOR dump).\n"
    "--emit-c-header writes C defining <PREFIX>_ID / <PREFIX>_META[] /\n"
    "<PREFIX>_META_LEN (to stdout, or to out.h if given).\n"
    "--emit-rust-flat writes Rust defining the FLAT carrier's <PREFIX>_FLAT_ID /\n"
    "<PREFIX>_FLAT_META / <PREFIX>_OFF_* field offsets (for a non-C++ artefact).\n"
)

def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]

    if argv and argv[0] in ('--emit-c-header', '--emit-rust-flat', '--emit-py-flat'):
        mode = argv[0]
        if len(argv) not in (3, 4):
            sys.stderr.write(_USAGE)
            sys.exit(1)
        prefix     = argv[1]
        shape_path = os.path.abspath(argv[2])
        out_path   = argv[3] if len(argv) == 4 else None
        if not os.path.isfile(shape_path):
            sys.stderr.write(f"fss_compile.py: file not found: {shape_path}\n")
            sys.exit(1)
        name, _entries, cbor_bytes, carrier_id, round_trip = compile_shape(shape_path)
        if mode == '--emit-c-header':
            id_cbor = build_cbor_meta_for_id(_entries)
            text = emit_c_header(name, cbor_bytes, id_cbor, carrier_id, prefix, round_trip)
        elif mode == '--emit-rust-flat':
            text = emit_rust_flat(name, _entries, prefix)
        else:
            text = emit_py_flat(name, _entries, prefix)
        if out_path:
            with open(out_path, 'w') as f:
                f.write(text)
        else:
            sys.stdout.write(text)
        sys.exit(0)

    if len(argv) != 1 or argv[0] in ('-h', '--help'):
        print(_USAGE, file=sys.stderr if len(argv) != 1 else sys.stdout)
        sys.exit(0 if len(argv) == 1 else 1)

    shape_path = os.path.abspath(argv[0])
    if not os.path.isfile(shape_path):
        sys.stderr.write(f"fss_compile.py: file not found: {shape_path}\n")
        sys.exit(1)

    carrier_name, meta_entries, cbor_bytes, carrier_id, round_trip = compile_shape(shape_path)

    # ----- Output -----
    print(f"name:        {carrier_name}")
    print(f"source:      {shape_path}")
    print(f"carrier id:  {carrier_id:016x}")
    print(f"round-trip:  {'yes' if round_trip else 'no'}  (round-trip ⇔ a mutable_by:'target' atom)")
    print()
    print("atom layout:")
    for m in meta_entries:
        if m.get('layout') == 'sub-meta':
            n = len(m.get('submeta', []))
            extra = f"  (+{n} nested)" if n else ''
            print(f"  {'(meta-slot)':>22s}  {m['qname']!s:<32s}  [sub-meta]{extra}")
            continue
        type_str = m['wire']
        if 'layout' in m:
            type_str += f" [{m['layout']}]"
        attrs_str = ''
        if 'attrs' in m:
            attrs_str = '  ' + ', '.join(f"{k}={v!r}" for k, v in m['attrs'].items())
        if 'encoding' in m:
            attrs_str += '  enc={' + ', '.join(f"{k}={v}" for k, v in m['encoding'].items()) + '}'
        print(f"  offset={m['byte_offset']:4d}  size={m['byte_size']:4d}  {m['qname']!s:<32s}  {type_str}{attrs_str}")
    print()
    print(f"CBOR meta:   {len(cbor_bytes)} bytes")
    # Hex dump, 16 bytes per line
    for i in range(0, len(cbor_bytes), 16):
        chunk = cbor_bytes[i:i+16]
        hex_part = ' '.join(f'{b:02x}' for b in chunk)
        print(f"  {i:04x}  {hex_part}")


if __name__ == '__main__':
    main()
