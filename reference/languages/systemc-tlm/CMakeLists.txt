# languages/systemc-tlm — SystemC-TLM language binding for FSS.
#
# Provides:
#   • systemc_no_main  OBJECT  — SystemC sans sc_main, for static embedding in DLLs
#   • cci_no_main      OBJECT  — CCI sans process-wide state, optional sibling
#   • fss_systemc_tlm_add_module() — helper to define an FSS module .dylib

if(NOT TARGET systemc)
    message(STATUS "languages/systemc-tlm: 'systemc' target not found — skipping "
                   "(set FSS_FETCH_SYSTEMC=ON)")
    return()
endif()

enable_language(ASM)
find_package(Threads REQUIRED)

# The carrier-agnostic meta resolver + wire serializer the SystemC receiver
# consumes now live in languages/core-c (target fss-lang-c-meta) —
# binding-independent so transports can link them too.  Referenced by name.

# ---------------------------------------------------------------------------
# Locate the SystemC source tree via the CPM-fetched target's SOURCE_DIR.
# ---------------------------------------------------------------------------
get_target_property(_SC_SRC_DIR systemc SOURCE_DIR)
if(NOT EXISTS "${_SC_SRC_DIR}/sysc/kernel/sc_simcontext.cpp")
    set(_SC_SRC_DIR "${_SC_SRC_DIR}/src")
endif()
if(NOT EXISTS "${_SC_SRC_DIR}/sysc/kernel/sc_simcontext.cpp")
    message(FATAL_ERROR "languages/systemc-tlm: cannot locate SystemC source tree under ${_SC_SRC_DIR}")
endif()
message(STATUS "languages/systemc-tlm: SystemC source dir = ${_SC_SRC_DIR}")

# ---------------------------------------------------------------------------
# Gather SystemC sources: all .cpp excluding sc_main* files, plus QuickThreads
# .c and the correct .s for the current architecture.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _ALL_SC_CPP "${_SC_SRC_DIR}/*.cpp")
set(_SC_SOURCES "")
foreach(_f IN LISTS _ALL_SC_CPP)
    string(FIND "${_f}" "sc_main.cpp"      _p1)
    string(FIND "${_f}" "sc_main_main.cpp" _p2)
    if(_p1 EQUAL -1 AND _p2 EQUAL -1)
        list(APPEND _SC_SOURCES "${_f}")
    endif()
endforeach()

list(APPEND _SC_SOURCES "${_SC_SRC_DIR}/sysc/packages/qt/qt.c")

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _proc)
if(_proc STREQUAL "arm64")
    set(_proc "aarch64")
endif()
set(_qt_asm "${_SC_SRC_DIR}/sysc/packages/qt/md/${_proc}.s")
if(EXISTS "${_qt_asm}")
    list(APPEND _SC_SOURCES "${_qt_asm}")
    message(STATUS "languages/systemc-tlm: QT assembly = ${_qt_asm}")
else()
    message(WARNING "languages/systemc-tlm: QT assembly not found for arch '${_proc}'")
endif()

# ---------------------------------------------------------------------------
# OBJECT lib: SystemC sans sc_main
# ---------------------------------------------------------------------------
add_library(systemc_no_main OBJECT ${_SC_SOURCES})

target_include_directories(systemc_no_main PUBLIC
    "${_SC_SRC_DIR}")

target_compile_definitions(systemc_no_main PRIVATE
    SC_BUILD
    SC_INCLUDE_FX
    SC_ENABLE_ASSERTIONS)

