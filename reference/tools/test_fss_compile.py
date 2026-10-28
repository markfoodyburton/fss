#!/usr/bin/env python3
# This file is part of fss
# Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
# Author: Mark Burton
#
# SPDX-License-Identifier: BSD-3-Clause

"""
test_fss_compile.py — tests for fss_compile.py.

Run directly:
    reference/tools/test_fss_compile.py

Exit 0 on all-pass, 1 on any failure.
Prints one line per test: PASS test_xxx or FAIL test_xxx: <reason>.

No external dependencies (no pytest).  Uses subprocess to call
fss_compile.py so we test it end-to-end.
"""

import os
import re
import subprocess
import sys
import tempfile
from typing import Optional

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Resolve paths relative to the repo root (two levels up from reference/tools/)
_HERE      = os.path.dirname(os.path.abspath(__file__))
_REPO      = os.path.dirname(os.path.dirname(_HERE))
_COMPILE   = os.path.join(_HERE, 'fss_compile.py')
sys.path.insert(0, _HERE)
import fss_compile as _compiler

TLM_SHAPE    = os.path.join(_REPO, 'known_carriers', 'tlm', 'transaction.jsonnet')
CONFIG_SHAPE = os.path.join(_REPO, 'protocols', 'config', 'param_request.jsonnet')
SIM_SHAPE    = os.path.join(_REPO, 'protocols', 'sim-events', 'event.jsonnet')

_ID_RE = re.compile(r'^[0-9a-f]{16}$')

_results = []

def run(path: str):
    """Run fss_compile.py on a shape file; return (returncode, stdout, stderr)."""
    result = subprocess.run(
        [sys.executable, _COMPILE, path],
        capture_output=True,
        text=True,
    )
    return result.returncode, result.stdout, result.stderr

def extract_id(stdout: str) -> Optional[str]:
    """Pull the carrier id out of fss_compile.py's output."""
    for line in stdout.splitlines():
        if line.startswith('carrier id:'):
            return line.split(':', 1)[1].strip()
    return None

def record(name: str, passed: bool, reason: str = ''):
    tag = 'PASS' if passed else 'FAIL'
    msg = f"{tag} {name}"
    if not passed and reason:
        msg += f": {reason}"
    print(msg)
    _results.append(passed)

# ---------------------------------------------------------------------------
# Test 1 — all three shapes compile cleanly and produce a valid id
# ---------------------------------------------------------------------------

def test_tlm_compiles():
    rc, out, err = run(TLM_SHAPE)
    if rc != 0:
        record('test_tlm_compiles', False, f"exit {rc}: {err.strip()}")
        return
    carrier_id = extract_id(out)
    if carrier_id is None:
        record('test_tlm_compiles', False, "no 'carrier id:' line in output")
        return
    if not _ID_RE.match(carrier_id):
        record('test_tlm_compiles', False, f"id {carrier_id!r} does not match [0-9a-f]{{16}}")
        return
    record('test_tlm_compiles', True)
    return carrier_id

def test_config_compiles():
    rc, out, err = run(CONFIG_SHAPE)
    if rc != 0:
        record('test_config_compiles', False, f"exit {rc}: {err.strip()}")
        return
    carrier_id = extract_id(out)
    if carrier_id is None:
        record('test_config_compiles', False, "no 'carrier id:' line in output")
        return
    if not _ID_RE.match(carrier_id):
        record('test_config_compiles', False, f"id {carrier_id!r} does not match [0-9a-f]{{16}}")
        return
    record('test_config_compiles', True)
    return carrier_id

def test_sim_compiles():
    rc, out, err = run(SIM_SHAPE)
    if rc != 0:
        record('test_sim_compiles', False, f"exit {rc}: {err.strip()}")
        return
    carrier_id = extract_id(out)
    if carrier_id is None:
        record('test_sim_compiles', False, "no 'carrier id:' line in output")
        return
    if not _ID_RE.match(carrier_id):
        record('test_sim_compiles', False, f"id {carrier_id!r} does not match [0-9a-f]{{16}}")
        return
    record('test_sim_compiles', True)
    return carrier_id

# ---------------------------------------------------------------------------
# Test 2 — id is reproducible (running twice gives the same result)
# ---------------------------------------------------------------------------

