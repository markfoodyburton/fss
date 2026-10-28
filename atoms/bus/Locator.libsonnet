{
  qname:       'bus::Locator',
  version:     1,
  wire:        'uint',
  description: 'The locator a transaction targets — e.g. a memory address (TLM) or a bus message id (CAN). Unsigned; width is per-carrier (byte_size in the composition).',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
