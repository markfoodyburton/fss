# transport_bench

`transport_bench` is a manual PoC micro-benchmark. It times sends of one
16-byte flat carrier. It is not a CTest and it does not measure a complete
application or platform.

The benchmark has two modes:

| Mode | Setup |
|---|---|
| `inproc` | Loads `fss_minimal_echo`, binds its `in` endpoint directly, and times local calls. |
| `shm` | Forks a receiver, creates a one-way shared-memory channel, and times parent sends. |

The modes have different execution models. In-process mode invokes the peer
receive callback in one process. Shared-memory mode uses a cross-process ring
and receiver. Do not compare the values as equivalent end-to-end simulation
measurements.

## Run

Build the targets in a clean repository-root CMake build with binary directory
`build`, then use these reference CMake output paths. Discover the produced
files in your build tree if its generator or output layout differs:

```sh
# In-process mode
./build/reference/examples/tools/transport_bench/transport_bench \
  inproc \
  ./build/transports/libfss_inproc.so \
  ./build/reference/examples/fss-artefacts/fss_minimal_echo/libfss_minimal_echo.so \
  200000

# Shared-memory mode
./build/reference/examples/tools/transport_bench/transport_bench \
  shm ./build/transports/libfss_shm.so 200000
```

`N` is optional and defaults to `200000`. CMake chooses the module suffix on
your platform; replace `.so` when it differs.

The shared-memory result includes the work needed to enqueue each carrier. It
is not a round-trip measurement and it does not establish transport portability
for live, pointer-based carriers.
