{
  qname:            'config::Status',
  version:          1,
  wire:             'uint .size 4',
  description:      'Response code, written by target.',
  default_encoding: { Ok: 0, NotFound: 1, TypeMismatch: 2, ReadOnly: 3 },
  mutable_by:       'target',
  read_by:          'initiator',
  phase:            'response',
}
