# languages/core-c — reference C helpers for reading/serialising carriers.
#
# These are NOT a shared library and NOT a mandated mechanism: a binding or
# transport that links `fss-lang-c-meta` compiles its OWN copy of the sources
# into its own binary (INTERFACE + target_sources), and is free to diverge or
# replace them — each transport may walk the meta however it likes.  This dir is
# just the reference source.  Contents:
#   • fss/c/meta.h  + meta.c  — carrier-agnostic meta reader (qname→offset,
#                               field iteration with layout + length_at).
#   • fss/c/wire.h  + wire.c  — generic pointer-inlining serialiser.
#   • fss/c/meta_hash.h       — runtime FNV-over-canonical-CBOR (header-only).
# Each is driven entirely by the carrier meta — one piece of code, every protocol.

add_library(fss-lang-c-meta INTERFACE)
target_include_directories(fss-lang-c-meta INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fss-lang-c-meta INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/meta.c
    ${CMAKE_CURRENT_SOURCE_DIR}/wire.c)
target_link_libraries(fss-lang-c-meta INTERFACE fss-abi)

# Unit tests use the generated TLM carrier meta as a real, non-trivial input.
if(FSS_TLM_ID_HEADER_AVAILABLE)
    # Resolve bus:: fields by qname; check offsets + field iteration (layout +
    # length_at) against the composition.
    add_executable(fss_c_meta_test test_meta.c)
    target_link_libraries(fss_c_meta_test PRIVATE fss-lang-c-meta fss-known-carrier-tlm)
    add_dependencies(fss_c_meta_test fss-tlm-id-gen)
    add_test(NAME c_meta_resolver COMMAND fss_c_meta_test)

    # Serialize/deserialize a TLM carrier with a pointer payload (bus::Data) and
    # confirm the inlined bytes survive the round-trip.
    add_executable(fss_c_wire_test test_wire.c)
    target_link_libraries(fss_c_wire_test PRIVATE fss-lang-c-meta fss-known-carrier-tlm)
    add_dependencies(fss_c_wire_test fss-tlm-id-gen)
    add_test(NAME c_wire_serializer COMMAND fss_c_wire_test)

    # The runtime C hash (fss/c/meta_hash.h) MUST agree bit-for-bit with the
    # compiler (tools/fss_compile.py).  The C program prints the flat + live ids
    # it computes at runtime; the python checker recomputes them and asserts.
    add_executable(fss_c_meta_hash_test test_meta_hash.c)
    target_link_libraries(fss_c_meta_hash_test PRIVATE fss-lang-c-meta fss-known-carrier-tlm)
    add_dependencies(fss_c_meta_hash_test fss-tlm-id-gen)
    find_program(PYTHON3_EXE python3)
    if(PYTHON3_EXE)
        add_test(NAME c_meta_hash
                 COMMAND ${PYTHON3_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/test_meta_hash_equality.py
                         $<TARGET_FILE:fss_c_meta_hash_test>)
    endif()
else()
    message(STATUS "languages/core-c: meta/wire tests skipped — FSS_TLM_ID header not generated")
endif()
