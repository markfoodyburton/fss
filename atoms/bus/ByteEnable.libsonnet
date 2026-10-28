{
  qname:       'bus::ByteEnable',
  version:     1,
  wire:        'bstr',
  description: 'Optional byte-enable mask; one byte per data byte (0x00 disabled, 0xFF enabled), length in bus::ByteEnableLength.  Wire form is a plain bstr; in-process protocols may override layout to fss:local-ptr (like bus::Data).',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
