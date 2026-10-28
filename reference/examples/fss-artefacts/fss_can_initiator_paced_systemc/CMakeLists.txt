# fss_can_initiator_paced_systemc — the time-sync bench's CAN initiator: a
# kernel-bearing SystemC initiator that polls the controller's TX-FIFO free space
# (TLM READ) and paces its writes (TLM WRITE) against it, so the federation's
# simulated time is dominated by the queue/drain dynamics (the time-sync regime).

if(NOT COMMAND fss_systemc_tlm_add_module)
    message(STATUS "fss_can_initiator_paced_systemc: fss_systemc_tlm_add_module not available — skipping")
    return()
endif()
if(NOT FSS_TLM_ID_HEADER_AVAILABLE)
    message(STATUS "fss_can_initiator_paced_systemc: skipped — FSS_TLM_ID header not generated (needs jsonnet + python3)")
    return()
endif()

set(_SC_CAN_DIR "${CMAKE_SOURCE_DIR}/reference/examples/example-models/can_systemc")

fss_systemc_tlm_add_module(fss_can_initiator_paced_systemc
    SOURCES
        initiator_paced.cpp)

# The extracted can_initiator_paced.h header lives in the can_systemc dir.
target_include_directories(fss_can_initiator_paced_systemc PRIVATE
    "${_SC_CAN_DIR}")