set_target_properties(systemc_no_main PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD   11
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

target_compile_options(systemc_no_main PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter -Wno-unused-variable>
    $<$<COMPILE_LANGUAGE:C>:  -Wno-unused-parameter -Wno-unused-variable>
    $<$<COMPILE_LANGUAGE:ASM>:-xassembler-with-cpp -Qunused-arguments>)

# ---------------------------------------------------------------------------
# OBJECT lib: CCI sans process-wide state (sibling, conditional on cci-config)
# ---------------------------------------------------------------------------
if(TARGET cci-config)
    get_target_property(_CCI_CFG_SRC cci-config SOURCE_DIR)
    # cci-config is rooted at <cci-src>/configuration/src; SOURCE_DIR points there.
    get_target_property(_CCI_INSPECT_SRC cci-inspection SOURCE_DIR)
    # The umbrella `cci_configuration` header includes `cci/common/cci_version.h`,
    # which lives at the CCI source root (parent of configuration/), not under
    # configuration/src/.  Add the root to the public include path.
    get_filename_component(_CCI_ROOT_SRC "${_CCI_CFG_SRC}/../.." ABSOLUTE)

    set(_CCI_CFG_SOURCES
        "${_CCI_CFG_SRC}/cci/core/cci_value_converter.cpp"
        "${_CCI_CFG_SRC}/cci/core/cci_name_gen.cpp"
        "${_CCI_CFG_SRC}/cci/core/cci_value.cpp"
        "${_CCI_CFG_SRC}/cci/utils/consuming_broker.cpp"
        "${_CCI_CFG_SRC}/cci/utils/broker.cpp"
        "${_CCI_CFG_SRC}/cci/cfg/cci_report_handler.cpp"
        "${_CCI_CFG_SRC}/cci/cfg/cci_param_if.cpp"
        "${_CCI_CFG_SRC}/cci/cfg/cci_broker_manager.cpp"
        "${_CCI_CFG_SRC}/cci/cfg/cci_param_untyped.cpp"
        "${_CCI_CFG_SRC}/cci/cfg/cci_param_untyped_handle.cpp"
        "${_CCI_CFG_SRC}/cci/cfg/cci_originator.cpp"
        "${_CCI_CFG_SRC}/cci/cfg/cci_broker_handle.cpp")

    set(_CCI_INSPECT_SOURCES
        "${_CCI_INSPECT_SRC}/cci/inspection/cci_inspection.cpp")

    add_library(cci_no_main OBJECT
        ${_CCI_CFG_SOURCES}
        ${_CCI_INSPECT_SOURCES})

    target_include_directories(cci_no_main PUBLIC
        "${_CCI_ROOT_SRC}"
        "${_CCI_CFG_SRC}"
        "${_CCI_INSPECT_SRC}")

    target_include_directories(cci_no_main PRIVATE
        "${_CCI_CFG_SRC}/rapidjson/include")

    target_compile_definitions(cci_no_main PRIVATE
        SC_BUILD
        SC_INCLUDE_FX)

    set_target_properties(cci_no_main PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    target_compile_options(cci_no_main PRIVATE
        -Wno-unused-parameter -Wno-unused-variable)

    # cci_no_main needs SystemC public headers.
    target_include_directories(cci_no_main PRIVATE
        "${_SC_SRC_DIR}")

    set(_FSS_HAVE_CCI ON)
else()
    set(_FSS_HAVE_CCI OFF)
endif()

# ---------------------------------------------------------------------------
# Helper: define an FSS-deployable SystemC module .dylib.
# ---------------------------------------------------------------------------
function(fss_systemc_tlm_add_module target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "fss_systemc_tlm_add_module(${target}): SOURCES required")
    endif()

    # Threads::Threads is an IMPORTED target with directory scope; re-find here
    # so the helper works when called from any caller's CMakeLists.
    find_package(Threads REQUIRED)

    set(_objs $<TARGET_OBJECTS:systemc_no_main>)
    if(TARGET cci_no_main)
        list(APPEND _objs $<TARGET_OBJECTS:cci_no_main>)
    endif()

    add_library(${target} MODULE ${ARG_SOURCES} ${_objs})

    target_include_directories(${target} PRIVATE
        $<TARGET_PROPERTY:systemc_no_main,INTERFACE_INCLUDE_DIRECTORIES>
        "${CMAKE_SOURCE_DIR}/reference/languages/systemc-tlm/include"
        $<TARGET_PROPERTY:fss-known-carrier-tlm,INTERFACE_INCLUDE_DIRECTORIES>
        $<$<BOOL:${FSS_SIM_ID_HEADER_AVAILABLE}>:$<TARGET_PROPERTY:fss-sim-id,INTERFACE_INCLUDE_DIRECTORIES>>
        $<$<BOOL:${FSS_CONFIG_ID_HEADER_AVAILABLE}>:$<TARGET_PROPERTY:fss-config-id,INTERFACE_INCLUDE_DIRECTORIES>>
        $<$<BOOL:${FSS_TIME_ID_HEADER_AVAILABLE}>:$<TARGET_PROPERTY:fss-time-id,INTERFACE_INCLUDE_DIRECTORIES>>)

    if(TARGET cci_no_main)
        target_include_directories(${target} PRIVATE
            $<TARGET_PROPERTY:cci_no_main,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()

    target_compile_definitions(${target} PRIVATE
        SC_INCLUDE_FX
        SC_ENABLE_ASSERTIONS)

    # fss-lang-c-meta: the carrier-agnostic Path-B meta resolver the artefact
    # receiver uses when carrier->id is not the compile-time TLM id.
    target_link_libraries(${target} PRIVATE Threads::Threads fss-abi fss-lang-c-meta)

    # The receiver includes the generated fss/known_carriers/tlm_id.h
    # (FSS_TLM_ID + FSS_TLM_META); make sure it is generated first.
    if(TARGET fss-tlm-id-gen)
        add_dependencies(${target} fss-tlm-id-gen)
    endif()
    # The artefact wrapper includes the generated fss/protocols/sim.h (FSS_SIM_ID).
    if(TARGET fss-sim-id-gen)
        add_dependencies(${target} fss-sim-id-gen)
    endif()
    # ...and fss/protocols/config.h (FSS_CONFIG_ID) for the fss/config port.
    if(TARGET fss-config-id-gen)
        add_dependencies(${target} fss-config-id-gen)
    endif()
    # ...and fss/protocols/time_sync.h (FSS_TIME_ID) for the fss/time port.
    if(TARGET fss-time-id-gen)
        add_dependencies(${target} fss-time-id-gen)
    endif()

    set_target_properties(${target} PROPERTIES
        PREFIX ""
        SUFFIX ".dylib"
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    if(APPLE)
        target_link_options(${target} PRIVATE
            "LINKER:-undefined,dynamic_lookup")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Test: a LIVE tlm_generic_payload serialises over the wire — the complete TLM
# meta (FSS_TLM_META, real GP offsets) lets the generic serialiser walk a live
# GP, inline bus::Data, and round-trip it into a receiver's buffers.  Links the
# full SystemC to build a real GP.  (Proves the SC-initiator-over-wire mechanism;
# the 2-process zenoh federation is the integration on top.)
# ---------------------------------------------------------------------------
if(FSS_TLM_ID_HEADER_AVAILABLE AND TARGET SystemC::systemc)
    add_executable(fss_live_gp_serialize_test test_live_gp_serialize.cpp)
    target_link_libraries(fss_live_gp_serialize_test PRIVATE
        fss-lang-c-meta fss-known-carrier-tlm SystemC::systemc)
    add_dependencies(fss_live_gp_serialize_test fss-tlm-id-gen)
    add_test(NAME tlm_live_gp_serialize COMMAND fss_live_gp_serialize_test)
endif()

# Note: sc_ob_event.h (the observable-event wrapper the time-sync upper bound
# rests on) is vendored from upstream qbox; it is USED by the binding
# (artefact.h) but not unit-tested here — testing the vendored kernel primitive
# is not FSS's responsibility.
