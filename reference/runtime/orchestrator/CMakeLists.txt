# runtime/orchestrator — ring-3 orchestrator scaffolding + shared transport
# plumbing (NON-NORMATIVE; see orchestrator.h).
#
# Compiled per-consumer via INTERFACE + target_sources (the fss-lang-c-meta
# convention), not a shared library.  Carries the fss-abi + control-protocol
# headers and the fss-sim-id include dir (fss_local_sim builds a sim:: carrier).

add_library(fss-orchestrator INTERFACE)
target_include_directories(fss-orchestrator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fss-orchestrator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/orchestrator.c)
target_link_libraries(fss-orchestrator INTERFACE fss-abi fss-control-protocol fss-lang-c-meta)
# The dlopen'able MODULE suffix, so a transport NAME resolves to libfss_<name><suffix>
# (".so" even on macOS for MODULE plugins).  Used by fss_transport_plugin_path.
target_compile_definitions(fss-orchestrator INTERFACE
    FSS_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}")
if(TARGET fss-sim-id)
    target_link_libraries(fss-orchestrator INTERFACE fss-sim-id)
endif()
if(TARGET fss-control-id)
    target_link_libraries(fss-orchestrator INTERFACE fss-control-id)
endif()

add_executable(fss_transport_abi_test test_transport_abi.c)
target_link_libraries(fss_transport_abi_test PRIVATE fss-abi)
set_target_properties(fss_transport_abi_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_test(NAME transport_abi_negotiation COMMAND fss_transport_abi_test)