def test_tlm_id_reproducible():
    _, out1, _ = run(TLM_SHAPE)
    _, out2, _ = run(TLM_SHAPE)
    id1 = extract_id(out1)
    id2 = extract_id(out2)
    if id1 is None or id2 is None:
        record('test_tlm_id_reproducible', False, "could not extract id from one or both runs")
        return
    if id1 != id2:
        record('test_tlm_id_reproducible', False, f"first={id1!r}, second={id2!r}")
        return
    record('test_tlm_id_reproducible', True)

# ---------------------------------------------------------------------------
# Synthetic shape template helpers
# ---------------------------------------------------------------------------

# Atoms barrel imports, relative to /tmp (we'll use absolute paths in the
# synthetic jsonnet so they resolve regardless of cwd)
_ATOMS_DIR = os.path.join(_REPO, 'atoms')

def _tlm_with_extension() -> str:
    """
    Synthetic shape: the real TLM core, but with core::Extensions carrying a
    `submeta` (an extension field describing further bytes in the same data
    region).  The id rule excludes the submeta, so the id MUST equal the real
    TLM id.
    """
    return f"""
local bus  = import '{_ATOMS_DIR}/bus.libsonnet';
local core = import '{_ATOMS_DIR}/core.libsonnet';

{{
  name: 'test::tlm_ext',
  atoms: [
    {{ atom: core.TypeId,          byte_offset:  0, byte_size: 8 }},
    {{ atom: bus.Locator,          byte_offset:  8, byte_size: 8 }},
    {{ atom: bus.Operation,        byte_offset: 16, byte_size: 4 }},
    {{ atom: bus.Data,             byte_offset: 24,
       layout: 'fss:local-ptr',
       attrs:    {{ length_at: 'bus::Length' }} }},
    {{ atom: bus.Length,           byte_offset: 32, byte_size: 4 }},
    {{ atom: bus.Status,           byte_offset: 36, byte_size: 4 }},
    {{ atom: bus.DmiAllowed,       byte_offset: 40 }},
    {{ atom: bus.ByteEnable,       byte_offset: 48,
       layout: 'fss:local-ptr',
       attrs:  {{ length_at: 'bus::ByteEnableLength' }} }},
    {{ atom: bus.ByteEnableLength, byte_offset: 56 }},
    {{ atom: bus.StreamingWidth,   byte_offset: 60 }},
    {{ atom: {{ qname: 'tlm::GpOption', version: 1, wire: 'uint',
                default_encoding: {{ MinPayload: 0, FullPayload: 1, FullPayloadAccepted: 2 }},
                mutable_by: 'initiator', read_by: 'target', phase: 'request' }},
       byte_offset: 64, byte_size: 4 }},
    {{ atom: core.Extensions, submeta: [
       // An extension field in the SAME data region, beyond the GP object.
       {{ atom: bus.Locator, byte_offset: 72, byte_size: 8 }},
    ] }},
  ],
}}
"""

def _tlm_shifted_address(address_offset: int) -> str:
    """
    Synthetic shape: the real TLM core but with bus::Locator at a
    different byte_offset.
    """
    return f"""
local bus  = import '{_ATOMS_DIR}/bus.libsonnet';
local core = import '{_ATOMS_DIR}/core.libsonnet';

{{
  name: 'test::tlm_shifted',
  atoms: [
    {{ atom: bus.Locator,    byte_offset: {address_offset}, byte_size: 8 }},
    {{ atom: bus.Operation,  byte_offset: 8, byte_size: 4 }},
    {{ atom: bus.Length,     byte_offset: 12, byte_size: 4 }},
    {{ atom: bus.Data,       byte_offset: 16,
       layout: 'fss:local-ptr',
       attrs:    {{ length_at: 'bus::Length' }} }},
    {{ atom: bus.Status,     byte_offset: 24, byte_size: 4 }},
    {{ atom: core.Extensions }},
  ],
}}
"""

def _local_ptr_no_byte_size() -> str:
    """
    Synthetic shape with a single fss:local-ptr entry, no explicit byte_size.
    Uses config::ParamName (intrinsic layout fss:local-ptr-nul, overridden to
    fss:local-ptr here to test the fss:local-ptr implicit-8 path).
    """
    return f"""
local config = import '{_ATOMS_DIR}/config.libsonnet';

{{
  name: 'test::ptr_no_byte_size',
  atoms: [
    {{ atom: config.OpCode,    byte_offset: 0 }},
    {{ atom: config.ParamName, byte_offset: 8,
       layout: 'fss:local-ptr' }},
  ],
}}
"""

