# fss_load_probe — host-program demo of the in-proc transport.  Loads any
# FSS artefact .dylib, drives the bind-then-send flow (bind + sim/Started/Stopped),
# and exercises the three receive paths plus reject.  Doubles as the smoke test
# for fss_spi_memory_systemc (ctest: fss_spi_memory_systemc_load).

# load_probe.cpp includes the generated fss/known_carriers/tlm_id.h and
# fss/protocols/sim.h; it builds a real tlm_generic_payload for the
# Path-1 cast test, so needs all generated headers.
if(NOT FSS_TLM_ID_HEADER_AVAILABLE)
    message(STATUS "fss_load_probe skipped — FSS_TLM_ID header not generated (needs jsonnet + python3)")
    return()
endif()
if(NOT FSS_SIM_ID_HEADER_AVAILABLE)
    message(STATUS "fss_load_probe skipped — FSS_SIM_ID header not generated (needs jsonnet + python3)")
    return()
endif()

if(NOT TARGET SystemC::systemc)
    find_package(SystemCLanguage CONFIG QUIET)
endif()
if(NOT TARGET SystemC::systemc)
    message(STATUS "fss_load_probe skipped — SystemC not found (set FSS_FETCH_SYSTEMC=ON)")
    return()
endif()

# Entry point is sc_main (SystemC owns main); the probe never calls sc_start.
add_executable(fss_load_probe load_probe.cpp)

target_link_libraries(fss_load_probe PRIVATE
    ${CMAKE_DL_LIBS} fss-known-carrier-tlm fss-lang-c-meta fss-placement SystemC::systemc)

target_include_directories(fss_load_probe PRIVATE
    $<TARGET_PROPERTY:fss-sim-id,INTERFACE_INCLUDE_DIRECTORIES>)

add_dependencies(fss_load_probe fss-tlm-id-gen fss-sim-id-gen fss_inproc fss_spi_memory_systemc)

set_target_properties(fss_load_probe PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

# Smoke test for the fss_spi_memory_systemc artefact: load it via the in-proc
# transport and drive carriers through it.  Registered here (not in the
# artefact's own CMakeLists) because tools/ is configured after fss-artefacts/,
# so both fss_load_probe and fss_spi_memory_systemc targets exist at this point.
if(TARGET fss_spi_memory_systemc)
    enable_testing()
    add_test(NAME fss_spi_memory_systemc_load
             COMMAND $<TARGET_FILE:fss_load_probe>
                     $<TARGET_FILE:fss_inproc>
                     $<TARGET_FILE:fss_spi_memory_systemc>)
endif()

