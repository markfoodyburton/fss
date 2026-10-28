// Control protocol — the PULLED PLAN carrier (agent initiator -> orchestrator
// responder).  Where control::instr (instr.jsonnet) was one pushed step, the
// plan carrier holds an agent's WHOLE plan as a JSON array of step objects,
// pulled in ONE round-trip: the agent sends an empty request, the responder
// writes the plan into PlanJson + Status=Ok, and the round-trip returns it.
// Round-trip (control::Status is mutable_by:'target').  The step VOCABULARY is
// the orchestrator's affair (NON-NORMATIVE); the carrier is standard.

local control = import '../../atoms/control.libsonnet';
local core    = import '../../atoms/core.libsonnet';

{
  name: 'control::plan',
  atoms: [
    { atom: control.PlanJson, byte_offset: 0,    byte_size: 3072 },
    { atom: control.Status,   byte_offset: 3072 },
    { atom: core.Extensions },
  ],
}
