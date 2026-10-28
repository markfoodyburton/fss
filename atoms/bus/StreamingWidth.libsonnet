{
  qname:       'bus::StreamingWidth',
  version:     1,
  wire:        'uint .size 4',
  description: 'TLM-2.0 streaming width; bytes after which the address wraps.  Equals bus::Length for a non-streaming (linear) transfer.',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
