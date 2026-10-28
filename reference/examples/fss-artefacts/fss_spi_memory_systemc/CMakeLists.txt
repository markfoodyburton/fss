# fss_spi_memory_systemc — FSS-binding wrap of the spi_memory_systemc model.

if(NOT COMMAND fss_systemc_tlm_add_module)
    message(STATUS "fss_spi_memory_systemc: fss_systemc_tlm_add_module not available — skipping")
    return()
endif()

# The SystemC artefact's receiver (artefact.h) includes the generated
# fss/known_carriers/tlm_id.h for the Path-A id check / Path-B resolver.
if(NOT FSS_TLM_ID_HEADER_AVAILABLE)
    message(STATUS "fss_spi_memory_systemc: skipped — FSS_TLM_ID header not generated (needs jsonnet + python3)")
    return()
endif()

set(_SC_SPI_MEM_DIR "${CMAKE_SOURCE_DIR}/reference/examples/example-models/spi_memory_systemc")

fss_systemc_tlm_add_module(fss_spi_memory_systemc
    SOURCES
        module_main.cpp
        "${_SC_SPI_MEM_DIR}/spi_memory_systemc.cpp")

target_include_directories(fss_spi_memory_systemc PRIVATE
    "${_SC_SPI_MEM_DIR}")

# The fss_spi_memory_systemc_load smoke test (driving this artefact with
# fss_load_probe) is registered in ../../tools/fss_load_probe, because that
# bucket is configured after this one — so the fss_load_probe target exists
# there but not here.

