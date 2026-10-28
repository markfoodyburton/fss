# bus:: — Transactional-interaction atoms

Atom family for the transactional pattern: **locator + operation +
payload + result**.  A request names a target, says what to do, may
carry data; the target acts and returns a status.

This is the general shape, not a single domain.  A memory-mapped
read/write (TLM, AXI, AHB) is the canonical instantiation — `Locator`
locates, `Operation` says Read/Write/Ignore, `Data`/`Length` carry the
payload, `Status` returns the result.  The same atoms suit any addressed
load/store-shaped exchange — a CAN frame reuses `Locator` (message id),
`Operation`, `Length`, and `Data` with different widths and an encoding
override.  Other transactional protocols (named-key config, key-value
stores) share the pattern but supply their own operation and status
vocabularies; see those namespaces.

These are **shared-role** atoms: the qname names the role and meaning;
the wire width (`byte_size`) and enum encoding are deferred to each
carrier composition (see [docs/spec/02-encodings.md](../../docs/spec/02-encodings.md)
§Deferred enum encoding).  `uint`/`int` without `.size` are
width-agnostic — the composition fixes the width.

## Catalogue

| Atom                          | Version | wire           | default_encoding / notes                                                                 | mutable_by | read_by   | phase    |
|-------------------------------|---------|----------------|------------------------------------------------------------------------------------------|------------|-----------|----------|
| [`bus::Locator`](Locator.libsonnet)               | 1 | `uint`         | Target locator — memory address (TLM) or bus message id (CAN).  Width per-carrier.       | initiator  | target    | request  |
| [`bus::Operation`](Operation.libsonnet)           | 1 | `uint`         | `Read=0, Write=1, Ignore=2` (default).  A composition MAY override (CAN: `Write=0,Read=1`). | initiator  | target    | request  |
| [`bus::Length`](Length.libsonnet)                 | 1 | `uint`         | Number of bytes in `bus::Data`.  Width per-carrier.                                       | initiator  | target    | request  |
| [`bus::Data`](Data.libsonnet)                     | 1 | `bstr`         | Payload bytes; size given by `bus::Length`.  In-process MAY override layout to `fss:local-ptr`. | either     | both      | both     |
| [`bus::Status`](Status.libsonnet)                 | 1 | `int`          | TLM-2.0 `tlm_response_status`: `Ok=1, Incomplete=0, GenericError=-1 … ByteEnableError=-5`. | target     | initiator | response |
| [`bus::StreamingWidth`](StreamingWidth.libsonnet) | 1 | `uint .size 4` | TLM-2.0 streaming width; bytes after which the address wraps.                            | initiator  | target    | request  |
| [`bus::ByteEnable`](ByteEnable.libsonnet)         | 1 | `bstr`         | Byte-enable mask, one byte per data byte; length in `bus::ByteEnableLength`.             | initiator  | target    | request  |
| [`bus::ByteEnableLength`](ByteEnableLength.libsonnet) | 1 | `uint .size 4` | Number of bytes in `bus::ByteEnable`; 0 means no mask.                                   | initiator  | target    | request  |
| [`bus::DmiAllowed`](DmiAllowed.libsonnet)         | 1 | `bool`         | Target grants direct-memory-interface access; degrades to false off-machine.            | target     | initiator | response |

## Semantics

- `bus::Operation` Read (`= 0` by default): target fetches `bus::Length`
  bytes starting at `bus::Locator` and writes them into `bus::Data`.
- `bus::Operation` Write (`= 1` by default): target stores the bytes from
  `bus::Data` at `bus::Locator`.
- `bus::Operation` Ignore (`= 2`): address-phase/no-op.
- The wire value for an operation is the carrier's *effective* encoding
  (`default_encoding`, or a composition `encoding:` override), recorded in
  the meta — so a CAN frame mapping `rtr` to `Write=0,Read=1` stays
  bridgeable via the shared meaning names.
- Atomicity: at the atom level, atoms make no atomicity guarantee.
  Specific protocols (e.g. TLM) define atomicity over a full transaction.
- Endianness: all multi-byte scalar atoms are little-endian on the wire.

## Inline (not registry) fields

A field that is genuinely protocol-specific is defined **inline** in its
carrier composition, not added here — e.g. `tlm::GpOption` and `can::Ide`
(see [docs/spec/03-atoms.md](../../docs/spec/03-atoms.md) §Sharing
principle).

## Future atoms

Anticipated additions, defined when concrete consumers require them:

- `bus::Burst` — burst-length and burst-type for AXI-style protocols.
- `bus::Snoop` — coherency hints.

The right boundary between shared `bus::` atoms and protocol-specific
vocabularies will firm up as more protocols are composed — specialise
when a real protocol needs it, not before.
