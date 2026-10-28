# fss_can_target_paced_python — time-synced Python CAN device (python-loader
# artefact).  Same loader source as the other Python artefacts (the windowed
# clock is built in, dormant unless fss/time is bound), with can_device_paced
# baked in.

if(NOT FSS_PYTHON_LOADER_AVAILABLE)
    message(STATUS "fss_can_target_paced_python skipped — python-loader not buildable (Python3 Development.Embed)")
    return()
endif()

add_library(fss_can_target_paced_python MODULE ${FSS_PYLOADER_SOURCE})

target_link_libraries(fss_can_target_paced_python PRIVATE fss-python-loader)

target_compile_definitions(fss_can_target_paced_python PRIVATE
    FSS_PY_MODULE="can_device_paced"
    FSS_PY_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(fss_can_target_paced_python PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
