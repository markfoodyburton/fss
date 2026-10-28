// Config protocol — Get/Set/Describe of named parameters (in-process form).
//
// Spec:           docs/spec/04-protocols.md
// Atom registry:  atoms/{config,fss}/
//
// In-process form: config::ParamName uses layout fss:local-ptr-nul
// (NUL-terminated string pointer).  Wire transports inline.
//
// mutable_by / read_by / phase are intrinsic to each atom (see imports).

local config = import '../../atoms/config.libsonnet';
local core   = import '../../atoms/core.libsonnet';

{
  name: 'config::param_request',
  atoms: [
    // Operation code.
    { atom: config.OpCode,     byte_offset: 0  },

    // Parameter name.  In-process: NUL-terminated string pointer; wire:
    // inlined bytes (the tstr itself, no layout).
    { atom: config.ParamName,  byte_offset: 8,
      layout: 'fss:local-ptr-nul' },

    // Parameter value (opaque bytes in v0.1; fixed 40-byte slot).
    { atom: config.ParamValue, byte_offset: 16, byte_size: 40 },

    // Response code.
    { atom: config.Status,     byte_offset: 56 },

    // Extension hook (sub-meta): no data-region bytes.
    { atom: core.Extensions },
  ],
}
