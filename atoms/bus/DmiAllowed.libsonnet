{
  qname:       'bus::DmiAllowed',
  version:     1,
  wire:        'bool',
  description: 'Hint: the target grants direct-memory-interface access for this region.  Set false when the path cannot support DMI (e.g. off-machine); a shared-memory transport may still honour it.',
  mutable_by:  'target',
  read_by:     'initiator',
  phase:       'response',
}
