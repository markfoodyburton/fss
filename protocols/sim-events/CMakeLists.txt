# protocols/sim-events — fss::sim::event control carrier.
#
# Two parts:
#  - event.jsonnet (committed) — carrier shape definition.
#  - sim.h (generated)        — FSS_SIM_ID emitted from the Jsonnet composition
#    by reference/tools/fss_compile.py, includable as fss/protocols/sim.h.

find_program(JSONNET_EXE jsonnet)
find_program(PYTHON3_EXE python3)
if(JSONNET_EXE AND PYTHON3_EXE)
    set(_sim_shape ${CMAKE_SOURCE_DIR}/protocols/sim-events/event.jsonnet)
    set(_gen_dir    ${CMAKE_CURRENT_BINARY_DIR}/gen/fss/protocols)
    set(_gen_hdr    ${_gen_dir}/sim.h)
    add_custom_command(
        OUTPUT ${_gen_hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-c-header FSS_SIM ${_sim_shape} ${_gen_hdr}
        DEPENDS ${_sim_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_SIM_ID from event.jsonnet"
        VERBATIM)
    add_custom_target(fss-sim-id-gen DEPENDS ${_gen_hdr})

    add_library(fss-sim-id INTERFACE)
    add_dependencies(fss-sim-id fss-sim-id-gen)
    target_include_directories(fss-sim-id INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/gen)

    # Signal availability to consumers.
    set(FSS_SIM_ID_HEADER_AVAILABLE ON CACHE INTERNAL "sim.h is generated")
else()
    message(STATUS "protocols/sim-events: jsonnet/python3 not found — FSS_SIM_ID not generated")
endif()
