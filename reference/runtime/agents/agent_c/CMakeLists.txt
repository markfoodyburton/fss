# runtime/agents/agent_c — the fss-agent-c binary (C / SystemC / Rust models).
#
# The agent is the loadable unit: agent-core + the cdylib model loader + a thin
# main that parses born-with off argv.  The launcher execv's this binary.

if(NOT TARGET fss_inproc)
    return()
endif()
if(NOT FSS_SIM_ID_HEADER_AVAILABLE)
    message(STATUS "runtime/agents/agent_c skipped — FSS_SIM_ID header not generated")
    return()
endif()

add_executable(fss-agent-c main.c)
target_link_libraries(fss-agent-c PRIVATE fss-agent-core fss-model-cdylib ${CMAKE_DL_LIBS})
set_target_properties(fss-agent-c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
# agent-core compiles orchestrator.c, which includes the generated sim.h.
add_dependencies(fss-agent-c fss-sim-id-gen)
