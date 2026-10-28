# protocols/

FSS-defined protocols where FSS owns both the carrier shape AND the
behavioural specification.

## How this differs from `known_carriers/`

| | `protocols/`               | `known_carriers/`                |
|---|---|---|
| Protocol design | FSS                            | external (IEEE, OSCI, vendor)    |
| Carrier shape   | FSS owns                       | FSS expresses                    |
| Behaviour spec  | this repo                      | external reference               |

## Layering

| Tier              | What                                              | Where                                           |
|-------------------|---------------------------------------------------|-------------------------------------------------|
| Field types       | Mechanical byte interpretation (CDDL flat-layout profile) | [docs/spec/02-encodings.md](../docs/spec/02-encodings.md) |
| Atoms             | Named, versioned semantic field shapes            | [`atoms/`](../atoms/) — shared with `known_carriers/` |
| Carrier shapes    | Atom compositions with explicit byte_offsets      | This directory and `known_carriers/`            |
| Protocols         | Carrier shape(s) + behavioural specification      | This directory (when FSS owns the behaviour)    |

## Protocols

| Directory       | Status                                          |
|-----------------|-------------------------------------------------|
| `config/`       | Parameter Get/Set/Describe; CDDL skeleton; design-pending bits in README. |
| `sim-events/`   | Simulation lifecycle (phase transitions + readiness wave); multi-message choreography design-pending. |
| `time-sync/`    | Time-advance window `(Lower, Upper)` at a `Unit` resolution; symmetric one-way exchange; binding handlers in progress. |

## Layout

Each protocol under `protocols/<name>/`:

- `README.md` — atom composition table, behavioural spec (Mermaid sequence diagram + prose), design-pending markers.
- `<carrier>.jsonnet` — the carrier shape (e.g. `param_request.jsonnet`, `event.jsonnet`).
- `bindings/<lang>/...` — transitional language binding (when one exists).
- `CMakeLists.txt` — defines the binding's INTERFACE library.

The atom registry at [`atoms/`](../atoms/) is shared with `known_carriers/`.
