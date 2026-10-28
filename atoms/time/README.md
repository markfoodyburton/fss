# time:: — Simulation-time atoms

Primitives for the time-sync protocol: a node tells its peer a **window** of
simulated time it should run within.  Time is carried faithfully as SystemC
`sc_time` — a `uint64` tick count at a resolution given by the IEEE-1666
`sc_time_unit` exponent — rather than inventing a representation.

## Catalogue

| Atom | Version | wire | byte_size | mutable_by | read_by | phase | Description |
|------|---------|------|-----------|------------|---------|-------|-------------|
| [`time::Unit`](Unit.libsonnet)   | 1 | `int .size 4`  | 4 | initiator | target | request | Resolution of the bounds: the `sc_time_unit` exponent (`Femtosecond=0 … Second=5`); absolute time = value · 1000^Unit fs. |
| [`time::Lower`](Lower.libsonnet) | 1 | `uint .size 8` | 8 | initiator | target | request | Run **at least** to this (tick count at `Unit`).  0 = no lower requirement. |
| [`time::Upper`](Upper.libsonnet) | 1 | `uint .size 8` | 8 | initiator | target | request | Do **not** simulate past this (tick count at `Unit`).  `(0, MAX)` = run free; `(0, 0)` = stop. |

Used by the [time-sync protocol](../../protocols/time-sync/) (`time::window`
on the `fss/time` control port).

## Note

FMI represents time as `fmi3Float64` seconds; an FMI node converts the
`(Unit, value)` window to/from seconds at its binding.  The `sc_time` carriage
is the v0.1 choice and may gain a richer representation if a simulator needs one.
