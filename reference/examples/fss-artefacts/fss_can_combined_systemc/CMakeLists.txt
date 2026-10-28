# fss_can_combined_systemc — single-kernel artefact that builds all three CAN models
# (initiator + controller + device) inside ONE SystemC kernel, wired internally via
# FSS endpoints so same-kernel sends hit the SAME-THREAD FAST PATH (no doorbell).
# Demonstrates: one artefact builds multiple models; FSS is ~free when co-located.

if(NOT COMMAND fss_systemc_tlm_add_module)
    message(STATUS "fss_can_combined_systemc: fss_systemc_tlm_add_module not available — skipping")
    return()
endif()
if(NOT FSS_TLM_ID_HEADER_AVAILABLE)
    message(STATUS "fss_can_combined_systemc: skipped — FSS_TLM_ID header not generated (needs jsonnet + python3)")
    return()
endif()
if(NOT TARGET fss-known-carrier-canbus)
    message(STATUS "fss_can_combined_systemc: skipped — fss-known-carrier-canbus not available (FSS_SCCAN_ID/META)")
    return()
endif()

set(_SC_CAN_DIR "${CMAKE_SOURCE_DIR}/reference/examples/example-models/can_systemc")

fss_systemc_tlm_add_module(fss_can_combined_systemc
    SOURCES
        combined.cpp)

# The hub includes the three model headers from can_systemc; the controller's
# outbound uses sccan::frame (fss-known-carrier-canbus adds the include dir).
target_include_directories(fss_can_combined_systemc PRIVATE
    "${_SC_CAN_DIR}"
    $<TARGET_PROPERTY:fss-known-carrier-canbus,INTERFACE_INCLUDE_DIRECTORIES>)
add_dependencies(fss_can_combined_systemc fss-canbus-id-gen)
