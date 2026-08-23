// Catch2's main, on its own so more than one test translation unit can share it.
// CATCH_CONFIG_MAIN may be defined exactly once per binary; with it here, each
// test_*.cpp is a plain source file and new ones cost one line of CMakeLists.
#define CATCH_CONFIG_MAIN
#include "catch.hpp"
