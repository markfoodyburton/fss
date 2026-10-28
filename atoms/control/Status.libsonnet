{
  qname:            'control::Status',
  version:          1,
  wire:             'uint .size 4',
  description:      'Result of interpreting the instruction, written by the agent (target).  0 = ok; non-zero = the op-specific failure code.',
  default_encoding: { Ok: 0 },
  mutable_by:       'target',
  read_by:          'initiator',
  phase:            'response',
}