def _shape_with_type_id(include_type_id: bool) -> str:
    type_id = "{ atom: core.TypeId, byte_offset: 0, byte_size: 8 }," if include_type_id else ""
    return f"""
local bus  = import '{_ATOMS_DIR}/bus.libsonnet';
local core = import '{_ATOMS_DIR}/core.libsonnet';
{{ name: 'test::type_id', atoms: [
    {type_id}
    {{ atom: bus.Locator, byte_offset: 8, byte_size: 8 }},
] }}
"""

def _shape_with_direction(mutable_by: str) -> str:
    return f"""
{{ name: 'test::direction', atoms: [
    {{ atom: {{ qname: 'test::Field', version: 1, wire: 'uint',
               mutable_by: '{mutable_by}' }}, byte_offset: 0, byte_size: 4 }},
] }}
"""

def _write_tmp_jsonnet(content: str) -> str:
    fd, path = tempfile.mkstemp(suffix='.jsonnet')
    os.close(fd)
    with open(path, 'w') as f:
        f.write(content)
    return path

# ---------------------------------------------------------------------------
# Test 3 — sub-meta (extension) exclusion rule
#
# A TLM shape whose core::Extensions carries a submeta MUST produce the same id
# as the real TLM shape: the hook itself defines the base identity, but its
# extension chain is excluded from that identity.
# ---------------------------------------------------------------------------

def test_submeta_exclusion(tlm_id: str):
    if tlm_id is None:
        record('test_submeta_exclusion', False, "skipped: TLM id not available")
        return

    tmp = _write_tmp_jsonnet(_tlm_with_extension())
    try:
        rc, out, err = run(tmp)
    finally:
        os.unlink(tmp)

    if rc != 0:
        record('test_submeta_exclusion', False, f"exit {rc}: {err.strip()}")
        return

    synthetic_id = extract_id(out)
    if synthetic_id is None:
        record('test_submeta_exclusion', False, "no 'carrier id:' in synthetic output")
        return

    if synthetic_id != tlm_id:
        record('test_submeta_exclusion', False,
               f"expected id={tlm_id!r} (same extension hook as TLM), got "
               f"{synthetic_id!r}; nested sub-meta exclusion rule not applied")
        return

    record('test_submeta_exclusion', True)

# ---------------------------------------------------------------------------
# Test 4 — changing a core atom's byte_offset DOES change the id
# ---------------------------------------------------------------------------

def test_core_offset_changes_id(tlm_id: str):
    if tlm_id is None:
        record('test_core_offset_changes_id', False, "skipped: TLM id not available")
        return

    tmp = _write_tmp_jsonnet(_tlm_shifted_address(address_offset=4))
    try:
        rc, out, err = run(tmp)
    finally:
        os.unlink(tmp)

    if rc != 0:
        record('test_core_offset_changes_id', False, f"exit {rc}: {err.strip()}")
        return

    shifted_id = extract_id(out)
    if shifted_id is None:
        record('test_core_offset_changes_id', False, "no 'carrier id:' in shifted output")
        return

    if shifted_id == tlm_id:
        record('test_core_offset_changes_id', False,
               f"id unchanged ({tlm_id!r}) despite different byte_offset; "
               f"core field not contributing to hash")
        return

    record('test_core_offset_changes_id', True)

# ---------------------------------------------------------------------------
# Test 5 — core::Extensions is a meta-slot carrying no data-region bytes
#
# The real TLM shape's core::Extensions entry MUST render as a sub-meta
# meta-slot (no byte_offset / byte_size of its own).
# ---------------------------------------------------------------------------

def test_submeta_is_meta_slot():
    rc, out, err = run(TLM_SHAPE)
    if rc != 0:
        record('test_submeta_is_meta_slot', False, f"exit {rc}: {err.strip()}")
        return
    # The core::Extensions line must be the meta-slot form, not a sized field.
    ext_lines = [l for l in out.splitlines() if 'core::Extensions' in l]
    if not ext_lines:
        record('test_submeta_is_meta_slot', False, "no core::Extensions line in layout")
        return
    line = ext_lines[0]
    if '[sub-meta]' not in line or 'size=' in line or 'offset=' in line:
        record('test_submeta_is_meta_slot', False,
               f"core::Extensions is not a bare sub-meta slot: {line.strip()!r}")
        return
    record('test_submeta_is_meta_slot', True)

