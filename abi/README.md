# abi

**Status:** Header-only library defining the normative C-linking ABI of FSS.

`include/fss/`:
- `carrier.h` — `fss_carrier_t`: id, data pointer/offset, data size, meta
  pointer/offset, and meta size.  It is a 40-byte, 64-bit ABI layout.  Pointer
  fields are process-local pointers in memory and wire offsets in a wire form.
- `endpoint.h` — `fss_endpoint_t`: an artefact-owned inbound `receive` callback
  plus a transport-installed outbound `send` callback and context.  `send ==
  NULL` means that the endpoint is not attached for outbound delivery.
- `transport_abi.h` — the transport-plugin vtable: `channel`, `attach`, and
  `send_to`.  `channel` creates or finds an opaque channel; `attach` joins a
  live endpoint as an initiator or target; `send_to` is one-shot delivery.
  Artefact construction is local placement work, not a transport-vtable call.
- `artefact_abi.h` — entry points an FSS-deployable .dylib exports for binary-loader transports (normative for the dlopen transport family).

Transport plugins for the binary-loader family export
`fss_transport_abi_version`, `fss_transport_capabilities`,
`fss_transport_create`, `fss_transport_destroy`, and `fss_transport_name`.
Before resolving a factory or reading a vtable, a host resolves and calls the
version and capability queries.  It accepts only the ABI major it supports, a
plugin minor at least as new as its required minor, and the complete
`FSS_TRANSPORT_CAP_CHANNEL_ABI` capability set.  It then resolves the factory
symbols, verifies the exported name against its selected transport name, and
only then creates and uses the transport.  The version/capability queries keep
the fixed vtable safe across ABI-minor additions.

CMake target `fss-abi` is INTERFACE — anyone who needs the C ABI links it.
Language realizations are under `reference/languages/<lang>/`.
