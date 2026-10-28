# fss_can_controller_queued_systemc — the time-sync bench's CAN controller: a
# kernel-bearing SystemC controller with a bounded TX FIFO drained on its own
# clock (WRITE=enqueue, READ=free-space poll, drain SC_THREAD emits can::frame).

if(NOT COMMAND fss_systemc_tlm_add_module)
    message(STATUS "fss_can_controller_queued_systemc: fss_systemc_tlm_add_module not available — skipping")
    return()
endif()
if(NOT FSS_TLM_ID_HEADER_AVAILABLE)
    message(STATUS "fss_can_controller_queued_systemc: skipped — FSS_TLM_ID header not generated (needs jsonnet + python3)")
    return()
endif()
if(NOT TARGET fss-known-carrier-canbus)
    message(STATUS "fss_can_controller_queued_systemc: skipped — fss-known-carrier-canbus not available (FSS_SCCAN_ID/META)")
    return()
endif()

set(_SC_CAN_DIR "${CMAKE_SOURCE_DIR}/reference/examples/example-models/can_systemc")

fss_systemc_tlm_add_module(fss_can_controller_queued_systemc
    SOURCES
        controller_queued.cpp)

# SystemC's own hop2 frame is sccan::frame (fss-known-carrier-canbus adds the include dir).
# Also need the can_systemc dir for the extracted can_controller_queued.h header.
target_include_directories(fss_can_controller_queued_systemc PRIVATE
    "${_SC_CAN_DIR}"
    $<TARGET_PROPERTY:fss-known-carrier-canbus,INTERFACE_INCLUDE_DIRECTORIES>)
add_dependencies(fss_can_controller_queued_systemc fss-canbus-id-gen)
