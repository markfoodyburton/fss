# transport_bench — hot-path send latency micro-benchmark for inproc + shm.
# Not a ctest; run manually:
#   transport_bench inproc <libfss_inproc.so> <libfss_minimal_echo.so> [N]
#   transport_bench shm    <libfss_shm.so>                     [N]

if(NOT (TARGET fss_inproc AND TARGET fss_shm AND TARGET fss_minimal_echo))
    message(STATUS "examples/tools/transport_bench skipped — needs fss_inproc + fss_shm + fss_minimal_echo")
    return()
endif()

add_executable(transport_bench transport_bench.c)

target_link_libraries(transport_bench PRIVATE
    ${CMAKE_DL_LIBS}
    fss-abi fss-placement)

set_target_properties(transport_bench PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

# Ensure plugin modules are built before the benchmark binary is run.
add_dependencies(transport_bench fss_inproc fss_shm fss_minimal_echo)

