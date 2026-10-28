# known_carriers/tlm — TLM carrier shape (IEEE 1666.1 §6).
# Canonical definition: transaction.jsonnet + README.md.
#
# Generates fss/known_carriers/tlm_id.h (FSS_TLM_ID + FSS_TLM_META) from the
# Jsonnet composition so the id and canonical meta track it exactly.

find_program(JSONNET_EXE jsonnet)
find_program(PYTHON3_EXE python3)
if(JSONNET_EXE AND PYTHON3_EXE)
    set(_tlm_shape ${CMAKE_CURRENT_SOURCE_DIR}/transaction.jsonnet)
    set(_gen_dir   ${CMAKE_CURRENT_BINARY_DIR}/gen/fss/known_carriers)
    set(_gen_hdr   ${_gen_dir}/tlm_id.h)
    add_custom_command(
        OUTPUT ${_gen_hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-c-header FSS_TLM ${_tlm_shape} ${_gen_hdr}
        DEPENDS ${_tlm_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_TLM_ID + FSS_TLM_META from transaction.jsonnet"
        VERBATIM)

    add_custom_target(fss-tlm-id-gen DEPENDS ${_gen_hdr})

    # Flat-form Rust constants (FSS_TLM_FLAT_ID / _FLAT_META / _OFF_*) for a
    # non-C++ artefact that authors/consumes the flat tlm::transaction carrier
    # (the wire form a transport derives from the live GP — see fss_meta_flatten).
    set(_gen_rs ${_gen_dir}/tlm_id.rs)
    add_custom_command(
        OUTPUT ${_gen_rs}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-rust-flat FSS_TLM ${_tlm_shape} ${_gen_rs}
        DEPENDS ${_tlm_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_TLM flat Rust constants from transaction.jsonnet"
        VERBATIM)
    add_custom_target(fss-tlm-id-rs-gen DEPENDS ${_gen_rs})
    set(FSS_TLM_FLAT_RS ${_gen_rs} CACHE INTERNAL "Path to generated tlm_id.rs")

    # Flat-form Python constants (tlm_flat.py) for a python-loader artefact.
    # Emitted into a shared gen-py dir so one FSS_PY_GEN_DIR covers tlm + can.
    set(_gen_py_dir ${CMAKE_BINARY_DIR}/gen-py)
    set(_gen_py ${_gen_py_dir}/tlm_flat.py)
    add_custom_command(
        OUTPUT ${_gen_py}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_py_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-py-flat FSS_TLM ${_tlm_shape} ${_gen_py}
        DEPENDS ${_tlm_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_TLM flat Python constants from transaction.jsonnet"
        VERBATIM)
    add_custom_target(fss-tlm-id-py-gen DEPENDS ${_gen_py})
    set(FSS_FLAT_PY_DIR ${_gen_py_dir} CACHE INTERNAL "Dir of generated flat-carrier Python constants")
    set(FSS_TLM_FLAT_PY ${_gen_py} CACHE INTERNAL "Path to generated tlm_flat.py")

    add_library(fss-known-carrier-tlm INTERFACE)
    add_dependencies(fss-known-carrier-tlm fss-tlm-id-gen)
    target_include_directories(fss-known-carrier-tlm INTERFACE
        ${CMAKE_CURRENT_BINARY_DIR}/gen)
    target_link_libraries(fss-known-carrier-tlm INTERFACE fss-abi)

    # Signal availability to consumers (probe, meta-resolver test, receiver).
    set(FSS_TLM_ID_HEADER_AVAILABLE ON CACHE INTERNAL "tlm_id.h is generated")
else()
    message(STATUS "fss-known-carrier-tlm: jsonnet/python3 not found — FSS_TLM_ID/META not generated")
endif()
