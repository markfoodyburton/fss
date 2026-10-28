# tools — drive or measure a single artefact (not multi-component federations).
#
#   fss_load_probe   host driver: loads one FSS artefact and drives carriers
#                    through it (also the smoke test for fss_spi_memory_systemc).
#   transport_bench  send-latency micro-benchmark for the transport plugins.

if(TARGET fss_inproc)
    add_subdirectory(fss_load_probe)   # self-skips further on SystemC / generated headers
    add_subdirectory(transport_bench)  # self-skips further on fss_shm / fss_minimal_echo
else()
    message(STATUS "examples/tools/fss_load_probe skipped — fss_inproc not built")
    message(STATUS "examples/tools/transport_bench skipped — fss_inproc not built")
endif()

# Python language-binding interop test (languages/python): a Python host drives
# the in-proc transport + fss_minimal_echo artefact via ctypes.  Registered here
# (not in languages/python) because tools/ is configured after the artefact
# exists.  Self-skips if python3 or the targets are absent.
find_program(PYTHON3_EXE python3)
if(PYTHON3_EXE AND TARGET fss_inproc AND TARGET fss_minimal_echo)
    enable_testing()
    add_test(NAME python_binding
             COMMAND ${PYTHON3_EXE}
                     ${CMAKE_SOURCE_DIR}/reference/languages/python/selftest.py
                     $<TARGET_FILE:fss_inproc>
                     $<TARGET_FILE:fss_minimal_echo>)
endif()
