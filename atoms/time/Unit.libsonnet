{
  qname:            'time::Unit',
  version:          1,
  wire:             'int .size 4',
  description:      'Resolution of the window bounds, as the IEEE 1666 sc_time_unit exponent (powers of 1000 from femtoseconds): Femtosecond=0, Picosecond=1, Nanosecond=2, Microsecond=3, Millisecond=4, Second=5.  Absolute time = value * 1000^Unit femtoseconds.  Negative exponents (sub-fs) are permitted but unusual.  Mirrors SystemC sc_time, which is a uint64 tick count interpreted at a resolution — FSS carries that representation faithfully rather than inventing one.',
  default_encoding: { Femtosecond: 0, Picosecond: 1, Nanosecond: 2, Microsecond: 3, Millisecond: 4, Second: 5 },
  mutable_by:       'initiator',
  read_by:          'target',
  phase:            'request',
}
