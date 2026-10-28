{
  qname:            'sim::Status',
  version:          1,
  wire:             'uint .size 4',
  description:      'Response code, written by target.',
  default_encoding: { Ok: 0, Failed: 1 },
  mutable_by:       'target',
  read_by:          'initiator',
  phase:            'response',
}
