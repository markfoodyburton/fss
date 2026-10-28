# Native-construct interop (Stage 2 design)

> ## SUPERSEDED — CORRECTED 2026-07-19 (read this first)
>
> **This document's central premise was wrong, and the design below (D-A "each
> framework emits a different native carrier + per-hop bridges", D-B "first-class
> `can::*` vocabulary") is NOT the FSS design. Do not implement it.** The whole
> point of FSS is **atom reuse**: a CAN identifier and a TLM address are the same
> *role* — a locator — so they are the **same atom** (`bus::Locator`), possibly at
> different `byte_size`. A consumer resolves a field by searching the CBOR meta
> for the atom's qname and reads it at whatever size the meta gives. Because the
> vocabulary is **shared**, a SystemC / FMI / Rust / Python component consuming a
> CAN frame needs **no bridge and no per-language translation** — the shared atom
> *is* the interop, and the 576/576 matrix is already green *because* of it.
>
> What actually came out of this:
> - The atom category was mis-named `txn::`, which read as "a TLM/SystemC thing"
>   and fooled me into thinking CAN needed a parallel vocabulary. **Renamed the
>   category to `bus::`** (bus-transaction roles, shared across TLM/CAN/…). That
>   rename is the one real, constructive change; it removes the confusion at the
>   source.
> - **R7 was a wrong turn and was reverted.** `can::frame` goes back to *reusing*
>   the shared `bus::` roles; only genuinely CAN-specific flags with no
>   cross-protocol counterpart stay **inline** in the carrier (`can::Ide`; the rtr
>   bit rides `bus::Operation`). No `atoms/can/*`, no `resolve_role` alias.
> - **R8/R9/R10 are EMPTY.** Once vocabulary is shared, FMI/Rust/Python already
>   interoperate by producing carriers tagged with shared atoms and consuming by
>   meta lookup. There is nothing to bridge. What this doc called "the cheat" was
>   largely the demonstration working exactly as intended.
> - The genuinely nice paper point that survives: **hop1 (TLM) carries an 8-byte
>   `bus::Locator`, hop2 (CAN) a 4-byte `bus::Locator` — same atom, different
>   size — and every consumer reads it correctly from meta.** Reuse across sizes,
>   live, no translation. `tab:perf` does **not** move (no bridges were added).
>
> The rest of this file is retained for the record only. Everything below the
> line describing "the problem" is the mistaken framing.

Status: **SUPERSEDED (see correction above).**
Branch `fss-poc-rework`.

## The problem — the current PoC cheats

The worked example is a heterogeneous chain:

```
  init ──socket/tx (hop1)──▶ ctrl ──can_tx/can_rx (hop2)──▶ dev
       register-write source   TLM→CAN bridge      CAN device
```

Today hop1 is a `tlm::transaction` and hop2 a `can::frame`, and **all four
languages emit the identical flat TLM image on hop1**, produced as follows
(verified in the artefacts):

- **SystemC** — genuine `tlm::tlm_generic_payload` (native). ✅
- **FMI** — *harvests* a real `fmi3LsBusCanOperationCanTransmit` from the FMU
  (native input!), then `emit_tlm_write` hand-packs it into a flat 72-byte GP at
  hardcoded `off_locator/off_operation/off_length/off_data` — **flattens native
  CAN into fake TLM**.
- **Rust** — no native object; byte-pokes a `[u8; 72]` at `FSS_TLM_OFF_txn_*`
  and ships `FSS_TLM_FLAT_ID/META`.
- **Python** — no native object; `struct.pack_into` at the same offsets.

This shows "four languages agree on one carrier," but **not** the actual claim of
FSS: that *independently-modelled components interoperate from their own native
representations*. The bridge — the thing that earns FSS its keep — is sidestepped
entirely (everyone pre-agrees the TLM byte image, so there is nothing to bridge).

## What each framework NATIVELY originates

| Framework | Native construct | Natural carrier |
|-----------|------------------|-----------------|
| SystemC   | `tlm::tlm_generic_payload` (b_transport) | `tlm::transaction` |
| FMI       | FMI-LS-BUS `CanTransmit` op (`fmi3LsBusUtilCan.h`) | `can::frame` |
| Rust      | a `#[repr(C)]` typed struct | typed → `can::frame` / `tlm::transaction` |
| Python    | a `@dataclass` | typed → `can::frame` / `tlm::transaction` |

The key asymmetry: **TLM is a SystemC/C++ concept.** An FMI CAN node does not do
memory-mapped register writes; it originates a *CAN frame*. Forcing FMI/Rust/
Python to emit TLM on hop1 is exactly the cheat. Conversely a SystemC initiator
doing register writes genuinely originates TLM.

## The design

Two moves, each a decision point below.

### D-A. Each initiator emits its OWN native carrier; the controller bridges by meta.

Stop forcing hop1 to be uniformly TLM. Instead:

- **SystemC init** → `tlm::transaction` (native register write). hop1 = TLM.
- **FMI / Rust / Python init** → `can::frame` (native CAN source). hop1 = CAN.

The **controller becomes a genuine meta-driven bridge**: it inspects the inbound
carrier's id/meta and produces the device's `can::frame`, regardless of whether
the source was TLM or CAN. Because the bridge walks *meta* (atom qnames), not
hardcoded offsets, it needs no compile-time knowledge of the sender's layout —
this is the FSS mechanism actually being exercised. When the source is already
`can::frame`, the bridge is near-identity; when it is `tlm::transaction`, the
bridge maps register-write semantics → CAN semantics.

