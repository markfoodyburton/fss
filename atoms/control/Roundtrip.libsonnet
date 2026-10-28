{
  qname:            'control::Roundtrip',
  version:          1,
  wire:             'uint .size 4',
  description:      'Server/Client: 1 if the DATA channel being stood up is itself round-trip (create-rr/attach-rr, e.g. a TLM b_transport), 0 for one-way.',
  mutable_by:       'initiator',
  read_by:          'target',
  phase:            'request',
}
