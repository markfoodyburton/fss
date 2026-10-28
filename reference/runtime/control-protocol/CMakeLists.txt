# runtime/control-protocol — the private orchestrator<->agent control language.
#
# Header-only and NON-NORMATIVE (see the banner in control_protocol.h): the
# instruction vocabulary an orchestrator/agent pair use to realise a topology.
# Not an FSS standard carrier, not an FSS ABI.

add_library(fss-control-protocol INTERFACE)
target_include_directories(fss-control-protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
