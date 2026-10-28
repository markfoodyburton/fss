# languages/fmi — FMI 3.0 Co-Simulation binding for FSS.
#
# Provides fss-fmi: protocol- and carrier-agnostic FMI mechanics for
# importing and driving ONE Co-Simulation FMU (open/close + generic
# variable/clock/step accessors).  NOT an FMU exporter or SSP/system
# importer — see README.md §Scope.  Protocol-specific logic belongs in
# the example that uses this binding.
#
# Self-skips if FMI 3.0 headers weren't fetched.

if(NOT FMI_HEADERS_DIR)
    message(STATUS "languages/fmi: skipped — FMI_HEADERS_DIR not set")
    return()
endif()

add_library(fss-fmi STATIC fmi.c)

target_include_directories(fss-fmi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}    # fmi.h lives here
    ${FMI_HEADERS_DIR}
)

target_link_libraries(fss-fmi PUBLIC
    fss-abi
    ${CMAKE_DL_LIBS}
)

# This test does not require an FMU.  It verifies that NULL cannot make dlopen
# select the main program as an FMU library.
add_executable(fss_fmi_test test_fmi.c)
target_link_libraries(fss_fmi_test PRIVATE fss-fmi)
add_test(NAME fmi_null_path COMMAND fss_fmi_test)
