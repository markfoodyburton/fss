{
  qname:       'control::Transport',
  version:     1,
  wire:        'tstr',
  description: 'Server/Client: the name of the transport that carries this data edge (fss_transport_name(); the agent resolves it to a plugin). Empty = the agent\'s default channel transport.',
  mutable_by:  'initiator',
  read_by:     'target',
  phase:       'request',
}
