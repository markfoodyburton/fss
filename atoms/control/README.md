# control:: — Topology-control atoms

The vocabulary in which an orchestrator describes work for an agent: which node
to construct, which channel to create or attach, when to arm, and when to tear
down.  The atoms compose [`control::instr`](../../protocols/control/instr.jsonnet).
The active bootstrap carrier is [`control::plan`](../../protocols/control/plan.jsonnet),
which carries a reference-specific JSON sequence of these operations.

`control::` atoms are declared, versioned, and carried exactly like any
other atom.  What sets the protocol apart is its **role, not its form**:
every other carrier is *carried by* the fabric, whereas `control::instr`
is *used by* it, to wire itself into being — a topology transport must
understand this one carrier to function.  It is therefore foundational,
though mechanically an ordinary described carrier.

## Catalogue

| Atom                    | Version | wire           | byte_size | mutable_by | read_by   | phase    | Description                                                                                                    |
|-------------------------|---------|----------------|-----------|------------|-----------|----------|----------------------------------------------------------------------------------------------------------------|
| [`control::OpCode`](OpCode.libsonnet)     | 1     | `uint .size 4` | 4        | initiator  | target    | request  | Individual-step operation. `Ping=0`, `Construct=1`, `Server=2`, `Client=3`, `Arm=4`, `Bind=5`, `Stop=6`, `Done=9`. |
| [`control::Status`](Status.libsonnet)     | 1     | `uint .size 4` | 4        | target     | initiator | response | Result of interpreting the instruction, written by the agent.  `Ok=0`; non-zero = op-specific failure.         |
| [`control::Roundtrip`](Roundtrip.libsonnet) | 1   | `uint .size 4` | 4        | initiator  | target    | request  | Server/Client: 1 when the data channel needs synchronous write-back; 0 for one-way. |
| [`control::Flags`](Flags.libsonnet)       | 1     | `uint .size 4` | 4        | initiator  | target    | request  | Per-op bitmask: `COUNT=1` wraps the delivery sink with the frame oracle; `OPTIONAL=2` skips an absent client endpoint. |
| [`control::Node`](Node.libsonnet)         | 1     | `tstr`         | variable  | initiator  | target    | request  | Target node or `node/endpoint` for the individual operation. |
| [`control::Locator`](Locator.libsonnet)   | 1     | `tstr`         | variable  | initiator  | target    | request  | Model name (Construct), channel name (Server/Client), or peer `node/endpoint` (Bind). |
| [`control::PeerEp`](PeerEp.libsonnet)     | 1     | `tstr`         | variable  | initiator  | target    | request  | Client-side peer endpoint detail used by the reference plan. |
| [`control::Transport`](Transport.libsonnet) | 1   | `tstr`         | variable  | initiator  | target    | request  | Server/Client: named data-edge plugin (`fss_transport_name()`), resolved and loaded lazily by the agent. |

Per-field mutability, role, and phase are intrinsic to each atom.
`control::Status` is `mutable_by:'target'`, so the composition is a
round-trip carrier (the return path is derived, not configured).

## Notes

- **The operation set is shared vocabulary; the plan is not yet a topology
  standard.** `control::OpCode` defines individual reference-control steps.
  `control::plan` carries their current JSON representation; its field names,
  sequencing, and limits are non-normative.
- **`control::Node`/`Locator`/`PeerEp`/`Transport` are inline `tstr` in v0.1**
  (fixed byte_size slots in the composition); the strings the reference
  runtime passes (node names, DLL paths, channel names, transport names) fit
  those slots.
- **The transport is selected by NAME, not a flag bit.**  `control::Transport`
  carries `fss_transport_name()` (`"shm"`, `"zenoh"`, …); the agent resolves it
  to `libfss_<name>.<ext>` on its transport-plugin search dir, loads it lazily on
  first use, and confirms the loaded plugin by matching `fss_transport_name()`.
  Adding a transport is a new plugin on the path, no agent recompile — the coarse
  shm-vs-zenoh flag of earlier drafts is gone.
