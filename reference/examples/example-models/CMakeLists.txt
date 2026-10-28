# example-models — FSS-INDEPENDENT models, harnesses, and devices.
#
# Ordinary SystemC / FMI / Python / Rust models that build and run on their own,
# with no dependency on FSS.  They are the corpus an FSS artefact later wraps
# (see ../fss-artefacts) and that federations wire together (see ../federations).
# Each self-skips cleanly if its toolchain is absent.

# The SystemC TLM SPI models (spi_initiator_systemc/, spi_memory_systemc/) have no
# standalone CMake target: their source is compiled by the consumers that need it
# (the FSS artefacts in ../fss-artefacts).  No add_subdirectory — there is nothing
# to build on their own.

# Clean, FSS-independent CAN corpus consumed by the paced/queued FSS artefacts:
# can_systemc provides can_target.h (the SC paced device's base); can_fmi
# provides the FMU targets the paced FMI artefacts wrap.  The standalone Rust
# and Python CAN corpus was removed with the plain artefacts — the paced Rust
# and Python models are self-contained (generated flat constants only).
if(FMI_HEADERS_DIR AND FMI_LS_BUS_HEADERS_DIR)
    add_subdirectory(can_fmi)
else()
    message(STATUS "examples/example-models/can_fmi skipped — set FSS_FETCH_FMI=ON (needs FMI 3.0 + LS-BUS headers)")
endif()

add_subdirectory(can_systemc)  # self-skips if SystemC absent
