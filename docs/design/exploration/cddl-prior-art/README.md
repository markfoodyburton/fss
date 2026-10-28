# Example interface libraries

These CDDL packages are **NOT** part of the FSS standard.  The
standard allocates **no** descriptors at all — not identity, not
time, not status, not lifecycle, nothing.  These files exist purely
as worked examples of how community packages plug into the FSS bag
mechanism.

In a real deployment, packages like these would live in separate
git repositories — one or more community-maintained "interface
library" repos — and be published independently of the FSS standard
itself.

## Files

- `fss-basics.cddl` — universally-useful descriptors that recur
  across most deployments: identity, time, time-window, status,
  error.  Other community packages typically depend on this.
- `fss-tlm-fields.cddl` — TLM bus field descriptors (address, size,
  direction, payload, response).
- `fss-lifecycle.cddl` — lifecycle qnames (initialise, start, stop,
  finalise, result).
- `fss-discovery.cddl` — discovery / manifest descriptors.
- `fss-orchestration.cddl` — top-down orchestration vocabulary
  (spawn, parent + config-source bootstrap, connect-port, ack).
  See `../../binding-and-phases.md` and the "node and orchestrator"
  roles in `../../../spec/00-object-model.md` for the architectural
  walkthrough.
- `fss-kv.cddl` — generic key-value primitive (get / value / set /
  list / watch).  Used for configuration, inspection, control
  surfaces — anywhere a name-keyed lookup is wanted.
- `fss-axi-extra.cddl` — AXI-specific extension fields, demonstrating
  how a vendor adds attributes orthogonally.

All of these use the `fss-community:` namespace.  The `fss:`
namespace is reserved by the standard for possible future
allocation, but v0.1 puts nothing in it.
