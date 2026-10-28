# protocols/time-sync — time::window control carrier (run-to / do-not-pass).
#
#  - window.jsonnet (committed)  — carrier shape definition.
#  - time_sync.h   (generated)   — FSS_TIME_ID + FSS_TIME_META emitted from the
#    Jsonnet by reference/tools/fss_compile.py, includable as
#    fss/protocols/time_sync.h (mirrors protocols/config + sim-events).

find_program(JSONNET_EXE jsonnet)
find_program(PYTHON3_EXE python3)
if(JSONNET_EXE AND PYTHON3_EXE)
    set(_ts_shape ${CMAKE_SOURCE_DIR}/protocols/time-sync/window.jsonnet)
    set(_gen_dir  ${CMAKE_CURRENT_BINARY_DIR}/gen/fss/protocols)
    set(_gen_hdr  ${_gen_dir}/time_sync.h)
    add_custom_command(
        OUTPUT ${_gen_hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
                --emit-c-header FSS_TIME ${_ts_shape} ${_gen_hdr}
        DEPENDS ${_ts_shape} ${CMAKE_SOURCE_DIR}/reference/tools/fss_compile.py
        COMMENT "Generating FSS_TIME_ID from window.jsonnet"
        VERBATIM)
    add_custom_target(fss-time-id-gen DEPENDS ${_gen_hdr})

    add_library(fss-time-id INTERFACE)
    add_dependencies(fss-time-id fss-time-id-gen)
    target_include_directories(fss-time-id INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/gen)

    set(FSS_TIME_ID_HEADER_AVAILABLE ON CACHE INTERNAL "time_sync.h is generated")
else()
    message(STATUS "protocols/time-sync: jsonnet/python3 not found — FSS_TIME_ID not generated")
endif()
