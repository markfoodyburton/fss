# fss_can_controller_queued_fmi — time-synced RELAY wrapping the can_controller FMU.
# Two timing peers (upstream initiator + downstream device); a software FIFO queues
# transmit requests from the inbound TLM port; the clock thread is the sole FMU
# driver and drains one request per DRAIN_EVERY quanta.  stop_on_detach = true:
# any peer detach cascades upstream + downstream via open_window to both peers.

if(NOT (FMI_HEADERS_DIR AND FMI_LS_BUS_HEADERS_DIR))
    message(STATUS "examples/fss_can_controller_queued_fmi: skipped — FMI headers not available")
    return()
endif()
if(NOT TARGET fss-fmi)
    message(STATUS "examples/fss_can_controller_queued_fmi: skipped — fss-fmi not built (languages/fmi)")
    return()
endif()
if(NOT TARGET fss-known-carrier-canbus)
    message(STATUS "examples/fss_can_controller_queued_fmi: skipped — canbus carrier headers not generated (FSS_FMICAN)")
    return()
endif()
if(NOT FSS_SIM_ID_HEADER_AVAILABLE OR NOT TARGET fss-sim-id)
    message(STATUS "examples/fss_can_controller_queued_fmi: skipped — FSS_SIM_ID header not generated")
    return()
endif()
if(NOT FSS_TIME_ID_HEADER_AVAILABLE OR NOT TARGET fss-time-id)
    message(STATUS "examples/fss_can_controller_queued_fmi: skipped — FSS_TIME_ID header not generated")
    return()
endif()

find_package(Threads REQUIRED)

add_library(fss_can_controller_queued_fmi MODULE can_fmi_ctrl_queued.c)
set_target_properties(fss_can_controller_queued_fmi PROPERTIES
    PREFIX ""
    OUTPUT_NAME fss_can_controller_queued_fmi
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
target_include_directories(fss_can_controller_queued_fmi PRIVATE ${FMI_LS_BUS_HEADERS_DIR})
target_link_libraries(fss_can_controller_queued_fmi PRIVATE
    fss-fmi
    fss-known-carrier-canbus
    fss-lang-c-meta
    fss-sim-id
    fss-time-id
    fss-abi
    Threads::Threads)
target_compile_definitions(fss_can_controller_queued_fmi PRIVATE
    FSS_FMU_PATH="$<TARGET_FILE:can_controller>")
add_dependencies(fss_can_controller_queued_fmi can_controller fss-canbus-id-gen fss-sim-id-gen fss-time-id-gen)