# ---------------------------------------------------------------------------
# Test 6 — implicit byte_size for fss:local-ptr (8)
#
# A synthetic shape using fss:local-ptr with no explicit byte_size MUST
# compile cleanly and show size=8 in the layout summary.
# ---------------------------------------------------------------------------

def test_implicit_byte_size_local_ptr():
    tmp = _write_tmp_jsonnet(_local_ptr_no_byte_size())
    try:
        rc, out, err = run(tmp)
    finally:
        os.unlink(tmp)

    if rc != 0:
        record('test_implicit_byte_size_local_ptr', False, f"exit {rc}: {err.strip()}")
        return

    if 'size=   8' not in out and 'size=8' not in out:
        record('test_implicit_byte_size_local_ptr', False,
               "layout does not show size=8 for fss:local-ptr entry")
        return

    record('test_implicit_byte_size_local_ptr', True)

def test_derived_byte_size_is_transmitted():
    """Fixed-width source types may derive a width, but wire metadata always
    carries byte_size."""
    tmp = _write_tmp_jsonnet(f"""
local bus = import '{_ATOMS_DIR}/bus.libsonnet';
{{ name: 'test::derived_size', atoms: [
    {{ atom: bus.DmiAllowed, byte_offset: 0 }},
] }}
""")
    try:
        rc, out, err = run(tmp)
    finally:
        os.unlink(tmp)
    if rc != 0:
        record('test_derived_byte_size_is_transmitted', False, f"exit {rc}: {err.strip()}")
        return
    line = next((line for line in out.splitlines() if 'bus::DmiAllowed' in line), '')
    if 'size=' not in line or not re.search(r'size=\s*1\b', line):
        record('test_derived_byte_size_is_transmitted', False,
               "fixed-width field has no transmitted byte_size=1")
        return
    record('test_derived_byte_size_is_transmitted', True)

# ---------------------------------------------------------------------------
# Test 7 — deferred encoding: bus::Operation at byte_size 1 with an encoding
# override produces a meta entry that carries the encoding dict, and a
# different id than a shape that uses the default encoding.
# ---------------------------------------------------------------------------

def _operation_with_encoding_override() -> str:
    """
    Tiny shape: bus::Operation at byte_size 1 with encoding {Write:0, Read:1}.
    Must produce a meta entry that carries that encoding (not the default
    {Read:0, Write:1, Ignore:2}) and a distinct carrier id from a shape that
    uses the default encoding.
    """
    return f"""
local bus = import '{_ATOMS_DIR}/bus.libsonnet';

{{
  name: 'test::op_enc_override',
  atoms: [
    {{ atom: bus.Operation, byte_offset: 0, byte_size: 1,
       encoding: {{ Write: 0, Read: 1 }} }},
  ],
}}
"""

def _operation_default_encoding() -> str:
    """
    Tiny shape: bus::Operation at byte_size 1 with no encoding override
    (uses default_encoding {Read:0, Write:1, Ignore:2}).
    """
    return f"""
local bus = import '{_ATOMS_DIR}/bus.libsonnet';

{{
  name: 'test::op_default_enc',
  atoms: [
    {{ atom: bus.Operation, byte_offset: 0, byte_size: 1 }},
  ],
}}
"""

