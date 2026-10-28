# fss_can_target_paced_fmi — time-synced FMI CAN device (wraps the can_target
# FMU).  Adds a native-C windowed clock to the FMI receiver: the clock thread is
# the sole FMU driver and steps the FMU with a CAPPED dt (the FMU step clamp), so
# the node honours the time::window protocol on fss/time.  Self-skips like the
# plain FMI target.

if(NOT (FMI_HEADERS_DIR AND FMI_LS_BUS_HEADERS_DIR))
    message(STATUS "examples/fss_can_target_paced_fmi: skipped — FMI headers not available")
    return()
endif()
if(NOT TARGET fss-fmi)
    message(STATUS "examples/fss_can_target_paced_fmi: skipped — fss-fmi not built (languages/fmi)")
    return()
endif()
if(NOT FSS_CONFIG_ID_HEADER_AVAILABLE OR NOT TARGET fss-config-id)
    message(STATUS "examples/fss_can_target_paced_fmi: skipped — FSS_CONFIG_ID header not generated")
    return()
endif()
if(NOT FSS_SIM_ID_HEADER_AVAILABLE OR NOT TARGET fss-sim-id)
    message(STATUS "examples/fss_can_target_paced_fmi: skipped — FSS_SIM_ID header not generated")
    return()
endif()
if(NOT FSS_TIME_ID_HEADER_AVAILABLE OR NOT TARGET fss-time-id)
    message(STATUS "examples/fss_can_target_paced_fmi: skipped — FSS_TIME_ID header not generated")
    return()
endif()

find_package(Threads REQUIRED)

add_library(fss_can_target_paced_fmi MODULE can_fmi_rx_paced.c)
set_target_properties(fss_can_target_paced_fmi PROPERTIES
    PREFIX ""
    OUTPUT_NAME fss_can_target_paced_fmi)
target_include_directories(fss_can_target_paced_fmi PRIVATE ${FMI_LS_BUS_HEADERS_DIR})
# R12: this device is a pure CONSUMER — it meta-resolves any CAN carrier by qname,
# so it needs the C meta walker (fss-lang-c-meta) but NO specific carrier header.
target_link_libraries(fss_can_target_paced_fmi PRIVATE
    fss-fmi fss-lang-c-meta fss-config-id fss-sim-id fss-time-id fss-abi Threads::Threads)
target_compile_definitions(fss_can_target_paced_fmi PRIVATE
    FSS_FMU_PATH="$<TARGET_FILE:can_target>")
add_dependencies(fss_can_target_paced_fmi can_target fss-config-id-gen fss-sim-id-gen fss-time-id-gen)
