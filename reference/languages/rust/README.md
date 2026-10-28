# languages/rust — FSS Rust language binding

The carrier-agnostic glue that lets a model written in Rust be an FSS node. It
mirrors the reference C ABI (`abi/include/fss/*.h`) as `#[repr(C)]` types and
provides the scaffolding to author an **artefact** — a `cdylib` that the
binary-loader transports (`reference/transports/inproc`, …) `dlopen` and drive, exactly
like the C and SystemC artefacts.

| ABI | Rust |
|---|---|
| `fss_carrier_t` | `Carrier` (`#[repr(C)]`, 40 bytes) |
| `fss_endpoint_t` | `Endpoint` (`#[repr(C)]`, 32 bytes) |
| `fss_carrier_fn` | `Option<extern "C" fn(*mut c_void, *mut Carrier) -> c_int>` |
| `fss_create` / `fss_get_endpoint` / `fss_destroy` | emitted by `fss_export_artefact!` |

This is a **language binding**, not a carrier binding: it knows nothing about
any particular carrier (TLM, CAN, …). A model's carrier-specific logic lives in
the example artefact that uses this crate.

## Authoring an artefact

```rust
use fss::{Artefact, Carrier, Endpoint, Node, FSS_SEND_OK};

struct MyTarget { node: Node }

impl Artefact for MyTarget {
    fn create(_name: &str) -> Self {
        let mut node = Node::new();
        node.add_endpoint("in", |c: &mut Carrier| {
            // inspect/mutate the data region in place via c.data() / c.data_mut()
            FSS_SEND_OK
        });
        MyTarget { node }
    }
    fn get_endpoint(&mut self, name: &str) -> *mut Endpoint {
        self.node.endpoint_ptr(name)
    }
}

fss::fss_export_artefact!(MyTarget);
```

The artefact crate sets `crate-type = ["cdylib"]` and a path dependency:
`fss = { path = "…/languages/rust" }`.

## Build / test

`cargo test` (zero dependencies, offline). Wired into CMake as the `rust_binding`
ctest; self-skips when `cargo` is absent. `bind`/`send` are **transport**
operations (a model calls them through a transport vtable), so they are not part
of this binding — its interop is proven by the federation examples that load a
Rust artefact through a real transport.