def test_encoding_deferral():
    """
    1. The override shape's layout must show enc={Write=0, Read=1}.
    2. The override shape's id must differ from the default-encoding shape's id.
    """
    tmp_override = _write_tmp_jsonnet(_operation_with_encoding_override())
    tmp_default  = _write_tmp_jsonnet(_operation_default_encoding())
    try:
        rc1, out1, err1 = run(tmp_override)
        rc2, out2, err2 = run(tmp_default)
    finally:
        os.unlink(tmp_override)
        os.unlink(tmp_default)

    if rc1 != 0:
        record('test_encoding_deferral', False, f"override shape exit {rc1}: {err1.strip()}")
        return
    if rc2 != 0:
        record('test_encoding_deferral', False, f"default shape exit {rc2}: {err2.strip()}")
        return

    # Check encoding appears in layout output.
    enc_lines = [l for l in out1.splitlines() if 'bus::Operation' in l and 'enc=' in l]
    if not enc_lines:
        record('test_encoding_deferral', False,
               "override shape layout does not show enc= for bus::Operation")
        return
    enc_line = enc_lines[0]
    if 'Write=0' not in enc_line or 'Read=1' not in enc_line:
        record('test_encoding_deferral', False,
               f"encoding values wrong in layout: {enc_line.strip()!r}")
        return

    # Check ids differ.
    id1 = extract_id(out1)
    id2 = extract_id(out2)
    if id1 is None or id2 is None:
        record('test_encoding_deferral', False, "could not extract ids")
        return
    if id1 == id2:
        record('test_encoding_deferral', False,
               f"encoding override did not change carrier id ({id1!r})")
        return

    record('test_encoding_deferral', True)

# ---------------------------------------------------------------------------
# Test 8 — wire form: a carrier with fss:local-ptr fields emits a DISTINCT
# wire-form id + meta (pointer fields inlined); a self-contained carrier
# (no pointers) emits no wire block.
# ---------------------------------------------------------------------------

def _emit_header(prefix: str, path: str):
    """Run fss_compile.py --emit-c-header; return (rc, stdout, stderr)."""
    result = subprocess.run(
        [sys.executable, _COMPILE, '--emit-c-header', prefix, path],
        capture_output=True, text=True,
    )
    return result.returncode, result.stdout, result.stderr

def test_emits_id_meta():
    rc, out, err = _emit_header('FSS_TLM', TLM_SHAPE)
    if rc != 0:
        record('test_emits_id_meta', False, f"exit {rc}: {err.strip()}")
        return
    # The header must emit the exact pre-hash bytes (_ID_META) so a runtime can
    # derive related ids (e.g. the live form) without re-encoding the meta.
    if 'FSS_TLM_ID_META[]' not in out or 'FSS_TLM_ID_META_LEN' not in out:
        record('test_emits_id_meta', False, "header missing _ID_META")
        return
    record('test_emits_id_meta', True)

# ---------------------------------------------------------------------------
# Test 9 — per-atom attributes (serializable / in_hash)
#
#  - serializable=False remains in live meta and can define the live id.  It is
#    removed only when a portable flat form is produced.
#  - in_hash=False is NOT identity-defining: adding such an atom leaves the
#    carrier id unchanged (transmitted but excluded from the hash).
# ---------------------------------------------------------------------------

def _shape_plain() -> str:
    return f"""
local bus = import '{_ATOMS_DIR}/bus.libsonnet';
{{ name: 'test::attrs_base', atoms: [
    {{ atom: bus.Locator, byte_offset: 0, byte_size: 8 }},
] }}
"""

def _shape_serializable_false() -> str:
    return f"""
local bus = import '{_ATOMS_DIR}/bus.libsonnet';
{{ name: 'test::attrs_ser', atoms: [
    {{ atom: bus.Locator, byte_offset: 0, byte_size: 8, serializable: false }},
] }}
"""

def _shape_extra_in_hash_false() -> str:
    return f"""
local bus = import '{_ATOMS_DIR}/bus.libsonnet';
{{ name: 'test::attrs_excl', atoms: [
    {{ atom: bus.Locator, byte_offset: 0, byte_size: 8 }},
    {{ atom: bus.Status,  byte_offset: 8, byte_size: 4, in_hash: false }},
] }}
"""

def test_attr_serializable_is_retained_in_live_meta_and_id():
    a = _write_tmp_jsonnet(_shape_plain())
    b = _write_tmp_jsonnet(_shape_serializable_false())
    try:
        rca, outa, _ = run(a); rcb, outb, _ = run(b)
    finally:
        os.unlink(a); os.unlink(b)
    if rca != 0 or rcb != 0:
        record('test_attr_serializable_is_retained_in_live_meta_and_id', False,
               "compile failed"); return
    if extract_id(outa) == extract_id(outb):
        record('test_attr_serializable_is_retained_in_live_meta_and_id', False,
               "serializable=False did not define a distinct live identity"); return
    if 'bus::Locator' not in outb or 'size=' not in outb:
        record('test_attr_serializable_is_retained_in_live_meta_and_id', False,
               "serializable=False atom is absent from live metadata"); return
    record('test_attr_serializable_is_retained_in_live_meta_and_id', True)

