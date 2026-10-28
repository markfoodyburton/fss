# runtime/agent-core — the generic agent (NON-NORMATIVE; see agent_core.h).
#
# Compiled per-consumer via INTERFACE + target_sources.  The one per-language
# seam is the model-loader vtable; agent-core knows only that.  Links the
# orchestrator (shared plumbing + taps), the control protocol, and placement.

add_library(fss-agent-core INTERFACE)
target_include_directories(fss-agent-core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fss-agent-core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/agent_core.c)
target_link_libraries(fss-agent-core INTERFACE fss-abi fss-control-protocol fss-orchestrator fss-placement)
