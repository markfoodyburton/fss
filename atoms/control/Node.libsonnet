{
  qname:       'control::Node',
  version:     1,
  wire:        'tstr',
  description: 'Target node, or "node/endpoint", the op acts on (Construct: node name; Server/Client/Bind/Arm: local endpoint).',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
