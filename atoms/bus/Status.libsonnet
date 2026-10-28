{
  qname:            'bus::Status',
  version:          1,
  wire:             'int',
  description:      'Response code, written by target after executing the transaction.  Values and semantics are the TLM-2.0 tlm_response_status (Ok=1; Incomplete=0 is the initial/unset value; negative values are errors).',
  default_encoding: { Ok: 1, Incomplete: 0, GenericError: -1, AddressError: -2, CommandError: -3, BurstError: -4, ByteEnableError: -5 },
  mutable_by:       'target',
  read_by:          'initiator',
  phase:            'response',
}
