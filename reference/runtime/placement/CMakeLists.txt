# runtime/placement — process-level artefact placement facility.
#
# Compiled per-consumer via INTERFACE + target_sources.  Linked into
# EXECUTABLES and the cdylib model-loader ONLY — NOT into transport plugins
# (plugins are dlopened RTLD_LOCAL; linking placement into them would create
# duplicate registry singletons).

add_library(fss-placement INTERFACE)
target_include_directories(fss-placement INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fss-placement INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/placement.cpp)
target_link_libraries(fss-placement INTERFACE fss-abi ${CMAKE_DL_LIBS})
