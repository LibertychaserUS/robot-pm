cmake_minimum_required(VERSION 3.28)
project(robot-pm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(cmake/warnings.cmake)
include(cmake/sanitizers.cmake)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nlohmann_json doctest)

include(CTest)
add_subdirectory(libs/core)
add_subdirectory(apps/robot-pm)
add_subdirectory(apps/feishu-rules)
add_subdirectory(apps/supervise)
if(BUILD_TESTING)
    add_subdirectory(tests/cpp)
    add_subdirectory(tests/blackbox)
endif()
