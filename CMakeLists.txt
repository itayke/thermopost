cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)

# The release version. tools/release.sh builds with THERMOPOST_RELEASE set; every other
# build ends in "-dev", which a post never replaces with an update from the Sheet.
set(THERMOPOST_VERSION 0.2.0)
if(THERMOPOST_RELEASE)
    set(PROJECT_VER ${THERMOPOST_VERSION})
else()
    set(PROJECT_VER ${THERMOPOST_VERSION}-dev)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(thermopost
    VERSION ${THERMOPOST_VERSION}
    LANGUAGES C CXX)

idf_build_set_property(CXX_STANDARD 17)
idf_build_set_property(COMPILE_OPTIONS "-Wno-missing-field-initializers" APPEND)
