{
  qname:       'time::Lower',
  version:     1,
  wire:        'uint .size 8',
  description: 'Lower bound of a time window: a request to execute AT LEAST up to this time (a tick count at time::Unit).  The receiver should advance its clock to >= this before it replies with its own window.  0 = no lower requirement.',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
