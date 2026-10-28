# Exploration

Half-formed ideas and spikes — kept for reference, but **not** part of
the design or the spec. Things here may be stale, contradicted by later
decisions, or abandoned. For what is actually settled, see the
[design notes](../README.md) and the [spec](../../spec/).

## Contents

- [cddl-prior-art/](cddl-prior-art/) — worked-example CDDL interface
  packages (`fss-community:` namespace) showing how community packages
  *would* plug into the carrier mechanism. Explicitly not standardised.
- `shm-spike/shm_spike.c` — a self-contained C program proving the
  POSIX shared-memory + named-semaphore primitives work together on
  macOS and Linux. Backs [shm-transport.md](../shm-transport.md) §7.
- `rfc8610.txt` — the CDDL RFC, kept locally for reference.
