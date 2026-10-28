{
  qname:       'config::ParamName',
  version:     1,
  wire:        'tstr',
  description: 'Hierarchical parameter name.  UTF-8 byte sequence; need not be NUL-terminated (length given by byte_size in the meta entry).',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
