# core:: — FSS-internal atoms

The `core::` namespace is reserved for atoms defined by the FSS
standard itself.  It contains two atoms: `core::Extensions`, the
canonical mechanism for hops along an FSS path to attach information to
a carrier without changing its protocol id; and `core::TypeId`, the
native-object marker that enables the in-process zero-copy fast path.

## Catalogue

| Atom              | Version | wire          | byte_size                        | mutable_by | read_by | phase | Description                                                                                        |
|-------------------|---------|---------------|----------------------------------|------------|---------|-------|----------------------------------------------------------------------------------------------------|
| [`core::Extensions`](Extensions.libsonnet) | 1       | — (sub-meta)  | none (sub-meta)                  | either     | both    | both  | Extension hook; a sub-meta describing further bytes in the one data region (carries no data bytes of its own; excluded from the id hash). |
| [`core::TypeId`](TypeId.libsonnet) | 1       | `uint .size 8` | 8 (vtable ptr)                  | neither    | both    | both  | Native-object marker: points at the C++ vtable pointer (offset 0 of a polymorphic object).  Presence ⇒ `data_ptr` is a live in-memory C++ object whose real type is reachable via RTTI.  `non_serializable` (a transport drops it); `in_hash: true` (live and flat forms get distinct ids).  Enables the in-process zero-copy fast path. |

## Semantics

`core::Extensions` uses the `sub-meta` layout: a nested meta whose
entries describe further bytes in the carrier's single data region;
it has no data bytes of its own.  A hop grows the data region, writes
its bytes, and adds a sub-meta describing them; the sub-meta typically
nests the existing extension chain (a further `core::Extensions`
pointing at what came from upstream).

Dispatch is on the outer carrier's protocol id, which is computed
from the core atoms only.  `core::Extensions` content does not affect
the id.  See
[docs/spec/04-protocols.md](../../docs/spec/04-protocols.md)
§Extensions for the full recursive-wrapping semantics and sealed
protocol rules.

`core::TypeId` points at the vtable pointer of a live polymorphic C++
object; its **presence** (not its value) marks the carrier as the *live*
form — `data_ptr` is a real object, so an in-process peer that recognises
the carrier id MAY cast `data_ptr` directly to the live object (the
zero-copy fast path) instead of walking the meta.  A protocol does **not**
need `core::TypeId`; without it a carrier is purely flat bytes.  Because it
is `in_hash: true`, the live carrier id differs from the flat carrier id; a
wire transport **drops** the `non_serializable` `core::TypeId` atom and
re-hashes, so a serialised carrier never carries the live id.  See
[docs/spec/02-encodings.md](../../docs/spec/02-encodings.md)
§core::TypeId.

## Future atoms

Other FSS-internal atoms (e.g., lifecycle markers, standard error
payload) will be added to this namespace when the spec defines them.
