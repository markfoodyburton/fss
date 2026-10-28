{
  qname:       'time::Upper',
  version:     1,
  wire:        'uint .size 8',
  description: 'Upper bound of a time window: the receiver MUST NOT simulate past this time (a tick count at time::Unit).  With time::Lower: (0, MAX) = run freely; (0, 0) = stop.',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
