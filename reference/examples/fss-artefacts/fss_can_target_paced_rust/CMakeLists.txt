# fss_can_target_paced_rust — time-synced Rust CAN device (cdylib FSS artefact).
#
# The Rust counterpart of fss_can_target_paced_systemc: a CAN rx target with a
# windowed invented clock (fss::TimeSync) so it participates in time-sync on
# fss/time.  Built via cargo; self-skips cleanly when cargo is absent.  A pure
# consumer: it META-RESOLVES any CAN frame's fields by qname (no generated
# carrier constants needed — only the fss crate's meta reader).

find_program(CARGO_EXE cargo)
if(NOT CARGO_EXE)
    message(STATUS "examples/fss-artefacts/fss_can_target_paced_rust skipped — cargo not found")
    return()
endif()

set(RUST_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/target)

if(APPLE)
    set(_RUST_LIB_NAME libfss_can_target_paced_rust.dylib)
elseif(WIN32)
    set(_RUST_LIB_NAME fss_can_target_paced_rust.dll)
else()
    set(_RUST_LIB_NAME libfss_can_target_paced_rust.so)
endif()

set(RUST_CDYLIB_OUTPUT ${RUST_TARGET_DIR}/release/${_RUST_LIB_NAME})
set(FSS_CAN_TARGET_PACED_RUST_LIB ${CMAKE_CURRENT_BINARY_DIR}/${_RUST_LIB_NAME}
    CACHE INTERNAL "Path to the fss_can_target_paced_rust cdylib")

add_custom_command(
    OUTPUT ${FSS_CAN_TARGET_PACED_RUST_LIB}
    COMMAND ${CARGO_EXE} build --offline --release
            --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
            --target-dir ${RUST_TARGET_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy
            ${RUST_CDYLIB_OUTPUT} ${FSS_CAN_TARGET_PACED_RUST_LIB}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
        ${CMAKE_SOURCE_DIR}/reference/languages/rust/src/lib.rs
    COMMENT "cargo build fss_can_target_paced_rust cdylib"
    VERBATIM)

add_custom_target(fss_can_target_paced_rust ALL
    DEPENDS ${FSS_CAN_TARGET_PACED_RUST_LIB})
