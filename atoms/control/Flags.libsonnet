{
  qname:            'control::Flags',
  version:          1,
  wire:             'uint .size 4',
  description:      'Per-op bitmask: COUNT=1 (wrap the sink with the frame-count oracle), OPTIONAL=2 (skip if the local endpoint is absent), ZENOH=4 (this channel rides zenoh, not shm).',
  mutable_by:       'initiator',
  read_by:          'target',
  phase:            'request',
}
