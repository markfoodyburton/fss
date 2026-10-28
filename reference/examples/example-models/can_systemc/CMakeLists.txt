# examples/can_systemc — clean, FSS-independent SystemC CAN target + initiator.
#
# The SystemC member of the CAN-target corpus (alongside FMI, Python, Rust).
# Pure SystemC, no FSS, no CCI.

if(NOT TARGET SystemC::systemc)
    find_package(SystemCLanguage CONFIG QUIET)
endif()

if(NOT TARGET SystemC::systemc)
    message(STATUS "examples/can_systemc skipped — SystemC not found (set FSS_FETCH_SYSTEMC=ON)")
    return()
endif()

# "SystemC on its own, no FSS" baseline: the SAME three paced/queued CAN models
# as the combined FSS artefact, one kernel, wired with NATIVE TLM binds (no FSS,
# no CCI).  The perf run is manual (pass a frame count); the ctest checks only
# correctness at the default count.
#
# PAPER-ONLY: this native baseline exists to back the FSS-vs-native perf figure.
# Delete it (target + test) once the 2026 paper is submitted.
add_executable(sc_can_native_bench can_native_bench.cpp)
target_include_directories(sc_can_native_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sc_can_native_bench PRIVATE SystemC::systemc)

add_test(NAME can_systemc_native_bench COMMAND sc_can_native_bench)
