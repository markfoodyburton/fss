# fss_minimal_echo — minimal FSS artefact plugin (plain C).
# Exports fss_create / fss_destroy / fss_get_endpoint.
# Built as a MODULE (dlopen target) so fss_inproc can load it at runtime.

add_library(fss_minimal_echo MODULE echo.c)

target_link_libraries(fss_minimal_echo PRIVATE fss-abi)

set_target_properties(fss_minimal_echo PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
