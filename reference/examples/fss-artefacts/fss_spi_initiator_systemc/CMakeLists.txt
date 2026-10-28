# fss_spi_initiator_systemc — FSS-binding wrap of the heavyweight spi_initiator
# (CCI/DMI benchmark). Drives the SPI chain as a benchmark federation.

if(NOT COMMAND fss_systemc_tlm_add_module)
    message(STATUS "fss_spi_initiator_systemc: fss_systemc_tlm_add_module not available — skipping")
    return()
endif()

if(NOT FSS_TLM_ID_HEADER_AVAILABLE)
    message(STATUS "fss_spi_initiator_systemc: skipped — FSS_TLM_ID header not generated (needs jsonnet + python3)")
    return()
endif()

set(_SC_SPI_INI_DIR "${CMAKE_SOURCE_DIR}/reference/examples/example-models/spi_initiator_systemc")

fss_systemc_tlm_add_module(fss_spi_initiator_systemc
    SOURCES
        module_main.cpp
        "${_SC_SPI_INI_DIR}/spi_initiator_systemc.cpp")

target_include_directories(fss_spi_initiator_systemc PRIVATE
    "${_SC_SPI_INI_DIR}")
