# fss-artefacts — FSS-WRAPPED, dlopen-loadable artefacts.
#
# Each exports the artefact ABI (fss_create / fss_get_endpoint / fss_destroy)
# and is loaded by a transport's open().  Most wrap an FSS-independent model
# from ../example-models (1-1 where a model exists); fss_minimal_echo is an
# FSS-native test fixture with no standalone model.  See ../README.md for the
# mapping.

if(TARGET fss_inproc)
    add_subdirectory(fss_minimal_echo)
else()
    message(STATUS "examples/fss-artefacts/fss_minimal_echo skipped — fss_inproc not built")
endif()

# Python CAN artefacts as dlopen'd artefacts (via the CPython-embedding loader):
# the paced/queued time-synced trio the platform matrix drives.
add_subdirectory(fss_can_target_paced_python) # time-sync bench: Python device w/ invented clock
add_subdirectory(fss_can_initiator_paced_python)    # time-sync bench: paced Python initiator
add_subdirectory(fss_can_controller_queued_python)  # time-sync bench: bounded-FIFO draining Python controller

# SystemC artefacts: wrap the example-models SystemC TLM building blocks.
if(FSS_FETCH_SYSTEMC OR TARGET systemc)
    add_subdirectory(fss_spi_memory_systemc)      # target (4 KiB RAM)
    add_subdirectory(fss_spi_initiator_systemc)   # heavyweight CCI/DMI initiator
    add_subdirectory(fss_can_initiator_paced_systemc)     # time-sync bench: paced/polling initiator
    add_subdirectory(fss_can_controller_queued_systemc)   # time-sync bench: bounded-FIFO draining controller
    add_subdirectory(fss_can_target_paced_systemc)        # time-sync bench: free-running (heartbeat) device
    add_subdirectory(fss_can_combined_systemc)            # combined: all three CAN models in one kernel
else()
    message(STATUS "examples/fss-artefacts/fss_*_systemc skipped — set FSS_FETCH_SYSTEMC=ON")
endif()

# Rust cdylib FSS artefacts: the paced/queued time-synced CAN trio.
find_program(CARGO_EXE cargo)
if(CARGO_EXE)
    add_subdirectory(fss_can_target_paced_rust) # time-sync bench: Rust device w/ invented clock
    add_subdirectory(fss_can_initiator_paced_rust)   # time-sync bench: paced/polling Rust initiator
    add_subdirectory(fss_can_controller_queued_rust) # time-sync bench: bounded-FIFO draining Rust controller
else()
    message(STATUS "examples/fss-artefacts/fss_can_{target,initiator,controller}_rust skipped — cargo not found")
endif()

# FSS artefacts wrapping the can_fmi FMUs (need the fmi binding): the paced/queued
# time-synced CAN trio, each driving its FMU under the windowed clock.
if(FMI_HEADERS_DIR AND FMI_LS_BUS_HEADERS_DIR)
    add_subdirectory(fss_can_target_paced_fmi) # time-sync bench: FMI device w/ FMU step-clamp
    add_subdirectory(fss_can_initiator_paced_fmi)    # time-sync bench: paced FMI initiator (source-role)
    add_subdirectory(fss_can_controller_queued_fmi)  # time-sync bench: queued FMI relay (N-peer clock)
else()
    message(STATUS "examples/fss-artefacts/fss_can_{target,initiator,controller}_fmi skipped — set FSS_FETCH_FMI=ON (needs FMI 3.0 + LS-BUS headers)")
endif()
