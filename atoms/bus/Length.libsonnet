{
  qname:       'bus::Length',
  version:     1,
  wire:        'uint',
  description: 'Payload length in bytes; width per-carrier.',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
