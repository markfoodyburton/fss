# runtime/launcher — spawn an agent + hand it its born-with (NON-NORMATIVE).
#
# The thinnest, least standard part of the runtime (fork + execv, argv marshal).
# Compiled per-consumer via INTERFACE + target_sources.

add_library(fss-launcher INTERFACE)
target_include_directories(fss-launcher INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(fss-launcher INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/launcher.c)
