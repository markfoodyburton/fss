# Examples

These examples are proof-of-concept (PoC) reference code. They show current
interfaces and build combinations; they are not a supported product interface.

The tree has four active groups:

| Directory | Purpose |
|---|---|
| [`example-models/`](example-models) | FSS-independent source models and standalone FMI harnesses. |
| [`fss-artefacts/`](fss-artefacts) | Loadable FSS artefacts. |
| [`tools/`](tools) | Single-artefact probe and transport benchmark. |
| [`platforms/`](platforms) | Generated CAN platform test matrix. |

There is no `federations/` directory. The CAN platform is the current
multi-node example.

## Models

`example-models/` contains these current model families:

| Directory | Content |
|---|---|
| `can_fmi/` | Three FMI 3.0 / FMI-LS-BUS CAN FMUs (`can_initiator`, `can_controller`, and `can_target`) plus the initiator-to-target standalone harness. |
| `can_systemc/` | SystemC CAN source used by the SystemC artefacts. |
| `spi_initiator_systemc/` | SystemC TLM initiator source, compiled by its artefact. |
| `spi_memory_systemc/` | SystemC TLM memory source, compiled by its artefact. |

The SPI source directories do not define standalone CMake targets. The Rust
and Python paced CAN implementations are self-contained artefacts, not
standalone entries in `example-models/`.

## FSS artefacts

The artefact directory currently builds:

| Family | Artefacts |
|---|---|
| Fixture | `fss_minimal_echo` |
| SystemC SPI | `fss_spi_memory_systemc`, `fss_spi_initiator_systemc` |
| Paced CAN endpoints | `fss_can_initiator_paced_{systemc,rust,python,fmi}`, `fss_can_target_paced_{systemc,rust,python,fmi}` |
| Queued CAN controllers | `fss_can_controller_queued_{systemc,rust,python,fmi}` |
| SystemC CAN | `fss_can_combined_systemc` |

Entries build only when their required generated headers, language runtime, and
toolchain are available. The FMI artefacts use `fss-fmi` to import the three
local CAN FMU modules. They are PoC adapters, not general FMI integration.

## CAN platform

`platforms/can_bus/` generates tests for a paced CAN pipeline:

```
initiator -> queued controller -> target
```

Each role can use a built SystemC, Rust, Python, or FMI artefact. Each hop can
use a built in-process, shared-memory, or Zenoh transport. Test names start
with `agent_can_`; additional fan-out, fan-in, and combined SystemC tests have
the `fanout_matrix`, `fanin_matrix`, and `combined` labels. This matrix is a
PoC and has known cross-process scheduling limits; use individual tests to
diagnose a failed cross-process cell.

## Tools

| Directory | Purpose |
|---|---|
| [`tools/fss_load_probe/`](tools/fss_load_probe) | SystemC/TLM smoke probe for `fss_spi_memory_systemc`. It requires an in-process transport module and an artefact module. |
| [`tools/transport_bench/`](tools/transport_bench) | Manual 16-byte send-latency probe for the in-process and shared-memory transport implementations. |

## Build

From a clean CMake build directory configured at the repository root, use:

```sh
cmake -S . -B build -DFSS_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Zenoh is optional and is off by default. To build its transport and Zenoh
matrix entries, configure with `-DFSS_FETCH_ZENOH=ON`. The fetch builds
zenoh-c through Cargo, so CMake, a C11 compiler, Cargo/Rust, and platform
linker tools must be available. `zenohc::lib` may be static and is linked into
the `fss_zenoh` loadable module; use one compatible host toolchain and
architecture, with PIC support where required. Do not substitute an unrelated
shared Zenoh library for the fetched target.

When examples are enabled, CMake enables the SystemC, CCI, and FMI header
fetches. Individual entries still skip when a required generator, runtime, or
toolchain is unavailable.
