# config:: — Configuration parameter atoms

Parameter-exchange primitives for generic configuration protocols.
Not specific to any language or framework — SystemC-CCI is one
binding mapping, FMI variables are another.  The `cci::` namespace
used in earlier FSS drafts is retired; `config::` is the replacement.

## Catalogue

| Atom                 | Version | wire          | byte_size | mutable_by | read_by   | phase    | Description                                                                                                           |
|----------------------|---------|---------------|-----------|------------|-----------|----------|-----------------------------------------------------------------------------------------------------------------------|
| [`config::OpCode`](OpCode.libsonnet)       | 1       | `uint .size 4` | 4        | initiator  | target    | request  | Operation code.  Allowed values: `Get = 0`, `Set = 1`, `Describe = 2`.                                               |
| [`config::ParamName`](ParamName.libsonnet) | 1       | `tstr`        | variable  | initiator  | target    | request  | Hierarchical parameter name.  UTF-8 byte sequence; need not be NUL-terminated (length given by `byte_size`).         |
| [`config::ParamValue`](ParamValue.libsonnet) | 1     | `bstr`        | 40 (fixed, opaque) | either | both   | both     | Parameter value; opaque bytes for v0.1.  Typed value-shapes are deferred to the config protocol's design. |
| [`config::Status`](Status.libsonnet)       | 1       | `uint .size 4` | 4        | target     | initiator | response | Response code, written by target.  Allowed values: `Ok = 0`, `NotFound = 1`, `TypeMismatch = 2`, `ReadOnly = 3`.     |

## Notes

- **Parameter listing/enumeration is deferred.**  For v0.1 the host
  iterates Get/Describe by known name; a discovery mechanism (listing
  all parameters or enumerating by prefix) will be added as a separate
  protocol when consumers require it.
- **Typed `config::ParamValue` shapes are deferred.**  For v0.1 the
  value is opaque bytes; typed value-shapes (integer, float, string,
  boolean) are deferred and will be described by a sub-meta, not a
  separate carrier.

## Future atoms

- `config::ParamHandle` — opaque integer handle for repeated access to
  a parameter, avoiding repeated name lookups.
- `config::ParamFlags` — read-only, write-once, volatile, and other
  attribute bits.
- `config::ParamType` — encoded type tag for `Describe` responses.
