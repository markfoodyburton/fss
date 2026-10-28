# protocols/config — config::param_request control carrier.
#
#  - param_request.jsonnet (committed) — carrier shape definition.
#  - config.h (generated)              — FSS_CONFIG_ID + FSS_CONFIG_META emitted
#    from the Jsonnet by reference/tools/fss_compile.py, includable as
#    fss/protocols/config.h (mirrors protocols/sim-events).

find_program(JSONNET_EXE jsonnet)
find_program(PYTHON3_EXE python3)
if(JSONNET_EXE AND PYTHON3_EXE)
    set(_cfg_shape ${CMAKE_SOURCE_DIR}/protocols/config/param_request.jsonnet)
    set(_gen_dir   ${CMAKE_CURRENT_BINARY_DIR}/gen/fss/protocols)
    set(_gen_hdr   ${_gen_dir}/config.h)
    add_custom_command(
        OUTPUT ${_gen_hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-c-header FSS_CONFIG ${_cfg_shape} ${_gen_hdr}
        DEPENDS ${_cfg_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_CONFIG_ID from param_request.jsonnet"
        VERBATIM)
    add_custom_target(fss-config-id-gen DEPENDS ${_gen_hdr})

    add_library(fss-config-id INTERFACE)
    add_dependencies(fss-config-id fss-config-id-gen)
    target_include_directories(fss-config-id INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/gen)

    set(FSS_CONFIG_ID_HEADER_AVAILABLE ON CACHE INTERNAL "config.h is generated")
else()
    message(STATUS "protocols/config: jsonnet/python3 not found — FSS_CONFIG_ID not generated")
endif()
