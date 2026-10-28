# can_fmi — FMI 3.0 / LS-BUS CAN PoC models

This directory contains three FMI 3.0 Network FMUs and one standalone
co-simulation harness. The harness connects `can_initiator` to `can_target` as
a trivial CAN bus. Nothing here references FSS.

The models are PoC implementations of the
[FMI-LS-BUS](https://github.com/modelica/fmi-ls-bus) layered standard. They are
not packaged `.fmu` archives and are not a general FMI integration example.

## Models

The models use the FMI-LS-BUS low-cut CAN convention:

- `CanTransmit` operations are serialised into FMI 3.0 binary variables.
- Clocks signal sent and received frames. The FMUs use Event Mode.
- The source uses the FMI-LS-BUS CAN utility macros; it does not hand-code the
  operation format.

| FMU | Role | Main variables |
|---|---|---|
| `can_initiator` | sender | `Tx_Clock`, `Tx_Data`, `TxCount` |
| `can_controller` | queued relay | accepts a transmit request and emits a frame after a step |
| `can_target` | receiver | `Rx_Clock`, `Rx_Data`, `LastId`, `RxCount`, `LastByte0`, `LastDlc` |

Each model ships a `modelDescription.xml`. The in-repo code loads the built
shared modules directly; it does not create `.fmu` archives for external tools.

`harness_fmi_can` loads the initiator and target modules with `RTLD_LOCAL`,
steps them, and copies each initiated frame to the target. It passes only when
the target receives the expected 16 frames. The controller FMU is built for
the queued FMI FSS controller artefact; the standalone harness does not use it.

## Run

```sh
cmake -S . -B build -DFSS_FETCH_FMI=ON
cmake --build build --target harness_fmi_can
ctest --test-dir build -R can_fmi_roundtrip --output-on-failure
```

Expected output includes `[OK] can-fmi round-trip: 16 CAN frames delivered`.
