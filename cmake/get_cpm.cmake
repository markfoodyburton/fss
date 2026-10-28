# get_cpm.cmake — bootstrap CPM (CMake Package Manager) on first configure.
#
# CPM lets us declare dependencies in CMake (`CPMAddPackage(...)`) and
# fetches them from GitHub / git / URL into the build dir.  We vendor
# only this tiny bootstrap; the full CPM.cmake (~500 lines) downloads
# automatically on first configure.
#
#   include(cmake/get_cpm.cmake)
#   CPMAddPackage(...)

set(CPM_DOWNLOAD_VERSION 0.40.2)
set(CPM_HASH_SUM
    "c8cdc32c03816538ce22781ed72964dc864b2a34a310d3b7104812a5ca2d835d")

if(CPM_SOURCE_CACHE)
    set(CPM_DOWNLOAD_LOCATION
        "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_DOWNLOAD_LOCATION
        "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
    set(CPM_DOWNLOAD_LOCATION
        "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

# Make download silent on subsequent configures.
get_filename_component(CPM_DOWNLOAD_LOCATION ${CPM_DOWNLOAD_LOCATION} ABSOLUTE)

function(fss_download_cpm)
    message(STATUS "Downloading CPM.cmake to ${CPM_DOWNLOAD_LOCATION}")
    file(DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
        ${CPM_DOWNLOAD_LOCATION}
        EXPECTED_HASH SHA256=${CPM_HASH_SUM})
endfunction()

if(NOT (EXISTS ${CPM_DOWNLOAD_LOCATION}))
    fss_download_cpm()
else()
    # Re-download if hash mismatches (e.g. cache corruption).
    file(SHA256 ${CPM_DOWNLOAD_LOCATION} CPM_EXISTING_HASH)
    if(NOT CPM_EXISTING_HASH STREQUAL CPM_HASH_SUM)
        fss_download_cpm()
    endif()
endif()

include(${CPM_DOWNLOAD_LOCATION})
