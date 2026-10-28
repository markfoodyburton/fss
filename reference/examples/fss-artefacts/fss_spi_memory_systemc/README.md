# fss_spi_memory_systemc

FSS-binding form of the `spi_memory_systemc` model (see
`../../example-models/spi_memory_systemc/`).

Built as a `.dylib` that statically embeds its own SystemC and CCI kernels.
The host `dlopen`s it `RTLD_LOCAL` and calls `fss_create` / `fss_destroy` /
`fss_get_endpoint` from `<fss/artefact_abi.h>`.  See
`reference/languages/systemc-tlm/` for the binding machinery.

## Build & test

```sh
cmake -S . -B build -DFSS_FETCH_SYSTEMC=ON -DFSS_FETCH_CCI=ON
cmake --build build --target fss_spi_memory_systemc fss_load_probe
ctest --test-dir build -R fss_spi_memory_systemc_load --output-on-failure
```
