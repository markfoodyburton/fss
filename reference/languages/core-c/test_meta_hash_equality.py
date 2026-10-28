#!/usr/bin/env python3
# This file is part of fss
# Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
# Author: Mark Burton
#
# SPDX-License-Identifier: BSD-3-Clause

"""
test_meta_hash_equality.py — assert the runtime C hash + flatten (fss/c/meta_hash.h,
fss_meta_flatten via the compiled test_meta_hash) agree bit-for-bit with the
compiler (reference/tools/fss_compile.py).

Usage: test_meta_hash_equality.py <path-to-fss_c_meta_hash_test>

Computes for the TLM carrier:
  - the live id = FSS_TLM_ID (the compiler's id of the complete, TypeId-bearing meta),
  - the flat id = the compiler's id of the meta with serializable:false dropped,
runs the C program, and checks the runtime values match.  Exit 0 on agreement.
"""
import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))  # reference/languages/core-c -> repo
sys.path.insert(0, os.path.join(_REPO, 'reference', 'tools'))
import fss_compile as f                                    # noqa: E402

TLM_SHAPE = os.path.join(_REPO, 'known_carriers', 'tlm', 'transaction.jsonnet')


def main():
    if len(sys.argv) != 2:
        print("usage: test_meta_hash_equality.py <fss_c_meta_hash_test>", file=sys.stderr)
        return 2
    c_bin = sys.argv[1]

    _name, entries, _cbor, cid, _rt = f.compile_shape(TLM_SHAPE)
    py_live = cid                                          # id of the complete (live) meta
    py_flat = f.flat_id(entries)                           # TypeId dropped, re-hashed

    out = subprocess.run([c_bin], capture_output=True, text=True).stdout
    got = {}
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0] in ('id', 'compiler_id', 'flat'):
            got[parts[0]] = int(parts[1], 16)

    fails = 0
    def check(label, c_val, py_val):
        nonlocal fails
        if c_val is None:
            print(f"  FAIL {label}: C produced no value"); fails += 1
        elif c_val != py_val:
            print(f"  FAIL {label}: C 0x{c_val:016x} != python 0x{py_val:016x}"); fails += 1
        else:
            print(f"  ok   {label}: 0x{c_val:016x}")

    check("runtime hash == compiler id", got.get('id'),          py_live)
    check("compiler_id matches python",  got.get('compiler_id'), py_live)
    check("flatten flat id == compiler", got.get('flat'),        py_flat)

    if fails == 0:
        print("[OK] runtime hash + flatten agree with the compiler (bit-for-bit)")
        return 0
    print(f"[FAIL] {fails} mismatch(es)")
    return 1


if __name__ == '__main__':
    sys.exit(main())
