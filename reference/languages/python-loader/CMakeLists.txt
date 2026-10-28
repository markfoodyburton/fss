# languages/python-loader — embed CPython so a Python model is a dlopen'd FSS
# artefact (vs. the python/ binding, where Python is only a host or a
# cross-process sink).  Provides the loader source + an INTERFACE target that
# carries the CPython embed link/include; an artefact compiles the source with
# its own FSS_PY_MODULE / FSS_PY_DIR bakes (cf. the FMI artefact's FMU path).

find_package(Python3 COMPONENTS Development.Embed QUIET)
if(NOT Python3_FOUND)
    message(STATUS "languages/python-loader skipped — Python3 Development.Embed not found")
    return()
endif()

set(FSS_PYLOADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/py_loader.c"
    CACHE INTERNAL "path to the python-loader artefact source")

find_package(Threads REQUIRED)
add_library(fss-python-loader INTERFACE)
target_link_libraries(fss-python-loader INTERFACE fss-abi Python3::Python Threads::Threads)

# The shared Python binding (languages/python/fss.py) provides the carrier-meta
# resolver fss.meta_find — the receive shim of a Python consumer meta-resolves a
# foreign carrier's fields by qname through it.  Put its directory on the loader's
# sys.path (baked at compile time, like FSS_PY_GEN_DIR) so every python artefact
# can `import fss`.
target_compile_definitions(fss-python-loader INTERFACE
    FSS_PY_LIB_DIR="${CMAKE_SOURCE_DIR}/reference/languages/python")

# The loader walks every control carrier's meta via fss_meta_resolve (the
# carrier-agnostic resolver) — compile its source into each python artefact.
target_link_libraries(fss-python-loader INTERFACE fss-lang-c-meta)

# The loader runs the time::window protocol on fss/time — it needs the generated
# fss/protocols/time_sync.h (FSS_TIME_ID/META) to attach on its window sends.
if(TARGET fss-time-id)
    target_link_libraries(fss-python-loader INTERFACE fss-time-id)
endif()

# The loader applies config::param_request on fss/config — it needs the generated
# fss/protocols/config.h (FSS_CONFIG_ID).  Propagate the include dir +
# generation dependency to every python artefact that compiles the loader source.
if(TARGET fss-config-id)
    target_link_libraries(fss-python-loader INTERFACE fss-config-id)
endif()

# The loader dispatches sim::event on fss/sim — it needs the generated
# fss/protocols/sim.h (FSS_SIM_ID) for the upward readiness wave.
if(TARGET fss-sim-id)
    target_link_libraries(fss-python-loader INTERFACE fss-sim-id)
endif()

# Signal availability to consumers (the per-model python artefacts).
set(FSS_PYTHON_LOADER_AVAILABLE ON CACHE INTERNAL "python-loader is buildable")
