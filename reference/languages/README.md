# languages/ — per-language FSS bindings

A **language binding** is how a given language speaks the reference C ABI
(`abi/`): it mirrors the ABI structs (`fss_carrier_t`, `fss_endpoint_t`) in
the language's own type system and marshals calls across the FFI boundary, so
a model in that language can be an FSS node. It is one of two distinct things
the word "binding" can mean in FSS — keep them separate:

| kind | what | lives in |
|---|---|---|
| **language binding** (here) | per-language realization of the C ABI types and artefact/endpoint calls. Carrier-agnostic. | `reference/languages/<lang>/` |
| **carrier accessor binding** | typed get/set for *one* carrier in a language (e.g. the TLM carrier's fields). Carrier-specific. | `known_carriers/<c>/bindings/<lang>/`, `protocols/<p>/bindings/<lang>/` |

**The amount of binding needed is inversely proportional to how close the
language is to C.** The ABI *is* C, so:

| Language | Directory | Binding | Status |
|---|---|---|---|
| C | — | none — the ABI is C; a model includes `abi/` headers directly | n/a |
| C (carrier helpers) | `core-c/` | carrier-agnostic meta resolver (`fss/c/meta.h`), wire serialiser (`fss/c/wire.h`), runtime hash (`fss/c/meta_hash.h`) — compiled per consumer, not a shared lib | working (`c_meta_resolver`, `c_wire_serializer`, `c_meta_hash`) |
| C++ / SystemC | `systemc-tlm/` | artefact wrapper (`fss/systemc_tlm/artefact.h`) + carrier↔`tlm_generic_payload` translation | working (`tlm_live_gp_serialize` unit test; `fss_spi_memory_systemc_load` artefact-load smoke test) |
| Rust | `rust/` | `#[repr(C)]` ABI mirror + `fss_export_artefact!` macro | working (`rust_binding`, `can_rust`, `fss_can_inproc_to_rust`) |
| Python | `python/` | ctypes ABI mirror (`fss.py`): Python as host/driver or cross-process sink | working (`python_binding`, `can_python`, `fss_can_shm_to_python`) |
| Python (loader) | `python-loader/` | C cdylib that embeds CPython so a `.py` model is a loadable artefact | working (`agent_can_*_python`) |
| FMI / FMU | `fmi/` | FMI-3.0 import plumbing (`fmi.h`/`fmi.c`): struct mirrors, dlopen helpers, FMI-LS-BUS slot offset extraction. Carrier wrapping and artefact export live in `reference/examples/` (CAN FMI examples). | working (`can_fmi_roundtrip`) |

Channel creation and endpoint attachment are **transport** operations, not part
of a language binding.  After an initiator attach, a model invokes its installed
endpoint `send` callback; it does not call a transport on the data hot path.

Each binding is **hand-written** (no generator — see `docs/design/open-issues.md`
§10) and was written when a federation actually crossed into that language,
against the ABI as it then was — building one earlier is guessing at the shape.

The **carrier helpers** in `core-c/` are carrier-agnostic plain C used by the
SystemC binding and the transports: the Path-B meta resolver (`fss/c/meta.h` —
resolve a field's offset by qname from the carrier meta, cache by `(id, qname)`,
and iterate fields), the pointer-inlining wire serialiser (`fss/c/wire.h`), and
the runtime carrier-id hash (`fss/c/meta_hash.h`).  They are **not a shared
library** — each consumer compiles its own copy (the `fss-lang-c-meta` INTERFACE
target), free to diverge.