*Consequence:* hop1's carrier **varies by initiator language** (TLM for sc, CAN
for the rest). The matrix oracle must accept both hop1 carriers. This is the
honest "native-to-native" demonstration and the real test of the atom/meta bridge.

### D-B. Promote CAN fields to first-class atoms.

Today `can::frame` reuses `txn::` roles (`Locator`/`Operation`/`Length`/`Data`)
with one inline `can::Ide`, deliberately adding "no shared vocabulary." Native CAN
models think in CAN terms, so promote a first-class CAN vocabulary:

- `can::Id` (11/29-bit identifier), `can::Ide` (extension flag, already inline →
  atom), `can::Dlc` (data length code), `can::Data` (≤8-byte payload), `can::Rtr`
  (remote flag). `atoms/can/*` + `atoms/can.libsonnet`.
- Recompose `known_carriers/can/frame.jsonnet` from `can::` atoms; regenerate the
  id/meta via `reference/tools/fss_compile.py`. `FSS_CAN_*` ids in `tab:protocol-ids`/
  `tab:atoms` will shift → a deliberate, documented oracle update.

This makes TLM↔CAN a *genuine* vocabulary bridge (`txn::Locator`↔`can::Id`,
`txn::Length`↔`can::Dlc`, `txn::Data`↔`can::Data`) rather than a shared-atom
identity — more convincing, and it is why the perf numbers move (real
atom-remap per hop, not a memcpy).

### Per-hop bridges — where they run

- **At each artefact's edge (all languages):** the binding builds/reads its
  NATIVE typed object and (de)serialises to the carrier by walking the carrier
  meta — never by hardcoded offsets. This replaces `emit_tlm_write` /
  `FSS_TLM_OFF_*` / `struct.pack_into`. One `fss_carrier_from_native` /
  `fss_native_from_carrier` helper per binding.
- **At the controller (hop boundary):** the TLM/CAN → CAN bridge, meta-driven as
  in D-A. This is the only place two *different* carrier vocabularies meet.

### One artefact per language (build 1/2/3 things)

Enabled by the Stage-1 build-plan carrier (R5): the control plan lists the nodes
an agent builds. Today there are **3 `.so` per language** (init/ctrl/dev × 4 =
12). Consolidating to **1 `.so` per language** exporting a model factory that
builds any of {initiator, controller, device} by name — the agent's plan naming
1/2/3 of them — is the artefact-restructure half of Stage 2. It is orthogonal to
the carrier work (D-A/D-B) and can land either before or after; recommend after,
so the carriers are settled first.

## Impact

- **`tab:perf`** — worse and that is the point: real per-hop atom translation vs
  today's zero-bridge memcpy. Re-measure in Stage 3.
- **Paper §Results worked example** — rewrite: hop1 is native-per-language (not
  "all four emit `tlm::transaction`"); the controller is a meta-driven bridge.
  `fig:canbus` framing already shows artefacts building models + channels; update
  the carrier labels to per-language.
- **Matrix oracle** — accept both hop1 carriers (D-A) and the shifted `FSS_CAN_*`
  ids (D-B); deliberate, documented.
- **Atom catalogue** — new `atoms/can/*`; `can::frame` recomposed.

## SIGN-OFF (confirmed 2026-07-18)

1. **D-A topology — APPROVED.** hop1 carrier varies by initiator language;
   controller is a meta-driven multi-source bridge.
2. **D-B first-class CAN atoms — APPROVED.** Promote `can::*`, recompose
   `can::frame` (ids shift, deliberate oracle update).
3. **Artefact consolidation — user's discretion.** Plan: do it AFTER the carrier
   work, and only if it lands cleanly.
4. **R8–R10 order — FMI → Rust → Python.**

## Decisions to confirm (SIGN-OFF)

1. **D-A topology**: accept that hop1's carrier varies by initiator language
   (TLM for SystemC, `can::frame` for FMI/Rust/Python), with the controller as a
   meta-driven multi-source bridge? *(Recommended — it is the real test.)*
   Alternative: keep hop1 uniformly TLM and only de-cheat the *production* (native
   typed object + meta-driven serialise), leaving FMI bridging CanTransmit→TLM —
   less honest, smaller perf move.
2. **D-B first-class CAN atoms**: promote `can::Id/Ide/Dlc/Data/Rtr` and recompose
   `can::frame` (ids shift)? *(Recommended — native CAN speaks CAN; makes the
   bridge genuine.)* Alternative: keep the `txn::`-reuse minimality (shared-atom
   identity bridge, smaller perf move, but the "native" claim is weaker).
3. **Artefact consolidation** (3 `.so`→1 per language): in scope for Stage 2, and
   before or after the carrier work? *(Recommended: after.)*
4. **Execution order** of R8–R10: FMI first (the crisp native case — it already
   has `CanTransmit`), then Rust, then Python? *(Recommended.)*

## Cross-references
- The cheat: `reference/examples/fss-artefacts/fss_can_initiator_paced_{fmi/can_fmi_tx_paced.c,
  rust/src/lib.rs,python/can_initiator_paced.py}`; `known_carriers/{tlm/transaction,
  can/frame}.jsonnet`; `atoms/{txn,can}/*`; `reference/tools/fss_compile.py`.
- Prior context: `docs/design/channel-abi-rework.md` §TODO native-interop;
  `[[fss-channel-abi-rework]]`, `[[fss-consumption-model]]` (every binding walks
  the carrier meta for offsets — the mechanism this generalises).
