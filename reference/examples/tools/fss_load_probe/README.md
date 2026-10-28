# fss_load_probe

`fss_load_probe` is a PoC SystemC/TLM smoke driver. It loads an artefact with
the in-process transport, resolves artefact endpoints through placement, and
exercises the TLM receive paths.

It is not a generic artefact tester. The probe constructs a live
`tlm_generic_payload` and expects these placement endpoints from
`fss_spi_memory_systemc`:

- `spi/socket` for the TLM transaction carrier;
- `spi/fss/sim` for `Started` and `Stopped` lifecycle events.

It therefore cannot validate arbitrary artefacts or shared-memory and Zenoh
transport paths. CTest registers it as `fss_spi_memory_systemc_load` when the
required SystemC and generated headers are available.

## Run directly

After a clean repository-root CMake build with binary directory `build`, pass
both the transport module and the artefact module. These are the reference
CMake output paths; discover the produced files in your build tree if its
generator or output layout differs:

```sh
./build/reference/examples/tools/fss_load_probe/fss_load_probe \
  ./build/transports/libfss_inproc.so \
  ./build/reference/examples/fss-artefacts/fss_spi_memory_systemc/fss_spi_memory_systemc.dylib
```

The transport modules are staged in `build/transports/`. The probe executable
and the SystemC artefact remain in their CMake binary directories. On a
non-macOS build, use the platform suffix that CMake generated for the transport
module; the SystemC helper currently sets the artefact suffix to `.dylib`.

Successful output ends with `[OK]` and the artefact path.
