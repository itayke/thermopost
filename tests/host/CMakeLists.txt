cmake_minimum_required(VERSION 3.16)
project(thermopost_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Alarm decision logic from the alert component. Intentionally NO ESP-IDF here —
# alert_logic.cpp has zero IDF includes, so the system compiler builds it
# straight. If you reach for an IDF header in alert_logic.{hpp,cpp}, this build
# will break — that's the design, not an accident.
add_executable(test_alert
    test_alert_logic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/alert/alert_logic.cpp
)

target_include_directories(test_alert PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/alert/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(NOT MSVC)
    # Catch2 v2 trips a few harmless -Wsign-compare in its parser; the rest of
    # the warning set is worth keeping strict on our own code.
    target_compile_options(test_alert PRIVATE -Wall -Wextra -Wpedantic -Wno-sign-compare)
endif()

enable_testing()
add_test(NAME alert_unit COMMAND test_alert)
