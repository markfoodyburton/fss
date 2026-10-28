# languages/rust — FSS Rust language binding (carrier-agnostic).
#
# A library crate (rlib) that mirrors the C ABI and provides artefact-authoring
# glue (fss_export_artefact!).  Artefact crates depend on it by path and build
# as cdylibs the transports dlopen.  Built/tested out-of-source via cargo;
# self-skips if cargo is absent.

find_program(CARGO_EXE cargo)
if(NOT CARGO_EXE)
    message(STATUS "languages/rust skipped — cargo not found")
    return()
endif()

set(_FSS_RUST_DIR        "${CMAKE_CURRENT_SOURCE_DIR}")
set(_FSS_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/target")

# Compile the binding at build time (catches breakage in CI even when no
# artefact consumes it yet).
add_custom_target(fss_rust_binding ALL
    COMMAND ${CARGO_EXE} build --offline --quiet
            --manifest-path "${_FSS_RUST_DIR}/Cargo.toml"
            --target-dir "${_FSS_RUST_TARGET_DIR}"
    COMMENT "cargo build fss rust binding")

# Unit tests: ABI layout + the endpoint trampoline glue.
enable_testing()
add_test(NAME rust_binding
         COMMAND ${CARGO_EXE} test --offline
                 --manifest-path "${_FSS_RUST_DIR}/Cargo.toml"
                 --target-dir "${_FSS_RUST_TARGET_DIR}")

# Expose the crate dir so artefact examples can add a path dependency on it.
set(FSS_RUST_BINDING_DIR "${_FSS_RUST_DIR}" CACHE INTERNAL "FSS Rust binding crate dir")
