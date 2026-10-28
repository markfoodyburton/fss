{
  qname:            'sim::EventCode',
  version:          1,
  wire:             'uint .size 4',
  description:      'Simulation lifecycle phase transition.  Single-carrier request/response semantics.  Time advance is a SEPARATE protocol (a dedicated time-window carrier), not a lifecycle event.',
  default_encoding: { Bound: 0, Elaborated: 1, Started: 2, Paused: 3, Resumed: 4, Stopped: 5 },
  mutable_by:       'initiator',
  read_by:          'target',
  phase:            'request',
}
