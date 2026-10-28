# Known carriers — externally-defined protocols where FSS packages the carrier shape.

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tlm/CMakeLists.txt)
    add_subdirectory(tlm)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/can/CMakeLists.txt)
    add_subdirectory(can)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/canbus/CMakeLists.txt)
    add_subdirectory(canbus)
endif()
