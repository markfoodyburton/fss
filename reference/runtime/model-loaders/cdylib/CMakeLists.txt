# runtime/model-loaders/cdylib — the C/SystemC/Rust model loader.
#
# Delegates to the placement facility (fss_place_construct / fss_place_endpoint /
# fss_place_destroy).  Compiled per-consumer via INTERFACE + target_sources.

add_library(fss-model-cdylib INTERFACE)
target_include_directories(fss-model-cdylib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fss-model-cdylib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/model_cdylib.c)
target_link_libraries(fss-model-cdylib INTERFACE fss-agent-core fss-placement)
