cmake_minimum_required(VERSION 3.16)
project(thermopost_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# One binary, so `run.sh "[tag]"` filters across everything rather than needing to
# know which executable holds which tag. Catch2's main is in main.cpp; each
# test_*.cpp is a plain source file beside it.
#
# Intentionally NO ESP-IDF here. Every source listed below has zero IDF includes,
# so the system compiler builds it straight. If you reach for an IDF header in one
# of them, this build breaks — that is the design, not an accident.
add_executable(test_host
    main.cpp
    test_alert_logic.cpp
    test_cloud_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/alert/alert_logic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/cloud/cloud_json.cpp
)

target_include_directories(test_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/alert/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/cloud   # cloud_json.hpp is private to the component
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(NOT MSVC)
    # Catch2 v2 trips a few harmless -Wsign-compare in its parser; the rest of
    # the warning set is worth keeping strict on our own code.
    target_compile_options(test_host PRIVATE -Wall -Wextra -Wpedantic -Wno-sign-compare)
endif()

enable_testing()
add_test(NAME host_unit COMMAND test_host)
