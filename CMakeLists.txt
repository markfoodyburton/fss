cmake_minimum_required(VERSION 3.20)

project(fss
    VERSION       0.2.0
    LANGUAGES     C CXX
    DESCRIPTION   "Federated Simulation Standard — reference implementations & examples"
    HOMEPAGE_URL  "https://github.com/fss-standard"
)

# ----------------------------------------------------------------------------
# Build options
# ----------------------------------------------------------------------------

option(FSS_BUILD_EXAMPLES   "Build example projects"                      ON)
option(FSS_BUILD_DOCS       "Render markdown spec/design via pandoc"     OFF)

# Fetch heavy third-party dependencies on demand.  All default OFF so
# `cmake -B build` is fast for users who only want the in-proc demo.
option(FSS_FETCH_SYSTEMC    "Fetch Accellera SystemC via CPM"            OFF)
option(FSS_FETCH_CCI        "Fetch Accellera SystemC CCI via CPM"        OFF)
option(FSS_FETCH_QBOX       "Fetch qbox via CPM"                         OFF)
option(FSS_FETCH_FMI        "Fetch FMI 3.0 headers via CPM"              OFF)
option(FSS_FETCH_ZENOH      "Fetch eclipse-zenoh-c via CPM"              OFF)
option(FSS_FETCH_TINYCBOR   "Fetch tinycbor via CPM"                     ON)

# ----------------------------------------------------------------------------
# CPM bootstrap
# ----------------------------------------------------------------------------

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/get_cpm.cmake)

# ----------------------------------------------------------------------------
# Auto-enable fetches when building examples
# ----------------------------------------------------------------------------

if(FSS_BUILD_EXAMPLES)
    set(FSS_FETCH_SYSTEMC ON CACHE BOOL "" FORCE)
    set(FSS_FETCH_CCI     ON CACHE BOOL "" FORCE)
    set(FSS_FETCH_FMI     ON CACHE BOOL "" FORCE)
endif()

# ----------------------------------------------------------------------------
# Third-party dependency fetches
# ----------------------------------------------------------------------------

set(FSS_SYSTEMC_PATH "" CACHE PATH "Local SystemC source dir (e.g. psystemc); empty = fetch from GitHub")

if(FSS_FETCH_SYSTEMC)
    if(FSS_SYSTEMC_PATH)
        CPMAddPackage(
            NAME       systemc
            SOURCE_DIR ${FSS_SYSTEMC_PATH}
            OPTIONS    "ENABLE_PHASE_CALLBACKS_TRACING OFF"
                       "BUILD_SOURCE_DOCUMENTATION OFF"
        )
    else()
        CPMAddPackage(
            NAME       systemc
            GITHUB_REPOSITORY accellera-official/systemc
            GIT_TAG    main
            OPTIONS    "ENABLE_PHASE_CALLBACKS_TRACING OFF"
                       "BUILD_SOURCE_DOCUMENTATION OFF"
        )
    endif()
endif()

if(FSS_FETCH_CCI)
    CPMAddPackage(
        NAME       cci
        GITHUB_REPOSITORY accellera-official/cci
        GIT_TAG    main
        OPTIONS    "SYSTEMCCCI_BUILD_TESTS OFF"
                   "BUILD_SOURCE_DOCUMENTATION OFF"
    )
endif()

if(FSS_FETCH_FMI)
    CPMAddPackage(
        NAME       fmi-standard
        GITHUB_REPOSITORY modelica/fmi-standard
        GIT_TAG    v3.0.2
        DOWNLOAD_ONLY YES
    )
    if(fmi-standard_ADDED)
        set(FMI_HEADERS_DIR ${fmi-standard_SOURCE_DIR}/headers
            CACHE INTERNAL "FMI 3.0 header directory")
    endif()

    # FMI-LS-BUS — the Network/Bus layered standard (CAN/LIN/FlexRay/Ethernet
    # operation definitions).  Separate repo from the core FMI 3.0 headers.
    CPMAddPackage(
        NAME       fmi-ls-bus
        GITHUB_REPOSITORY modelica/fmi-ls-bus
        GIT_TAG    main
        DOWNLOAD_ONLY YES
    )
    if(fmi-ls-bus_ADDED)
        set(FMI_LS_BUS_HEADERS_DIR ${fmi-ls-bus_SOURCE_DIR}/headers
            CACHE INTERNAL "FMI-LS-BUS header directory")
    endif()
endif()

# Zenoh-c — optional network / cross-host transport (transports/zenoh). zenoh-c
# builds via Cargo through its own CMakeLists and exports zenohc::lib. Building
# the fss_zenoh module needs compatible C and Rust toolchains, linker, and host
# architecture. zenohc::lib can be static, so it must be PIC-capable when this
# platform requires it for a loadable module. Plain network primitives only —
# the UNSTABLE_API / SHARED_MEMORY features stay OFF.
if(FSS_FETCH_ZENOH)
    # On macOS, Rust dylibs can have LINKEDIT compatibility issues when linked
    # into FSS loadable modules. Build zenoh-c as a static archive instead,
    # without changing the global BUILD_SHARED_LIBS policy for FSS targets.
    if(DEFINED BUILD_SHARED_LIBS)
        set(_fss_build_shared_libs_was_defined TRUE)
        set(_fss_build_shared_libs_previous_value "${BUILD_SHARED_LIBS}")
    endif()
    set(BUILD_SHARED_LIBS OFF)
    CPMAddPackage(
        NAME       zenoh-c
        GITHUB_REPOSITORY eclipse-zenoh/zenoh-c
        GIT_TAG    1.9.0
        OPTIONS    "ZENOHC_BUILD_WITH_UNSTABLE_API OFF"
                   "ZENOHC_BUILD_WITH_SHARED_MEMORY OFF"
    )
    if(_fss_build_shared_libs_was_defined)
        set(BUILD_SHARED_LIBS "${_fss_build_shared_libs_previous_value}")
    else()
        unset(BUILD_SHARED_LIBS)
    endif()
    unset(_fss_build_shared_libs_was_defined)
    unset(_fss_build_shared_libs_previous_value)
