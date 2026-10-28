# fss_zenoh — network / cross-host implementation of the fss_transport_t vtable
# (fss/transport_abi.h), built on eclipse-zenoh/zenoh-c.  See zenoh.c /
# docs/design/zenoh-transport.md. Only added when FSS_FETCH_ZENOH fetched
# zenoh-c and provided zenohc::lib. Zenoh-c builds through Cargo, so this
# optional target needs a compatible C toolchain, Cargo/Rust, linker, and host
# architecture. zenohc::lib may be static; it must be suitable for linking into
# this loadable module (including PIC where the platform requires it).

add_library(fss_zenoh MODULE zenoh.c)

target_include_directories(fss_zenoh PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(fss_zenoh PUBLIC ${CMAKE_DL_LIBS} fss-abi fss-lang-c-meta zenohc::lib)

set_target_properties(fss_zenoh PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${FSS_TRANSPORT_OUTPUT_DIR}"
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

# ── cross-process multipoint spike ──────────────────────────────────────────
# Proves the zenoh transport already supports cross-PROCESS fan-out (1->N pub/
# sub) and fan-in (N->1 query/reply) with NO change to zenoh.c: each participant
# is its own process (own session), so the single-sink-per-instance cap never
# bites.  Forks a "1" side (parent, listens) + N "N" side children (connect).
# argv[1] = fss_zenoh module path; argv[2] selects one test mode.
add_executable(test_zenoh_multipoint test_zenoh_multipoint.c)
target_link_libraries(test_zenoh_multipoint PRIVATE ${CMAKE_DL_LIBS} fss-abi)
set_target_properties(test_zenoh_multipoint PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
add_dependencies(test_zenoh_multipoint fss_zenoh)

# Exercise the private request-frame parser without a live Zenoh session.
add_executable(test_zenoh_frame test_zenoh_frame.c)
target_include_directories(test_zenoh_frame PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(test_zenoh_frame PRIVATE ${CMAKE_DL_LIBS} fss-abi fss-lang-c-meta zenohc::lib)
set_target_properties(test_zenoh_frame PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

add_test(NAME zenoh_multipoint_fanout
         COMMAND test_zenoh_multipoint $<TARGET_FILE:fss_zenoh> fanout)
add_test(NAME zenoh_multipoint_fanin
          COMMAND test_zenoh_multipoint $<TARGET_FILE:fss_zenoh> fanin)
add_test(NAME zenoh_ready_roundtrip
          COMMAND test_zenoh_multipoint $<TARGET_FILE:fss_zenoh> ready-roundtrip)
add_test(NAME zenoh_ready_pubsub
         COMMAND test_zenoh_multipoint $<TARGET_FILE:fss_zenoh> ready-pubsub)
add_test(NAME zenoh_overlong_channel
         COMMAND test_zenoh_multipoint $<TARGET_FILE:fss_zenoh> overlong-channel)
add_test(NAME zenoh_request_frame_trailing_bytes
         COMMAND test_zenoh_frame)
