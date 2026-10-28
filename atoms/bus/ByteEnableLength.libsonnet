{
  qname:       'bus::ByteEnableLength',
  version:     1,
  wire:        'uint .size 4',
  description: 'Number of bytes in bus::ByteEnable; 0 means no byte-enable mask.',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
