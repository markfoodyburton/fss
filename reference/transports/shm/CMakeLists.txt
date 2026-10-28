# fss_shm — cross-process shared-memory implementation of the fss_transport_t
# vtable (fss/transport_abi.h).  Bridges to a peer process over a POSIX SHM
# segment + named-semaphore doorbell.  See shm.c / docs/design/shm-transport.md.

add_library(fss_shm MODULE shm.c)

target_include_directories(fss_shm PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

# fss-lang-c-meta: the carrier-agnostic wire serialiser (fss/c/wire.h) + meta
# resolver — used to inline pointer (fss:local-ptr) payloads across the boundary.
target_link_libraries(fss_shm PUBLIC ${CMAKE_DL_LIBS} fss-abi fss-lang-c-meta)

# macOS: shm_open / mmap / sem_open / pthread all live in libSystem — no extra
# link flags (proven by the spike).  Linux needs pthread (and historically rt
# for shm_open); link them only there.
if(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(fss_shm PUBLIC Threads::Threads rt)
endif()

set_target_properties(fss_shm PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${FSS_TRANSPORT_OUTPUT_DIR}"
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

# ── cross-process multipoint spike ──────────────────────────────────────────
# Proves the SHM transport supports cross-PROCESS fan-out (1->N) and fan-in
# (N->1) with NO change to shm.c: shm stays a 1:1 SPSC primitive and multipoint
# is realised as N INDEPENDENT 1:1 SPSC segments in the WIRING.  The parent
# provisions all N segments as owner=1 before forking; fan-out sends on each in
# turn, fan-in attaches one reentrant sink as TARGET to all N (N reader threads,
# one sink) with per-initiator replies on separate reply rings.
# argv[1] = fss_shm module path; argv[2] = fanout|fanin.
add_executable(test_shm_multipoint test_shm_multipoint.c)
target_link_libraries(test_shm_multipoint PRIVATE ${CMAKE_DL_LIBS} fss-abi)
if(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(test_shm_multipoint PRIVATE Threads::Threads)
endif()
set_target_properties(test_shm_multipoint PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
add_dependencies(test_shm_multipoint fss_shm)

add_test(NAME shm_multipoint_fanout
         COMMAND test_shm_multipoint $<TARGET_FILE:fss_shm> fanout)
add_test(NAME shm_multipoint_fanin
          COMMAND test_shm_multipoint $<TARGET_FILE:fss_shm> fanin)

add_executable(test_shm_hardening test_shm_hardening.c)
target_link_libraries(test_shm_hardening PRIVATE ${CMAKE_DL_LIBS} fss-abi)
set_target_properties(test_shm_hardening PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(test_shm_hardening fss_shm)
add_test(NAME shm_hardening
          COMMAND test_shm_hardening $<TARGET_FILE:fss_shm>)

add_executable(test_shm_regressions test_shm_regressions.c)
target_link_libraries(test_shm_regressions PRIVATE ${CMAKE_DL_LIBS} fss-abi)
if(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(test_shm_regressions PRIVATE Threads::Threads)
endif()
set_target_properties(test_shm_regressions PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(test_shm_regressions fss_shm)
add_test(NAME shm_regressions
         COMMAND test_shm_regressions $<TARGET_FILE:fss_shm>)
