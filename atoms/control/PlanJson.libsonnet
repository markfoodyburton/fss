{
  qname:       'control::PlanJson',
  version:     1,
  wire:        'tstr',
  description: "The agent's full plan: a JSON array of step objects the agent runs in order (construct/server/client/bind).  PULLED: the agent (initiator) calls back with an empty request; the orchestrator's stateful responder (target) writes the plan into this slot in the round-trip reply, which the agent reads and runs.",
  mutable_by:  'target',
  read_by:     'initiator',
  phase:       'response',
}
