// Control protocol — the topology-control instruction carrier
// (orchestrator -> agent).  Standard, self-describing carrier: round-trip
// (control::Status is mutable_by:'target').  v0.1 in-process/shm form: every
// field is inline (fixed byte_size), so the wire bytes are identical to the
// flat fss_ctrl_instr struct the runtime casts.  The instruction VOCABULARY
// (which ops, in what order) is the orchestrator's affair (NON-NORMATIVE,
// runtime/control-protocol); the CARRIER is standard.

local control = import '../../atoms/control.libsonnet';
local core    = import '../../atoms/core.libsonnet';

{
  name: 'control::instr',
  atoms: [
    { atom: control.OpCode,    byte_offset: 0  },
    { atom: control.Status,    byte_offset: 4  },
    { atom: control.Roundtrip, byte_offset: 8  },
    { atom: control.Flags,     byte_offset: 12 },
    { atom: control.Node,      byte_offset: 16,  byte_size: 64  },
    { atom: control.Locator,   byte_offset: 80,  byte_size: 128 },
    { atom: control.PeerEp,    byte_offset: 208, byte_size: 48  },
    { atom: control.Transport, byte_offset: 256, byte_size: 32  },
    { atom: core.Extensions },
  ],
}
