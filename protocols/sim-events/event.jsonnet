// Sim-events protocol — a single lifecycle phase-transition request.
//
// Spec:           docs/spec/04-protocols.md
// Atom registry:  atoms/sim/
//
// v0.1 specifies only individual phase-transition carriers (one round-trip per
// event).  Multi-message phase choreography is design-pending — see this
// protocol's README.  TIME ADVANCE is a SEPARATE protocol (a dedicated
// time-window carrier), no longer carried here.

local sim = import '../../atoms/sim.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'sim::event',
  atoms: [
    // Lifecycle phase event code.
    { atom: sim.EventCode,   byte_offset: 0 },

    // Response code, written by the target.
    { atom: sim.Status,      byte_offset: 4 },

    // Extension hook (sub-meta): no data-region bytes.
    { atom: core.Extensions },
  ],
}
