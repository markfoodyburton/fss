# protocols/control — control::instr carrier.
#
#  - instr.jsonnet (committed) — carrier shape definition.
#  - control.h (generated)    — FSS_CONTROL_ID + FSS_CONTROL_META emitted
#    from the Jsonnet by reference/tools/fss_compile.py, includable as
#    fss/protocols/control.h (mirrors protocols/sim-events).

find_program(JSONNET_EXE jsonnet)
find_program(PYTHON3_EXE python3)
if(JSONNET_EXE AND PYTHON3_EXE)
    set(_ctl_shape  ${CMAKE_SOURCE_DIR}/protocols/control/instr.jsonnet)
    set(_plan_shape ${CMAKE_SOURCE_DIR}/protocols/control/plan.jsonnet)
    set(_gen_dir    ${CMAKE_CURRENT_BINARY_DIR}/gen/fss/protocols)
    set(_gen_hdr    ${_gen_dir}/control.h)
    set(_gen_plan   ${_gen_dir}/control_plan.h)
    add_custom_command(
        OUTPUT ${_gen_hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-c-header FSS_CONTROL ${_ctl_shape} ${_gen_hdr}
        DEPENDS ${_ctl_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_CONTROL_ID from instr.jsonnet"
        VERBATIM)
    add_custom_command(
        OUTPUT ${_gen_plan}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-c-header FSS_CONTROL_PLAN ${_plan_shape} ${_gen_plan}
        DEPENDS ${_plan_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_CONTROL_PLAN_ID from plan.jsonnet"
        VERBATIM)
    add_custom_target(fss-control-id-gen DEPENDS ${_gen_hdr} ${_gen_plan})

    add_library(fss-control-id INTERFACE)
    add_dependencies(fss-control-id fss-control-id-gen)
    target_include_directories(fss-control-id INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/gen)

    set(FSS_CONTROL_ID_HEADER_AVAILABLE ON CACHE INTERNAL "control.h is generated")
else()
    message(STATUS "protocols/control: jsonnet/python3 not found — FSS_CONTROL_ID not generated")
endif()
