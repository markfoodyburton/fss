# fss_can_controller_queued_python — time-synced bounded-FIFO CAN relay as a
# dlopen'd Python artefact.  Consumes any hop1 register-write by meta-resolve
# (via the shared fss.py binding, on the loader's baked-in FSS_PY_LIB_DIR); EMITS
# its own pycan::frame, so it needs the generated pycan_frame.py on FSS_PY_GEN_DIR.

if(NOT FSS_PYTHON_LOADER_AVAILABLE)
    message(STATUS "fss_can_controller_queued_python skipped — python-loader not buildable (Python3 Development.Embed)")
    return()
endif()
if(NOT FSS_FLAT_PY_DIR OR NOT FSS_PYCAN_PY)
    message(STATUS "fss_can_controller_queued_python skipped — pycan::frame Python constants not generated (jsonnet/python3)")
    return()
endif()

add_library(fss_can_controller_queued_python MODULE ${FSS_PYLOADER_SOURCE})
target_link_libraries(fss_can_controller_queued_python PRIVATE fss-python-loader)
target_compile_definitions(fss_can_controller_queued_python PRIVATE
    FSS_PY_MODULE="can_controller_queued"
    FSS_PY_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    FSS_PY_GEN_DIR="${FSS_FLAT_PY_DIR}")
set_target_properties(fss_can_controller_queued_python PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
add_dependencies(fss_can_controller_queued_python fss-canbus-id-gen)
