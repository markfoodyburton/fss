# fss_inproc — in-process implementation of the fss_transport_t vtable
# (fss/transport_abi.h).  Loads an artefact via dlopen + dlsym and
# dispatches send() by direct callback.

add_library(fss_inproc MODULE inproc.cpp)

target_include_directories(fss_inproc PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(fss_inproc PUBLIC ${CMAKE_DL_LIBS} fss-abi)


set_target_properties(fss_inproc PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${FSS_TRANSPORT_OUTPUT_DIR}"
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