endif()

# ----------------------------------------------------------------------------
# Compiler / global settings
# ----------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ----------------------------------------------------------------------------
# Subprojects
# ----------------------------------------------------------------------------

enable_testing()

# The C ABI — the reference contract every language binding and transport targets.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/abi/CMakeLists.txt)
    add_subdirectory(abi)
endif()

# Protocols — carrier shape definitions (must come before transports that use them).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/protocols/CMakeLists.txt)
    add_subdirectory(protocols)
endif()

# Known carriers (e.g. the TLM carrier id/meta) — before core-c, whose tests use
# the generated TLM meta, and before transports.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/known_carriers/CMakeLists.txt)
    add_subdirectory(known_carriers)
endif()

# Binding-independent C helpers (fss-lang-c-meta: meta resolver + wire
# serializer).  Before transports + languages, which all link it.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/reference/languages/core-c/CMakeLists.txt)
    add_subdirectory(reference/languages/core-c)
endif()

# Transports — one implementation per wire: in-process, shared memory, and,
# when FSS_FETCH_ZENOH=ON, Zenoh.
# All transport plugins land in ONE directory so an agent can resolve a bare
# transport NAME (carried in control::instr) to libfss_<name>.<ext> on a single
# search path (FSS_TRANSPORT_DIR), rather than being handed a full path per edge.
set(FSS_TRANSPORT_OUTPUT_DIR "${CMAKE_BINARY_DIR}/transports")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/reference/transports/inproc/CMakeLists.txt)
    add_subdirectory(reference/transports/inproc)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/reference/transports/shm/CMakeLists.txt)
    add_subdirectory(reference/transports/shm)
endif()

# Zenoh transport: only when its dir exists AND zenoh-c was fetched (target
# present).  A default -DFSS_FETCH_ZENOH=OFF build skips it entirely.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/reference/transports/zenoh/CMakeLists.txt
   AND TARGET zenohc::lib)
    add_subdirectory(reference/transports/zenoh)
endif()

# Language bindings — per-language FSS runtime (carrier/endpoint + send/receive).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/reference/languages/CMakeLists.txt)
    add_subdirectory(reference/languages)
endif()

# Ring-3 runtime — launcher / orchestrator / agents (NON-NORMATIVE machinery that
# builds a federation).  After the four ABIs + transports + languages it composes;
# before examples, which consume it.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/reference/runtime/CMakeLists.txt)
    add_subdirectory(reference/runtime)
endif()

if(FSS_BUILD_EXAMPLES)
    add_subdirectory(reference/examples)
endif()

if(FSS_BUILD_DOCS)
    add_subdirectory(docs)
endif()

# ----------------------------------------------------------------------------
# reference/tools/ — standalone Python tooling
# ----------------------------------------------------------------------------

if(EXISTS ${CMAKE_SOURCE_DIR}/reference/tools/test_fss_compile.py)
    find_program(JSONNET_EXE jsonnet)
    if(JSONNET_EXE)
        add_test(NAME fss_compile_tests
                 COMMAND ${CMAKE_SOURCE_DIR}/reference/tools/test_fss_compile.py
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    else()
        message(STATUS "reference/tools/test_fss_compile.py skipped — jsonnet not on PATH")
    endif()
endif()

# ----------------------------------------------------------------------------
# Configure summary
# ----------------------------------------------------------------------------

message(STATUS "")
message(STATUS "FSS configuration summary")
message(STATUS "  CMAKE_BUILD_TYPE       : ${CMAKE_BUILD_TYPE}")
message(STATUS "  FSS_BUILD_EXAMPLES     : ${FSS_BUILD_EXAMPLES}")
message(STATUS "  FSS_BUILD_DOCS         : ${FSS_BUILD_DOCS}")
message(STATUS "  FSS_FETCH_SYSTEMC      : ${FSS_FETCH_SYSTEMC}")
message(STATUS "  FSS_SYSTEMC_PATH       : ${FSS_SYSTEMC_PATH}")
message(STATUS "  FSS_FETCH_CCI          : ${FSS_FETCH_CCI}")
message(STATUS "  FSS_FETCH_QBOX         : ${FSS_FETCH_QBOX}")
message(STATUS "  FSS_FETCH_FMI          : ${FSS_FETCH_FMI}")
message(STATUS "  FMI_HEADERS_DIR        : ${FMI_HEADERS_DIR}")
message(STATUS "  FMI_LS_BUS_HEADERS_DIR : ${FMI_LS_BUS_HEADERS_DIR}")
message(STATUS "  FSS_FETCH_ZENOH        : ${FSS_FETCH_ZENOH}")
message(STATUS "  FSS_FETCH_TINYCBOR     : ${FSS_FETCH_TINYCBOR}")
message(STATUS "")
