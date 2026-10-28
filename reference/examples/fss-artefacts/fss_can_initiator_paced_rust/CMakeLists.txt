# fss_can_initiator_paced_rust — time-synced paced Rust CAN initiator (cdylib FSS artefact).
#
# Rust port of fss_can_initiator_paced_systemc: paces one rustreg::write register
# WRITE per quantum via TimeSync::on_tick, with Status-readback back-pressure.
# Emits its OWN native hop1 carrier, so it needs the generated rustreg::write Rust
# constants (FSS_RUSTREG_RS).

find_program(CARGO_EXE cargo)
if(NOT CARGO_EXE)
    message(STATUS "examples/fss-artefacts/fss_can_initiator_paced_rust skipped — cargo not found")
    return()
endif()
if(NOT FSS_RUSTREG_RS)
    message(STATUS "examples/fss-artefacts/fss_can_initiator_paced_rust skipped — rustreg::write Rust constants not generated (jsonnet/python3)")
    return()
endif()

set(RUST_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/target)

if(APPLE)
    set(_RUST_LIB_NAME libfss_can_initiator_paced_rust.dylib)
elseif(WIN32)
    set(_RUST_LIB_NAME fss_can_initiator_paced_rust.dll)
else()
    set(_RUST_LIB_NAME libfss_can_initiator_paced_rust.so)
endif()

set(RUST_CDYLIB_OUTPUT ${RUST_TARGET_DIR}/release/${_RUST_LIB_NAME})
set(FSS_CAN_INITIATOR_PACED_RUST_LIB ${CMAKE_CURRENT_BINARY_DIR}/${_RUST_LIB_NAME}
    CACHE INTERNAL "Path to the fss_can_initiator_paced_rust cdylib")

add_custom_command(
    OUTPUT ${FSS_CAN_INITIATOR_PACED_RUST_LIB}
    COMMAND ${CMAKE_COMMAND} -E env FSS_RUSTREG_RS=${FSS_RUSTREG_RS}
            ${CARGO_EXE} build --offline --release
            --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
            --target-dir ${RUST_TARGET_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy
            ${RUST_CDYLIB_OUTPUT} ${FSS_CAN_INITIATOR_PACED_RUST_LIB}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
        ${FSS_RUSTREG_RS}
        ${CMAKE_SOURCE_DIR}/reference/languages/rust/src/lib.rs
    COMMENT "cargo build fss_can_initiator_paced_rust cdylib"
    VERBATIM)

add_custom_target(fss_can_initiator_paced_rust ALL
    DEPENDS ${FSS_CAN_INITIATOR_PACED_RUST_LIB})
add_dependencies(fss_can_initiator_paced_rust fss-canbus-id-gen)
