// Time-sync protocol — a one-way time WINDOW (run-to / do-not-pass).
//
// Spec:           docs/spec/04-protocols.md
// Atom registry:  atoms/time/
//
// A node sends its peer a window (time::Lower, time::Upper): "execute AT LEAST
// up to Lower, and do NOT simulate past Upper".  (0, MAX) = run freely;
// (0, 0) = stop.  The exchange is SYMMETRIC and ONE-WAY: the peer acts and
// replies with its OWN window — at the latest when it reaches Upper, earlier if
// it can.  Across a federation the time skew is then bounded by
// quantum x hop-count (e.g. initiator→controller→device = 2 hops → <= 2 quanta).
//
// Time is carried faithfully as SystemC sc_time: a uint64 tick count
// (time::Lower / time::Upper) at a resolution given by the sc_time_unit exponent
// (time::Unit).  Absolute time = value * 1000^Unit femtoseconds.  Delivered to
// the standard control port "fss/time".

local time = import '../../atoms/time.libsonnet';
local core = import '../../atoms/core.libsonnet';

{
  name: 'time::window',
  atoms: [
    // Shared resolution of both bounds (sc_time_unit exponent).
    { atom: time.Unit,  byte_offset: 0 },

    // Run at least to here (tick count at Unit).
    { atom: time.Lower, byte_offset: 8 },

    // Do not simulate past here (tick count at Unit).
    { atom: time.Upper, byte_offset: 16 },

    // Extension hook (sub-meta): no data-region bytes.
    { atom: core.Extensions },
  ],
}
