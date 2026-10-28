# examples/can_fmi — clean, FSS-independent FMI 3.0 / LS-BUS CAN models.
#
# Two Network FMUs (initiator + target) and a minimal co-simulation harness
# that wires them together as a trivial CAN bus.  Nothing here references FSS;
# these are the kind of existing models an FSS adapter would later wrap.
#
# Built against the fetched FMI 3.0 + FMI-LS-BUS headers (header-only APIs).

# Interface target carrying the FMI + LS-BUS include paths.
add_library(fmi_ls_bus_headers INTERFACE)
target_include_directories(fmi_ls_bus_headers INTERFACE
    ${FMI_HEADERS_DIR}
    ${FMI_LS_BUS_HEADERS_DIR}
)

# Each FMU is a shared library exporting the standard (unprefixed) fmi3* symbols.
# The harness loads them with RTLD_LOCAL so the two symbol sets stay private.
add_library(can_initiator MODULE can_initiator/can_initiator.c)
target_link_libraries(can_initiator PRIVATE fmi_ls_bus_headers)
set_target_properties(can_initiator PROPERTIES PREFIX "" OUTPUT_NAME can_initiator)

add_library(can_target MODULE can_target/can_target.c)
target_link_libraries(can_target PRIVATE fmi_ls_bus_headers)
set_target_properties(can_target PROPERTIES PREFIX "" OUTPUT_NAME can_target)

# CAN controller FMU: queues a transmit request and emits the frame after a step
# (faithful "derive + time passes").  Wrapped as the FMI controller artefact.
add_library(can_controller MODULE can_controller/can_controller.c)
target_link_libraries(can_controller PRIVATE fmi_ls_bus_headers)
set_target_properties(can_controller PROPERTIES PREFIX "" OUTPUT_NAME can_controller)

# The standalone importer harness (no FSS).
add_executable(harness_fmi_can harness_fmi_can/harness.c)
target_link_libraries(harness_fmi_can PRIVATE fmi_ls_bus_headers ${CMAKE_DL_LIBS})

# Hand the harness the built FMU module paths.
target_compile_definitions(harness_fmi_can PRIVATE
    CAN_INITIATOR_LIB="$<TARGET_FILE:can_initiator>"
    CAN_TARGET_LIB="$<TARGET_FILE:can_target>"
)
add_dependencies(harness_fmi_can can_initiator can_target)

add_test(NAME can_fmi_roundtrip COMMAND harness_fmi_can)
