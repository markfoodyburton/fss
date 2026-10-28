# languages/ — per-language FSS runtime bindings.
#
# A language binding is the carrier-agnostic glue that lets a model in a given
# language be an FSS node: represent fss_carrier / fss_endpoint, register a
# receive callback, and bind/send carriers.  It is independent of any specific
# carrier (those typed accessors live with each carrier under bindings/<lang>/)
# and of any specific transport.

# core-c (binding-independent C helpers: fss-lang-c-meta) is added directly by
# the root CMakeLists BEFORE transports, since transports link it too.

add_subdirectory(systemc-tlm)   # C++ / SystemC-TLM binding (artefact wrapper + the
                                # Path-B meta resolver it consumes; defines
                                # fss_systemc_tlm_add_module).

add_subdirectory(fmi)       # FMI 3.0 Co-Simulation binding: turns an FMI-LS-BUS
                            # CAN-receiver Network FMU into an FSS artefact endpoint.
                            # Self-skips if FMI headers are not fetched.

add_subdirectory(rust)      # Rust binding: ABI types + artefact-authoring glue
                            # (cdylib artefacts the transports dlopen).  Self-skips
                            # if cargo is absent.

add_subdirectory(python)    # Python binding (ctypes): Python as a host/driver or
                            # a cross-process receiver.  Self-skips if python3 absent.

add_subdirectory(python-loader)  # Embeds CPython so a Python model IS a dlopen'd
                                 # artefact (open()-able like C/Rust/SystemC).
                                 # Self-skips if Python3 Development.Embed absent.

# C needs no binding — the ABI *is* C; a C model includes the abi/ headers directly.
