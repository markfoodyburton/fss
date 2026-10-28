{
  qname:       'bus::Data',
  version:     1,
  wire:        'bstr',  // variable-length raw bytes; size determined by bus::Length
  description: 'Payload bytes; size given by bus::Length.  The wire form is a plain `bstr`; in-process protocols may override the layout to `fss:local-ptr` (declared per-protocol via the entry\'s `layout` field and `attrs.length_at`).',
  mutable_by:  'either',
  read_by:     'both',
  phase:       'both',
}
