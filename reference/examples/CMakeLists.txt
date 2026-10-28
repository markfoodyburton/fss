# Examples — organised into five buckets (see README.md):
#   example-models/  FSS-independent models + harnesses (run on their own)
#   fss-artefacts/   FSS-wrapped, dlopen-loadable artefacts (one per model)
#   federations/     wire >= 2 components together over a transport
#   tools/           drive or measure a single artefact (host probe, benchmark)
#   platforms/       one platform x a matrix of transports and model languages
#
# Order matters: models provide sources/targets that artefacts consume, and
# artefacts provide targets that federations + tools + platforms consume.  Each
# bucket's CMakeLists gates its own children on the toolchains/targets it needs.

add_subdirectory(example-models)
add_subdirectory(fss-artefacts)
add_subdirectory(tools)
add_subdirectory(platforms)
