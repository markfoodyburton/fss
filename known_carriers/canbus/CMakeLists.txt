# known_carriers/canbus — the framework-native CAN-bus carriers (R12).
#
# Eight carriers demonstrate the reuse thesis end to end: each framework emits its
# OWN native carrier on the hop where it is the sender (hop1 register-write into
# the controller; hop2 CAN frame to the device), ALL composed from the shared
# bus:: atoms, so any-language receiver reads any-language sender by meta (qname),
# with NO bridge.  SystemC's hop1 carrier is the existing tlm::transaction
# (known_carriers/tlm); the seven authored here are the rest.
#
# For each carrier we emit all three id/offset forms (C header, rust-flat, py-flat)
# so the AUTHORING language has its offset macros; a CONSUMING language needs none
# (it meta-resolves bus:: by qname).  Emitting all three is cheap and uniform.

find_program(JSONNET_EXE jsonnet)
find_program(PYTHON3_EXE python3)
if(JSONNET_EXE AND PYTHON3_EXE)
    set(_gen_dir    ${CMAKE_CURRENT_BINARY_DIR}/gen/fss/known_carriers)
    set(_gen_py_dir ${CMAKE_BINARY_DIR}/gen-py)

    # carrier : PREFIX  (PREFIX drives the emitted macro names, e.g. FSS_FMIREG_ID)
    set(_carriers
        "fmireg_write:FSS_FMIREG"
        "rustreg_write:FSS_RUSTREG"
        "pyreg_write:FSS_PYREG"
        "sccan_frame:FSS_SCCAN"
        "fmican_frame:FSS_FMICAN"
        "rustcan_frame:FSS_RUSTCAN"
        "pycan_frame:FSS_PYCAN")

    set(_canbus_c_headers "")
    foreach(_entry ${_carriers})
        string(REPLACE ":" ";" _pair ${_entry})
        list(GET _pair 0 _stem)
        list(GET _pair 1 _prefix)
        set(_shape ${CMAKE_CURRENT_SOURCE_DIR}/${_stem}.jsonnet)

        set(_c   ${_gen_dir}/${_stem}.h)
        add_custom_command(OUTPUT ${_c}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                    --emit-c-header ${_prefix} ${_shape} ${_c}
            DEPENDS ${_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
            COMMENT "Generating ${_prefix}_ID from ${_stem}.jsonnet" VERBATIM)
        list(APPEND _canbus_c_headers ${_c})

        set(_rs ${_gen_dir}/${_stem}.rs)
        add_custom_command(OUTPUT ${_rs}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                    --emit-rust-flat ${_prefix} ${_shape} ${_rs}
            DEPENDS ${_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
            COMMENT "Generating ${_prefix} flat Rust constants" VERBATIM)
        list(APPEND _canbus_c_headers ${_rs})

        set(_py ${_gen_py_dir}/${_stem}.py)
        add_custom_command(OUTPUT ${_py}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_py_dir}
            COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                    --emit-py-flat ${_prefix} ${_shape} ${_py}
            DEPENDS ${_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
            COMMENT "Generating ${_prefix} flat Python constants" VERBATIM)
        list(APPEND _canbus_c_headers ${_py})

        # Expose the generated paths to artefacts by convention:
        #   <PREFIX>_RS / <PREFIX>_PY cache vars (PREFIX already carries the FSS_
        #   stem, e.g. FSS_RUSTREG_RS — mirrors FSS_CAN_FLAT_RS/PY).
        set(${_prefix}_RS ${_rs} CACHE INTERNAL "flat Rust for ${_stem}")
        set(${_prefix}_PY ${_py} CACHE INTERNAL "flat Python for ${_stem}")
    endforeach()

    add_custom_target(fss-canbus-id-gen DEPENDS ${_canbus_c_headers})

    add_library(fss-known-carrier-canbus INTERFACE)
    add_dependencies(fss-known-carrier-canbus fss-canbus-id-gen)
    target_include_directories(fss-known-carrier-canbus INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/gen)
    target_link_libraries(fss-known-carrier-canbus INTERFACE fss-abi)

    set(FSS_CANBUS_ID_HEADERS_AVAILABLE ON CACHE INTERNAL "canbus carrier headers generated")
else()
    message(STATUS "fss-known-carrier-canbus: jsonnet/python3 not found — canbus carriers not generated")
endif()
