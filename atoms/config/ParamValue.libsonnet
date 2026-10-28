{
  qname:       'config::ParamValue',
  version:     1,
  wire:        'bstr',
  description: 'Parameter value, opaque bytes for v0.1 (the composition fixes byte_size).  Typed value-shapes — integer, float, string, boolean — are deferred to the config protocol\'s design; they will be described by a sub-meta, not a separate carrier.',
  mutable_by:  'either',
  read_by:     'both',
  phase:       'both',
}
