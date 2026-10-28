# fss_can_target_paced_systemc — the time-sync bench's CAN device: the clean
# can_target plus a free-running heartbeat SC_THREAD so it has its own clock and
# participates in time-sync (making the controller a two-timing-peer node).

if(NOT COMMAND fss_systemc_tlm_add_module)
    message(STATUS "fss_can_target_paced_systemc: fss_systemc_tlm_add_module not available — skipping")
    return()
endif()
if(NOT FSS_TLM_ID_HEADER_AVAILABLE)
    message(STATUS "fss_can_target_paced_systemc: skipped — FSS_TLM_ID header not generated (needs jsonnet + python3)")
    return()
endif()

set(_SC_CAN_DIR "${CMAKE_SOURCE_DIR}/reference/examples/example-models/can_systemc")

fss_systemc_tlm_add_module(fss_can_target_paced_systemc
    SOURCES
        target_paced.cpp)

target_include_directories(fss_can_target_paced_systemc PRIVATE
    "${_SC_CAN_DIR}")