def test_attr_in_hash_excluded():
    base  = _write_tmp_jsonnet(_shape_plain())
    extra = _write_tmp_jsonnet(_shape_extra_in_hash_false())
    try:
        rcb, outb, _ = run(base); rce, oute, _ = run(extra)
    finally:
        os.unlink(base); os.unlink(extra)
    if rcb != 0 or rce != 0:
        record('test_attr_in_hash_excluded', False, "compile failed"); return
    if extract_id(outb) != extract_id(oute):
        record('test_attr_in_hash_excluded', False,
               "in_hash=False atom changed the id (must be excluded from the hash)"); return
    record('test_attr_in_hash_excluded', True)

# ---------------------------------------------------------------------------
# Test 10 — live metadata and identity retain process-local TypeId; flattening
# drops it and re-hashes to a distinct flat id.  An extension slot defines the
# base identity, but its nested content does not; direction derives from the
# atom's mutable_by annotation.
# ---------------------------------------------------------------------------

def test_type_id_live_and_flat_ids_differ():
    with_type_id = _write_tmp_jsonnet(_shape_with_type_id(True))
    without_type_id = _write_tmp_jsonnet(_shape_with_type_id(False))
    try:
        rc_a, out_a, err_a = run(with_type_id)
        rc_b, out_b, err_b = run(without_type_id)
        flat_result = subprocess.run(
            [sys.executable, _COMPILE, '--emit-py-flat', 'TEST_TYPE_ID', with_type_id],
            capture_output=True, text=True,
        )
    finally:
        os.unlink(with_type_id); os.unlink(without_type_id)
    if rc_a != 0 or rc_b != 0:
        record('test_type_id_live_and_flat_ids_differ', False,
               f"compile failed: {err_a.strip() or err_b.strip()}"); return
    live_id = extract_id(out_a)
    without_type_id_id = extract_id(out_b)
    if live_id == without_type_id_id:
        record('test_type_id_live_and_flat_ids_differ', False,
               "core::TypeId did not define the live carrier id"); return
    type_id_lines = [line for line in out_a.splitlines() if 'core::TypeId' in line]
    if not type_id_lines or not re.search(r'size=\s*8\b', type_id_lines[0]):
        record('test_type_id_live_and_flat_ids_differ', False,
               "live TypeId metadata is missing required byte_size=8"); return

    if flat_result.returncode != 0:
        record('test_type_id_live_and_flat_ids_differ', False,
               f"flat emit failed: {flat_result.stderr.strip()}"); return
    live_match = re.search(r'^TEST_TYPE_ID_LIVE_ID = 0x([0-9a-f]{16})$', flat_result.stdout, re.M)
    flat_match = re.search(r'^TEST_TYPE_ID_FLAT_ID = 0x([0-9a-f]{16})$', flat_result.stdout, re.M)
    if not live_match or not flat_match:
        record('test_type_id_live_and_flat_ids_differ', False,
               "flat emit did not contain live and flat ids"); return
    if (live_match.group(1) != live_id or live_match.group(1) == flat_match.group(1)
            or flat_match.group(1) != without_type_id_id):
        record('test_type_id_live_and_flat_ids_differ', False,
               "flattening did not drop TypeId and re-hash the identity"); return
    if 'TEST_TYPE_ID_OFF_core_TypeId' in flat_result.stdout:
        record('test_type_id_live_and_flat_ids_differ', False,
               "flat metadata retained core::TypeId"); return
    record('test_type_id_live_and_flat_ids_differ', True)

