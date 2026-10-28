# fss_can_initiator_paced_python — time-synced paced CAN source as a dlopen'd Python
# artefact.  Built from the python-loader (embeds CPython) with this model's
# module + dir baked in, plus the generated flat-carrier constants dir on the
# path (its own hop1 carrier, pyreg_write.py).

if(NOT FSS_PYTHON_LOADER_AVAILABLE)
    message(STATUS "fss_can_initiator_paced_python skipped — python-loader not buildable (Python3 Development.Embed)")
    return()
endif()
if(NOT FSS_FLAT_PY_DIR OR NOT FSS_PYREG_PY)
    message(STATUS "fss_can_initiator_paced_python skipped — pyreg::write Python constants not generated (jsonnet/python3)")
    return()
endif()

add_library(fss_can_initiator_paced_python MODULE ${FSS_PYLOADER_SOURCE})
target_link_libraries(fss_can_initiator_paced_python PRIVATE fss-python-loader)
target_compile_definitions(fss_can_initiator_paced_python PRIVATE
    FSS_PY_MODULE="can_initiator_paced"
    FSS_PY_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    FSS_PY_GEN_DIR="${FSS_FLAT_PY_DIR}")
set_target_properties(fss_can_initiator_paced_python PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
add_dependencies(fss_can_initiator_paced_python fss-canbus-id-gen)
