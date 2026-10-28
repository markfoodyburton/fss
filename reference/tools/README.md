# tools/

Standalone tooling for the FSS reference repo.

## fss_compile.py

Compile a carrier-shape `.jsonnet` to its canonical wire-format meta
and compute the carrier id.

```sh
reference/tools/fss_compile.py known_carriers/tlm/transaction.jsonnet
```

Outputs the carrier id (FNV-1a-64 of the canonical CBOR meta, with
the sub-meta exclusion rule applied per
[docs/spec/01-carrier.md](../../docs/spec/01-carrier.md) §Identity), the
atom layout summary, and a hex dump of the canonical CBOR meta bytes.

Dependencies: `jsonnet` on PATH (the tool resolves the input via
the standard Jsonnet binary).  No other external dependencies; the
canonical CBOR encoder and FNV-1a-64 are implemented in this file.

## test_fss_compile.py

Unit tests for `fss_compile.py`.  Run directly:

```sh
reference/tools/test_fss_compile.py
```

Tests:
- All three carrier shapes (TLM, config, sim-events) compile cleanly.
- IDs are reproducible.
- The sub-meta exclusion rule is correctly applied (a `core::Extensions` sub-meta's `submeta` does NOT change the id).
- Changing a core atom's offset DOES change the id.

Exit 0 on all-pass, 1 on any failure.
