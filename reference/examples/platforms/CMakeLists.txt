# examples/platforms — assembled PLATFORMS exercised across a matrix of
# transports and model languages (vs. federations/, which are individual
# hand-wired systems).  Each platform's CMakeLists generates one test per
# viable (transport × language) combination.

add_subdirectory(can_bus)
