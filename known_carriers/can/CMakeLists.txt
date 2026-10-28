# known_carriers/can — classic CAN frame carrier shape (FMI-LS-BUS CanTransmit).
# Canonical definition: frame.jsonnet + README.md.
#
# Generates fss/known_carriers/can_id.h (FSS_CAN_ID + FSS_CAN_META) from the
# Jsonnet composition so the id and canonical meta track it exactly.

find_program(JSONNET_EXE jsonnet)
find_program(PYTHON3_EXE python3)
if(JSONNET_EXE AND PYTHON3_EXE)
    set(_can_shape ${CMAKE_CURRENT_SOURCE_DIR}/frame.jsonnet)
    set(_gen_dir   ${CMAKE_CURRENT_BINARY_DIR}/gen/fss/known_carriers)
    set(_gen_hdr   ${_gen_dir}/can_id.h)
    add_custom_command(
        OUTPUT ${_gen_hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-c-header FSS_CAN ${_can_shape} ${_gen_hdr}
        DEPENDS ${_can_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_CAN_ID + FSS_CAN_META from frame.jsonnet"
        VERBATIM)
    add_custom_target(fss-can-id-gen DEPENDS ${_gen_hdr})

    # Flat-form Rust constants (FSS_CAN_FLAT_ID / _FLAT_META / _OFF_*) for a
    # non-C++ artefact that authors/consumes the can::frame carrier.
    set(_gen_rs ${_gen_dir}/can_id.rs)
    add_custom_command(
        OUTPUT ${_gen_rs}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-rust-flat FSS_CAN ${_can_shape} ${_gen_rs}
        DEPENDS ${_can_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_CAN flat Rust constants from frame.jsonnet"
        VERBATIM)
    add_custom_target(fss-can-id-rs-gen DEPENDS ${_gen_rs})
    set(FSS_CAN_FLAT_RS ${_gen_rs} CACHE INTERNAL "Path to generated can_id.rs")

    # Flat-form Python constants (can_flat.py) for a python-loader artefact,
    # into the same shared gen-py dir as tlm_flat.py.
    set(_gen_py_dir ${CMAKE_BINARY_DIR}/gen-py)
    set(_gen_py ${_gen_py_dir}/can_flat.py)
    add_custom_command(
        OUTPUT ${_gen_py}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_py_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-py-flat FSS_CAN ${_can_shape} ${_gen_py}
        DEPENDS ${_can_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_CAN flat Python constants from frame.jsonnet"
        VERBATIM)
    add_custom_target(fss-can-id-py-gen DEPENDS ${_gen_py})
    set(FSS_CAN_FLAT_PY ${_gen_py} CACHE INTERNAL "Path to generated can_flat.py")

    add_library(fss-known-carrier-can INTERFACE)
    add_dependencies(fss-known-carrier-can fss-can-id-gen)
    target_include_directories(fss-known-carrier-can INTERFACE
        ${CMAKE_CURRENT_BINARY_DIR}/gen)
    target_link_libraries(fss-known-carrier-can INTERFACE fss-abi)

    # Signal availability to consumers (probe, meta-resolver test, receiver).
    set(FSS_CAN_ID_HEADER_AVAILABLE ON CACHE INTERNAL "can_id.h is generated")
else()
    message(STATUS "fss-known-carrier-can: jsonnet/python3 not found — FSS_CAN_ID/META not generated")
endif()