def test_extension_slot_identity_and_nested_content_exclusion():
    absent = _write_tmp_jsonnet(_shape_plain())
    bare = _write_tmp_jsonnet(f"""
local bus = import '{_ATOMS_DIR}/bus.libsonnet';
local core = import '{_ATOMS_DIR}/core.libsonnet';
{{ name: 'test::extension_base', atoms: [
    {{ atom: bus.Locator, byte_offset: 0, byte_size: 8 }},
    {{ atom: core.Extensions }},
] }}
""")
    nested_status = _write_tmp_jsonnet(f"""
local bus = import '{_ATOMS_DIR}/bus.libsonnet';
local core = import '{_ATOMS_DIR}/core.libsonnet';
{{ name: 'test::extension_base', atoms: [
    {{ atom: bus.Locator, byte_offset: 0, byte_size: 8 }},
    {{ atom: core.Extensions, submeta: [
        {{ atom: bus.Status, byte_offset: 8, byte_size: 4 }},
    ] }},
] }}
""")
    nested_operation = _write_tmp_jsonnet(f"""
local bus = import '{_ATOMS_DIR}/bus.libsonnet';
local core = import '{_ATOMS_DIR}/core.libsonnet';
{{ name: 'test::extension_base', atoms: [
    {{ atom: bus.Locator, byte_offset: 0, byte_size: 8 }},
    {{ atom: core.Extensions, submeta: [
        {{ atom: bus.Operation, byte_offset: 24, byte_size: 1 }},
    ] }},
] }}
""")
    try:
        rc_a, out_a, err_a = run(absent)
        rc_b, out_b, err_b = run(bare)
        rc_c, out_c, err_c = run(nested_status)
        rc_d, out_d, err_d = run(nested_operation)
        if rc_b == 0 and rc_c == 0 and rc_d == 0:
            bare_cbor = _compiler.compile_shape(bare)[2]
            status_cbor = _compiler.compile_shape(nested_status)[2]
            operation_cbor = _compiler.compile_shape(nested_operation)[2]
    finally:
        os.unlink(absent); os.unlink(bare)
        os.unlink(nested_status); os.unlink(nested_operation)
    if rc_a != 0 or rc_b != 0 or rc_c != 0 or rc_d != 0:
        record('test_extension_slot_identity_and_nested_content_exclusion', False,
               f"compile failed: {err_a.strip() or err_b.strip() or err_c.strip() or err_d.strip()}"); return
    absent_id = extract_id(out_a)
    bare_id = extract_id(out_b)
    status_id = extract_id(out_c)
    operation_id = extract_id(out_d)
    if absent_id == bare_id:
        record('test_extension_slot_identity_and_nested_content_exclusion', False,
               "core::Extensions marker did not define a distinct base identity"); return
    if bare_id != status_id or bare_id != operation_id:
        record('test_extension_slot_identity_and_nested_content_exclusion', False,
               "nested extension content changed the base id"); return
    if (bare_cbor == status_cbor or bare_cbor == operation_cbor
            or status_cbor == operation_cbor):
        record('test_extension_slot_identity_and_nested_content_exclusion', False,
               "serialized live metadata did not retain distinct nested content"); return
    record('test_extension_slot_identity_and_nested_content_exclusion', True)

def test_direction_from_target_mutability():
    target = _write_tmp_jsonnet(_shape_with_direction('target'))
    initiator = _write_tmp_jsonnet(_shape_with_direction('initiator'))
    try:
        rc_t, out_t, err_t = run(target)
        rc_i, out_i, err_i = run(initiator)
    finally:
        os.unlink(target); os.unlink(initiator)
    if rc_t != 0 or rc_i != 0:
        record('test_direction_from_target_mutability', False,
               f"compile failed: {err_t.strip() or err_i.strip()}"); return
    if 'round-trip:  yes' not in out_t or 'round-trip:  no' not in out_i:
        record('test_direction_from_target_mutability', False,
               "round-trip result does not follow mutable_by:'target'"); return
    record('test_direction_from_target_mutability', True)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    tlm_id    = test_tlm_compiles()
    test_config_compiles()
    test_sim_compiles()
    test_tlm_id_reproducible()
    test_submeta_exclusion(tlm_id)
    test_core_offset_changes_id(tlm_id)
    test_submeta_is_meta_slot()
    test_implicit_byte_size_local_ptr()
    test_derived_byte_size_is_transmitted()
    test_encoding_deferral()
    test_emits_id_meta()
    test_attr_serializable_is_retained_in_live_meta_and_id()
    test_attr_in_hash_excluded()
    test_type_id_live_and_flat_ids_differ()
    test_extension_slot_identity_and_nested_content_exclusion()
    test_direction_from_target_mutability()

    passed = sum(_results)
    total  = len(_results)
    print(f"\n{passed}/{total} tests passed")

    sys.exit(0 if passed == total else 1)


if __name__ == '__main__':
    main()
