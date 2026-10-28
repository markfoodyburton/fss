{
  qname:       'control::Locator',
  version:     1,
  wire:        'tstr',
  description: 'Op-dependent locator: Construct: model locator (DLL path); Server/Client: channel name; Bind: peer "node/endpoint".',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
