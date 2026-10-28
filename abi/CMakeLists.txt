# fss-abi — the normative C-linking ABI of FSS, header-only.
#
# include/fss/{carrier,endpoint,transport_abi,artefact_abi}.h:
# the strict C ABI.  Other languages (Rust, ...) sit under
# languages/<lang>/.

add_library(fss-abi INTERFACE)

target_include_directories(fss-abi INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Compile the public ABI headers as both C and C++.  The executables have no
# runtime ABI dependency; CTest records that each language accepted the layout.
add_executable(fss_abi_c_compile_test tests/abi_compile.c)
target_link_libraries(fss_abi_c_compile_test PRIVATE fss-abi)
add_test(NAME fss_abi_c_compile COMMAND fss_abi_c_compile_test)

add_executable(fss_abi_cxx_compile_test tests/abi_compile.cpp)
target_link_libraries(fss_abi_cxx_compile_test PRIVATE fss-abi)
add_test(NAME fss_abi_cxx_compile COMMAND fss_abi_cxx_compile_test)

# Public ABI headers must also support pre-static-assert language standards.
add_executable(fss_abi_c99_compile_test tests/abi_compile.c)
target_link_libraries(fss_abi_c99_compile_test PRIVATE fss-abi)
set_target_properties(fss_abi_c99_compile_test PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
add_test(NAME fss_abi_c99_compile COMMAND fss_abi_c99_compile_test)

add_executable(fss_abi_cxx98_compile_test tests/abi_compile.cpp)
target_link_libraries(fss_abi_cxx98_compile_test PRIVATE fss-abi)
set_target_properties(fss_abi_cxx98_compile_test PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME fss_abi_cxx98_compile COMMAND fss_abi_cxx98_compile_test)
