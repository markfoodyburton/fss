{
  qname:            'config::OpCode',
  version:          1,
  wire:             'uint .size 4',
  description:      'Operation code.  Get=0 returns the named parameter\'s current value; Set=1 stores ParamValue at ParamName; Describe=2 returns metadata about the parameter.',
  default_encoding: { Get: 0, Set: 1, Describe: 2 },
  mutable_by:       'initiator',
  read_by:          'target',
  phase:            'request',
}
