# fss_can_initiator_paced_fmi — time-synced FMI CAN source (wraps can_initiator FMU).
# The clock thread paces emission to the federation quantum (source-role flavour:
# no FMU step clamp — the FMU is driven at its natural FMU_STEP_SIZE, decoupled
# from now_ps).  stop_on_detach = true so a downstream detach cascades upstream.

if(NOT (FMI_HEADERS_DIR AND FMI_LS_BUS_HEADERS_DIR))
    message(STATUS "examples/fss_can_initiator_paced_fmi: skipped — FMI headers not available")
    return()
endif()
if(NOT TARGET fss-fmi)
    message(STATUS "examples/fss_can_initiator_paced_fmi: skipped — fss-fmi not built (languages/fmi)")
    return()
endif()
if(NOT TARGET fss-known-carrier-canbus)
    message(STATUS "examples/fss_can_initiator_paced_fmi: skipped — canbus carrier headers not generated (FSS_FMIREG)")
    return()
endif()
if(NOT FSS_CONFIG_ID_HEADER_AVAILABLE OR NOT TARGET fss-config-id)
    message(STATUS "examples/fss_can_initiator_paced_fmi: skipped — FSS_CONFIG_ID header not generated")
    return()
endif()
if(NOT FSS_SIM_ID_HEADER_AVAILABLE OR NOT TARGET fss-sim-id)
    message(STATUS "examples/fss_can_initiator_paced_fmi: skipped — FSS_SIM_ID header not generated")
    return()
endif()
if(NOT FSS_TIME_ID_HEADER_AVAILABLE OR NOT TARGET fss-time-id)
    message(STATUS "examples/fss_can_initiator_paced_fmi: skipped — FSS_TIME_ID header not generated")
    return()
endif()

find_package(Threads REQUIRED)

add_library(fss_can_initiator_paced_fmi MODULE can_fmi_tx_paced.c)
set_target_properties(fss_can_initiator_paced_fmi PROPERTIES
    PREFIX ""
    OUTPUT_NAME fss_can_initiator_paced_fmi
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
target_include_directories(fss_can_initiator_paced_fmi PRIVATE ${FMI_LS_BUS_HEADERS_DIR})
target_link_libraries(fss_can_initiator_paced_fmi PRIVATE
    fss-fmi
    fss-known-carrier-canbus
    fss-lang-c-meta
    fss-config-id
    fss-sim-id
    fss-time-id
    fss-abi
    Threads::Threads)
target_compile_definitions(fss_can_initiator_paced_fmi PRIVATE
    FSS_FMU_PATH="$<TARGET_FILE:can_initiator>")
add_dependencies(fss_can_initiator_paced_fmi can_initiator fss-canbus-id-gen fss-config-id-gen fss-sim-id-gen fss-time-id-gen)
