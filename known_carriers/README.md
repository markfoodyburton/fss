# known_carriers/

Carrier shapes for **externally-defined** protocols where FSS packages
the carrier without owning the protocol design itself.

## How this differs from `protocols/`

| | `known_carriers/`                | `protocols/`                  |
|---|---|---|
| Protocol design | external (IEEE, OSCI, vendor)   | FSS                                 |
| Carrier shape   | FSS expresses                    | FSS owns                            |
| Behaviour spec  | external reference               | in this repo                        |

Examples of what would live here: TLM (IEEE 1666.1), CAN, AXI, AHB,
custom on-chip buses with established semantics.  FSS provides the
atom composition + magic id; the protocol's read/write semantics are
defined elsewhere.

## Carriers

| Directory | External reference                    |
|-----------|---------------------------------------|
| `tlm/`    | IEEE 1666.1 §6 (TLM-2.0 base protocol)|

## Layout

Each carrier under `known_carriers/<name>/` mirrors the layout used
by `protocols/`:

- `README.md` — what the carrier represents; pointer to the external spec.
- `<carrier>.jsonnet` — the atom composition (e.g. `transaction.jsonnet`).
- `CMakeLists.txt` — generates the id/meta header (`fss-<name>-id-gen`) and
  exposes the INTERFACE library consumers link.

There is no hand-written C accessor: a carrier is language-neutral.
Consumers either recognise the carrier id and use native accessors (for
TLM, the live `tlm_generic_payload`), or resolve fields by qname from the
carrier meta at runtime (the generic resolver in `reference/languages/core-c/`).

The atom registry is shared with `protocols/` — see [`atoms/`](../atoms/).
