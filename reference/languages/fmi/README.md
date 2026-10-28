# languages/fmi — FMI 3.0 Co-Simulation binding

`fss-fmi` is a proof-of-concept (PoC) FSS language binding for FMI 3.0
Co-Simulation. It is
protocol- and carrier-agnostic: it provides the FMI *mechanics* (load an
FMU, drive its clocks/variables/steps) and nothing about any particular
protocol. The current CAN-over-FMI mappings live in the paced FMI artefacts
under `reference/examples/fss-artefacts/`, not here.

## Scope (read this first)

FMI is a large standard; this binding covers **one direction of one part**
of it. Precisely:

- **IN SCOPE — importing and driving a single FMU.** `fss-fmi` `dlopen`s one
  FMU, resolves its `fmi3*` Co-Simulation entry points, and steps it:
  `fss_fmi_open` (Instantiate → EnterInitializationMode →
  ExitInitializationMode → EnterStepMode), then the event/step accessors
   (`set_clock` / `set_binary` / status-aware `get_*_status` /
   `step_status` / `enter_event_status` + `advance_status`), then
   `fss_fmi_close` (Terminate → FreeInstance).  The status-aware APIs return
   normalized FMI status; step results also report FMI `DoStep` flags.  Legacy
   value-only and integer-returning wrappers remain for compatibility but can
   hide warnings or failures; new callers should use the status-aware APIs.
  In FMI terms, this binding plays the **importer** (the co-simulation
  master's per-FMU driver) for **one** FMU.

- **OUT OF SCOPE (for now):**
  - **FMU export.** Making an FSS federation *appear as* an FMU to an
    external FMI master (exporting `fmi3*` entry points) is not provided.
  - **System / SSP import.** Orchestrating *many* FMUs with connections
    (an `.ssp` system, i.e. a full FMI "simulator" in the colloquial sense)
    is a federation/orchestration concern, not this binding. An FSS
    federation composes nodes via the transport; each FMU node uses this
    binding to drive its own FMU.
  - **Model Exchange** and **Scheduled Execution** interface types — only
    Co-Simulation is wired.

So "FMI binding" here means "an FSS node that wraps one imported
Co-Simulation FMU", not "an FMI simulator". Export and SSP are deferred.
This is PoC support with a narrow FMU API surface; callers must supply the
protocol mapping, value references, model configuration, and deployment logic.

## Surface

See `fmi.h` for the full API. New code should use
`fss_fmi_get_{u32,u64,clock,binary}_status`,
`fss_fmi_enter_event_status`, and
`fss_fmi_{advance,step}_status`.  Getter status calls initialise their output
to a safe empty value before the FMI call.  Step-status calls return
`fss_fmi_step_result_t`, including the normalized FMI status,
`event_handling_needed`, `terminate_simulation`, `early_return`, and
`last_successful_time`.  `set_clock` and `set_binary` return normalized status
as integer values; `get_u32`, `get_u64`, `get_clock`, `get_binary`,
`enter_event`, `advance`, and `step` are legacy wrappers.

The binding requires the FMU to export the Co-Simulation entry points it uses;
it self-skips at build time if the FMI 3.0 headers were not fetched
(`FMI_HEADERS_DIR` unset).
