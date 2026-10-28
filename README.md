# FSS — Federated Simulation Standard

A minimal mechanism for inter-simulator communication: a self-describing
data **carrier**, a flat-layout profile over CDDL for typing fields, and
a registry of named semantic **atoms** that compose into protocols.

## Why FSS exists

Every simulator eventually needs to talk to another one — SystemC to
QEMU, an FMU to Simulink, a Python model to a Rust device. Today each
pairing is a **bespoke bridge** that re-invents lifecycle, time,
configuration, and bus traffic. N tools means up to N×M bridges, and
none of them compose.

FSS replaces that with **one thing every tool speaks: a carrier** — a
small, self-describing data envelope. A tool that emits and consumes
carriers talks to *every* other tool that does, over *any* transport
(in-process, shared memory, or network), with no bridge in between.

```
   Without FSS                          With FSS
   N×M bespoke bridges                  each tool speaks "carrier" once

   SystemC ─── QEMU                     SystemC ┐
      │  ╲   ╱  │                       QEMU    ┤
      │   ╳     │                       FMU     ├──▶  carrier  ◀── the only
      │  ╱   ╲  │                       Python  ┤   (self-describing   shared
   FMU ─────── Simulink                 Simulink┘    data envelope)    contract
```

**The core idea — bring your own data.** You describe your data layout
once (in CDDL); FSS turns that description into a carrier whose `id` is a
hash of the description. Both ends build the same description
independently and get the same `id`, so a receiver dispatches with a
single integer compare — no shared registry, no negotiation, no
code-generation step on the hot path. Your data stays a plain flat
struct; FSS only standardises how it is *described and carried*.

That is the whole standard. Everything else — atoms, protocols,
bindings, transports — is convention layered on that one mechanism.

## Status

Working draft (v0.2.0).  The carrier mechanism, field-type profile, and
atom registry are stable.  Specific values noted as **WORKING CANDIDATE**
in the spec (notably the hash function `FNV-1a-64`) are subject to
change before standardisation.

## Layers

The repo is organised in three tiers.  See
[`docs/design/repo-vs-impl.md`](docs/design/repo-vs-impl.md) for detail.

| Tier | What | Directories |
|------|------|-------------|
| **1 — Fundamental interoperability layer (normative)** | Carrier struct, canonical-CBOR meta, CDDL flat-layout profile, atom definition shape, identity computation, identify-before-consume rule, conformance MUSTs 1–7. C ABIs. | `docs/spec/`, `abi/` |
| **2 — Standard vocabulary (FSS Working Group; intended normative)** | The concrete atom set, protocol compositions, known-carrier packagings, topology language. Versioned and curated by the FSS WG. | `atoms/`, `protocols/`, `known_carriers/`, `topology/` |
| **3 — Reference implementation** | Tools, language bindings, transports, runtime/orchestrator, examples. | `reference/` |

## Layout

```
docs/
├── spec/              ★ NORMATIVE — the FSS specification (7 documents)
└── design/            design notes (open issues, exploration)

abi/                   ★ NORMATIVE — reference C ABI (carrier, endpoint, transport, artefact)

atoms/                 atom registry (core::, bus::, config::, sim::, time::, control::) — shared by all

protocols/             FSS-owned protocols (carrier shape + behavioural spec)
├── config/            parameter configuration (Jsonnet only)
├── sim-events/        simulation lifecycle (Jsonnet only)
├── time-sync/         time-window synchronisation (Jsonnet only)
└── control/           orchestration control (Jsonnet only)

known_carriers/        externally-defined protocols (FSS packages the carrier shape)
└── tlm/               TLM read/write carrier (IEEE 1666.1 §6; Jsonnet + C binding)

topology/              topology language (Tier-2 WG artefact; under development)

reference/             non-normative reference implementations
├── languages/         per-language FSS runtime bindings, all under reference/languages/:
│   ├── core-c/        carrier-agnostic meta resolver + wire serialiser + hash (plain C)
│   ├── systemc-tlm/   artefact wrapper + TLM carrier translation
│   ├── rust/          #[repr(C)] ABI mirror + fss_export_artefact! macro
│   ├── python-loader/ C cdylib embedding CPython so a .py model is a dlopen'd artefact
│   └── fmi/           FMI-3.0 co-simulation importer (FMI-LS-BUS FMU → FSS artefact)
├── transports/        per-transport implementations
│   ├── inproc/        in-process transport
│   ├── shm/           shared-memory transport
│   └── zenoh/         network / cross-host transport
├── runtime/           launcher / orchestrator / agents
├── examples/          clean models (CAN targets: fmi/sc/python/rust + SystemC TLM) + FSS drivers
└── tools/             standalone Python tooling (fss_compile.py)
```

## How to read

- **The standard itself**: `docs/spec/README.md` then `00-object-model.md` through `06-conformance.md`.
- **Atoms and protocols**: `atoms/README.md`, the per-protocol READMEs in `protocols/`, and the carrier READMEs in `known_carriers/`.
- **Something that actually runs**: `reference/examples/README.md`.
- **Architectural picture**: `docs/design/`.

## Quick start

```sh
cmake -B build -DFSS_FETCH_SYSTEMC=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

This builds the `fss_spi_memory_systemc.dylib` artefact and the
`fss_load_probe` host program; the `fss_spi_memory_systemc_load` ctest
exercises a full round-trip TLM transaction through the in-process
transport.

## Decisions

| Question                           | Answer                                            |
|------------------------------------|---------------------------------------------------|
| Field-type language                | **CDDL** (RFC 8610) — a closed flat-layout profile; see `docs/spec/02-encodings.md`. |
| Registry / composition language    | **Jsonnet** — atoms and protocol compositions are Jsonnet records carrying CDDL `wire` types. |
| Wire format (meta)                 | **CBOR** (RFC 8949 §4.2 deterministic).           |
| Hash function (working candidate)  | FNV-1a-64.                                        |
| Reserved namespace                 | `core::` (atoms only; allocates `core::Extensions`). |
| Privileged transport or language   | None.  C is the reference ABI; transports are bindings. |
| Dynamic extensibility              | First-class, via the `core::Extensions` sub-meta (extensions add meta + data to the one carrier). |

The standard fits in a short reading: seven markdown documents under
`docs/spec/`.  That is on purpose.
