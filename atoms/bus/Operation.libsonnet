{
  qname:            'bus::Operation',
  version:          1,
  wire:             'uint',
  description:      'The operation: Read=request data, Write=deliver data, Ignore=address-only/no-op. Width and wire encoding are per-carrier (a CAN frame overrides the encoding).',
  default_encoding: { Read: 0, Write: 1, Ignore: 2 },
  mutable_by:       'initiator',
  read_by:          'target',
  phase:            'request',
}
