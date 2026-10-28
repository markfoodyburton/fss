# languages/python — FSS Python language binding (ctypes, carrier-agnostic).
#
# Via THIS binding (ctypes) Python participates as a HOST/driver (load a
# transport plugin, drive its vtable) or a receiver process (register a Python
# receive sink) — the interpreter itself is not a cdylib.  (To run a .py model
# as a dlopen'd artefact instead, see languages/python-loader, which embeds
# CPython.)
#
# The interop self-test drives the real in-proc transport + fss_minimal_echo
# artefact from Python. Those targets live under examples/ (configured AFTER
# languages/), so the `python_binding` ctest is registered from
# examples/tools/CMakeLists.txt where they exist — the same pattern as the
# fss_spi_memory_systemc_load test. (Run standalone:
#   python3 languages/python/selftest.py <inproc-plugin> <fss_minimal_echo>)

find_program(PYTHON3_EXE python3)
if(NOT PYTHON3_EXE)
    message(STATUS "languages/python skipped — python3 not found")
    return()
endif()
message(STATUS "languages/python: ctypes binding present; python_binding ctest registered from examples/tools")
