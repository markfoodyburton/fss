{
  qname:            'control::OpCode',
  version:          1,
  wire:             'uint .size 4',
  description:      'Topology control operation.  Ping=0; Construct=1 (bring a node to life through the model loader); Server=2 (inbound channel, set_sink to Node); Client=3 (outbound channel, bind Node to Locator/PeerEp); Arm=4; Bind=5 (in-process edge inside the agent); Stop=6 (teardown trigger); Done=9 (wiring complete: arm, run, wait, teardown).',
  default_encoding: { Ping: 0, Construct: 1, Server: 2, Client: 3, Arm: 4, Bind: 5, Stop: 6, Done: 9 },
  mutable_by:       'initiator',
  read_by:          'target',
  phase:            'request',
}
