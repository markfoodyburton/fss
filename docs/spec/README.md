# FSS Specification

FSS — Federated Simulation Standard — defines a minimal mechanism for
inter-simulator communication: a carrier (the data envelope), a
flat-layout profile over CDDL for typing fields, an atom-definition
mechanism for naming reusable semantic field shapes, and conventions
for composing atoms into protocols.  This document set is the
normative spec.

## Layers

The FSS repository is organised in three tiers.  See
[`docs/design/repo-vs-impl.md`](../design/repo-vs-impl.md) for detail.

| Tier | What | Directories |
|------|------|-------------|
| **1 — Fundamental interoperability layer (normative)** | The carrier mechanism: carrier struct, canonical-CBOR meta, closed CDDL flat-layout profile, atom definition shape, identity computation, the identify-before-consume rule, conformance MUSTs 1–7. The C ABIs. | `docs/spec/`, `abi/` |
| **2 — Reference vocabulary and examples** | Concrete atoms, protocol compositions, known-carrier packagings, and topology examples. They evolve independently of the Tier-1 mechanism. | `atoms/`, `protocols/`, `known_carriers/`, `topology/` |
| **3 — Reference implementation** | Tools, language bindings, transports, runtime/orchestrator, examples. Implements the standard; other implementations may differ and still interoperate at the carrier level. | `reference/` |

The data layer has two halves.  **Logical** — atoms (`atoms/`, see
[03-atoms.md](03-atoms.md)) name reusable semantic *roles* (a locator,
an operation) independent of bytes.  **Physical** — carrier
compositions (`known_carriers/`, `protocols/`, see
[04-protocols.md](04-protocols.md)) pin those roles to concrete byte
offsets, widths, and enum encodings.  One logical role can appear in
many physical layouts, which is what lets different protocols share a
vocabulary and be bridged.

## FSS in two minutes

Simulators need to talk to each other, and today every pairing is a
custom bridge. FSS removes the bridges by giving every tool **one shared
thing to speak: a carrier.**

A **carrier** is a small header (`fss_carrier_t`) wrapping a flat data
region plus a CBOR **meta** that describes that region's fields. You
describe your data's layout once; the carrier's `id` is a hash of that
description, so both ends build the same description independently and
get the same `id` — dispatch is a single integer compare, with no shared
registry and no negotiation. Your data stays a plain flat struct.

An **endpoint** is a named carrier sink/source owned by an **artefact**
(a loadable simulation component). A **transport** — in-process, shared
memory, or zenoh — finds or creates a named **channel**, then `attach`es each
endpoint to it as an initiator or target. An initiator attach installs a direct
`send` callback, after which the transport is off the hot path. An
**orchestrator** coordinates artefacts, which construct and own their own
**nodes**; it creates channels and attaches those nodes into a topology, then
drives lifecycle and configuration as ordinary carriers over the reserved
`fss/` control ports.

That is the whole mechanism: typed data envelopes flowing between named
endpoints over interchangeable transports. Everything else — atoms,
protocols, bindings — is how carriers are *described and composed*, not
new machinery.

## Sections

| File                                           | Subject                                                              |
|------------------------------------------------|----------------------------------------------------------------------|
| [00-object-model.md](00-object-model.md)       | Object kinds: carrier, endpoint, transport, artefact, model, binding. |
| [01-carrier.md](01-carrier.md)                 | Carrier struct, meta format, id computation.                         |
| [02-encodings.md](02-encodings.md)             | Field types: the CDDL flat-layout profile.                           |
| [03-atoms.md](03-atoms.md)                     | Formal atom structure and naming convention.                         |
| [04-protocols.md](04-protocols.md)             | Atom composition, magic ids, behavioural specification.              |
| [05-abi-conventions.md](05-abi-conventions.md) | Reference shapes for the transport ABI and artefact ABI (informative). |
| [06-conformance.md](06-conformance.md)         | What conforming implementations must do.                             |

## Reading order

00 introduces the object kinds.  01 → 02 → 03 → 04 establishes the
data layer end-to-end.  05 and 06 are about implementation conventions
and conformance.  Each section is self-contained; there are no forward
dependencies on later docs.

## Status

Working draft.  Specific values noted as **WORKING CANDIDATE** (e.g.
the hash function `FNV-1a-64`) are subject to change before the
standard is finalised.
