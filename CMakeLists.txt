cmake_minimum_required(VERSION 3.22)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(oshexed C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# core/ has no UI and no engine: it configures, builds and tests without Luminoveau.
add_subdirectory(core)

option(HXED_BUILD_CLI "Build the command line into oshexed (oshexed --COMMAND)" ON)
if(HXED_BUILD_CLI)
    add_subdirectory(cli)
endif()

option(HXED_BUILD_TESTS "Build the core tests" ON)
if(HXED_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(HXED_BUILD_APP "Build the Luminoveau app" ON)
if(HXED_BUILD_APP)
    add_subdirectory(frontend)
endif()
