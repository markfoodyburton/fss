# spi_initiator_systemc

TLM-2.0 initiator that benchmarks seven access phases (A–G) across two address
regions: a register region at `register_base` (default `0x0`) and a memory
region at `memory_base` (default `0x1000`), each 4 KiB. Both addresses are
CCI parameters (`cci::cci_param<uint64_t>`) and can be overridden from a
platform-level broker before elaboration starts.

Phase A warms up b\_transport; B and C exercise bulk b\_transport writes and
reads against the register and memory regions respectively; D grants DMI for
the memory region and benchmarks raw-pointer bulk access; E and F repeat the
memory accesses with a `wait(1, SC_NS)` between each transaction to demonstrate
simulation-time advancement alongside wall-clock throughput; G accesses the
naughty register at `register_base + naughty_offset` (default 0xF00), where the
controller deliberately calls `wait()` inside `b_transport` — time advances per
transaction with no per-op wait in the initiator.

The op count defaults to 1 M and is overridden by the harness's first CLI
argument.
