# can/ — classic CAN frame carrier shape

Classic CAN frames over FSS.  Carrier shape for the FMI Layered Standard
for Bus Communication (FMI-LS-BUS) `CanTransmit` operation (op 0x0010);
FSS packages the carrier without owning the protocol design.

## Canonical definition

The canonical carrier definition is [`frame.jsonnet`](frame.jsonnet) plus the
field table below.  There is no hand-written C accessor: in SystemC the
carrier id is recognised natively; other consumers resolve fields by qname
from the carrier meta at runtime.  `CMakeLists.txt` generates only the id +
CBOR meta (`fss/known_carriers/can_id.h`) from the composition.

## Atom composition

The id, operation, length, and data fields are shared `bus::` roles.  The
only CAN-specific field is the identifier-extension flag (`can::Ide`); it is
not reusable vocabulary, so it is defined **inline** in `frame.jsonnet`
(cf. `tlm::GpOption`) rather than as a registry atom — CAN adds no shared
vocabulary.  `bus::Operation` uses an `encoding` override so that `rtr` maps
as `Write=0` (data frame) / `Read=1` (remote frame), faithful to the
FMI-LS-BUS byte value.

| Offset | Size | Atom                          | Notes |
|--------|------|-------------------------------|-------|
| 0      | 4    | `bus::Locator`                | CAN message identifier |
| 4      | 1    | `can::Ide` (inline)           | Standard=0 (11-bit), Extended=1 (29-bit) |
| 5      | 1    | `bus::Operation`              | enc:{Write=0, Read=1} (rtr: data=Write, remote=Read) |
| 6      | 2    | `bus::Length`                 | valid byte count in bus::Data (0..8) |
| 8      | 8    | `bus::Data` (inline; 8 bytes) | classic CAN ≤8 bytes; bus::Length bytes valid |
| —      | —    | `core::Extensions` (sub-meta; no data bytes) | |

Total core data region: 16 bytes.

Per-field mutability, role, and phase are intrinsic to each atom; the shared
roles live in [`atoms/bus/`](../../atoms/bus/) (the one CAN-specific field is
inline in `frame.jsonnet`).

`bus::Data` is inline (not a local-ptr) because classic CAN payload is small
and fixed (≤8 bytes).

`core::Extensions` permits hops to wrap the carrier without changing the
carrier id; CAN-FD (`brs`/`esi`) and CAN-XL extensions ride as
`core::Extensions` (future).

The carrier id is the FNV-1a-64 of the canonical CBOR meta of this
composition, with the sub-meta exclusion rule applied
(see [docs/spec/01-carrier.md](../../docs/spec/01-carrier.md) §Identity).
