# Open issues

Honest list of what the current design does not yet settle.  Scoped to
the carrier / atom / CDDL-profile / sub-meta model in `docs/spec/`.
Resolved or superseded issues are not kept here.

## 1. Hash function

`id` is a 64-bit hash of the canonical CBOR meta.  **FNV-1a-64** is the
working candidate (`docs/spec/01-carrier.md` §Identity).  It is fast and
trivial to implement in any language, but is not collision-resistant.
Open: keep FNV-1a-64, or pick a stronger 64-bit function before freeze?
The id is content-addressing, not security — collisions degrade dispatch,
they don't breach trust — so FNV-1a-64 is probably adequate.  Decide and
fix it; every conforming implementation must agree.

## 3. Sub-meta over the wire

In-process, an extension grows the one data region (realloc) and adds a
sub-meta whose entries point into it (`docs/spec/04-protocols.md`
§Extensions).  Across a process boundary the whole thing must serialise:
the grown data region plus the nested sub-meta chain, reconstructed at
the receiver.  The in-process meta is already CBOR, so the meta side is
solved; the open part is the framing that carries data-region + meta
together on a wire transport, paid once per cross-process hop.  Needs a
concrete wire format.

## 4. `fss:local-ptr` inlining mechanics

A wire transport MUST inline a `fss:local-ptr` field's pointed-to bytes,
drop the layout, and set `byte_size` to the inlined length
(`docs/spec/02-encodings.md`).  The length comes from the companion atom
named by `attrs.length_at`.  Open: pin down the inlining algorithm
(ordering when several pointer fields share a length atom; what happens
to the now-unused length field) so two transports inline identically and
produce the same wire-form id.

## 5. CDDL profile boundaries

The flat-layout profile is deliberately small: fixed-width scalars,
`bstr`, `tstr`, plus the FSS `layout` constructs.  No flags/bitfields
(`.bits`), no in-line arrays or nested records.  Open: which, if any, of
these are worth adding?  Each addition is a new byte-mapping rule FSS
must define (CDDL types alone don't fix layout) and every implementation
must support.  Default answer: stay minimal; add only on a real need.

## 6. Sub-meta recursion depth

A sub-meta can nest sub-metas without bound; a malformed carrier could
recurse arbitrarily.  Receivers need a depth limit.  Open: spec-mandated
default (e.g. 16) or per-binding?

## 7. Simulation-lifecycle protocol

`protocols/sim-events/` is a single-carrier skeleton (one phase-transition
or time-advance per round-trip).  The real protocol — phase choreography
(Bound→Elaborated→…→Stopped), time-grant negotiation, asynchronous
responses, a mid-elaboration host-binding window — is unspecified.
The bind/lifecycle design is settled at the single-carrier level
(see `docs/design/binding-and-phases.md`); multi-message choreography
is the remaining open block.  Ties into needing a psystemc
mid-elaboration pause.

The **simulation startup readiness barrier** is now settled, as an explicit sim-status
protocol (not by probing the data plane).  Readiness flows as an UPWARD wave
along the federation chain: the sink starts first, each node announces
`sim::event{Elaborated}` ("running") to its upstream once it is running and every
downstream peer has, and the head reports the federation ready to the
orchestrator.  `fss/time` is therefore pure fire-and-forget (the earlier
first-window `FSS_SEND_OK` confirm is removed).  This composes node-to-node and
node-to-orchestrator; see `protocols/sim-events/README.md`.  A
*federation-wide* coordinator barrier for arbitrary DAGs (vs a chain) is still open.

This protocol-level barrier is separate from a transport's optional launch
readiness. The Zenoh reference plugin advertises
`FSS_TRANSPORT_CAP_REMOTE_READY`; after listener-first target setup, its
liveliness token is followed by a bounded real data-path challenge. It does
not use a fixed discovery-settle sleep. The optional plugin symbols and their
flow-specific probes are documented in `zenoh-transport.md`.

KNOWN LIMITATION (pre-existing, surfaced under load): a synchronous cross-kernel
`b_transport` into a node that is *time-suspended* at its bound cannot be
serviced — the suspended kernel wakes on the doorbell but re-suspends before its
dispatch thread runs.  If the caller must itself advance to release that suspend,
the two deadlock.  Rare; provoked only by CPU oversubscription (`-j` parallel
test runs), so the 576-cell `agent_matrix` sweep uses
`ctest -L agent_matrix --repeat until-pass:3`. The real
fix is the passive/zero-handoff `b_transport` path (a foreign thread entering the
kernel safely under lock) — i.e. psystemc.

## 8. Endpoint naming and discovery

The current ABI has named opaque channels and endpoint `attach` roles; it no
longer uses `transport->open` or locator-based `bind`. A channel is directional
(initiator to target, with optional write-back), so peer-initiated asynchronous
traffic needs a separate reverse channel. Broader discovery — peer-to-peer
federations, late joiners, and no central orchestrator — remains unaddressed.

## 9. Typed `config::ParamValue`

`config::ParamValue` is opaque `bstr` in v0.1.  Typed value-shapes
(integer / float / string / boolean) are deferred; the intent is to
describe them with a sub-meta, not a separate carrier.  Depends on the
config protocol's broader design.

## 10. Codegen — deferred, not planned

A code generator for typed *accessors* (Jsonnet/CDDL → C/Rust/Python get/set)
was prototyped (`gen-typed-c`, since removed; recoverable from git history)
but is not part of the design.  Reasoning: transports never read fields;
federated models already have native structures; a new-model author writing
against a known carrier hand-writes a handful of offset accessors more cheaply
than adopting a codegen toolchain.

We DO generate the **drift-prone constants** — `fss_compile.py --emit-c-header`
emits a carrier's `FSS_<X>_ID` + canonical `FSS_<X>_META[]` (build-time, e.g.
`fss/known_carriers/tlm_id.h`, `fss/protocols/sim.h`).  Those can't be hand-
maintained sensibly and must track the composition exactly; the hand-written
*accessors* (offsets) stay hand-written.  Offset drift is guarded by
the `c_meta_resolver` test (Path B resolved from the generated meta must equal
the hand-written Path-A offsets).

The generic, protocol-independent **meta resolver** (`qname → offset`, cached
by `(id, qname)`) EXISTS at `reference/languages/core-c/include/fss/c/meta.h` +
`reference/languages/core-c/meta.c`; its first consumer is the SystemC receiver's Path B.
Rust/Python get their own when a consumer in those languages needs Path B.

Revisit accessor codegen only if many-protocols × many-languages makes hand-
written accessors a real maintenance cost.

## 11. Authentication / authorisation / trust

Not addressed.  Each transport handles its own.  The spec should probably
require every binding to document its trust model.  Out of scope for the
mechanism; critical for any real deployment.

## 12. Governance

Which body stewards the standard (Modelica Association, Eclipse, an
independent project)?  Out of scope for the spec text, but blocks any
real release.
