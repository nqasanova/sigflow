cmake_minimum_required(VERSION 3.21)

project(sigflow
    VERSION 0.3.0
    DESCRIPTION "Schema driven signaling messages for C++ and Python"
    LANGUAGES CXX)

option(SIGFLOW_BUILD_APPS "Build the UE and base station simulators" ON)
option(SIGFLOW_BUILD_TESTS "Build unit and system tests" ON)
option(SIGFLOW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(SIGFLOW_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SIGFLOW_USE_CCACHE "Use ccache when it is installed" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ccache makes rebuilds in CI (and after switching branches locally) a lot
# faster. A launcher passed on the command line or by a preset wins.
if(SIGFLOW_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()
endif()

# Sanitizers have to be applied to everything, including GoogleTest,
# otherwise mixing instrumented and plain code gives false reports.
if(SIGFLOW_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

include(cmake/CompilerWarnings.cmake)
include(cmake/Msgc.cmake)

# Core library ------------------------------------------------------------

add_library(sigflow
    src/endpoint.cpp
    src/frame.cpp
    src/loopback_transport.cpp
    src/posix_transport.cpp
    src/result.cpp
    src/tlv.cpp)
add_library(sigflow::sigflow ALIAS sigflow)
target_include_directories(sigflow PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(sigflow PUBLIC cxx_std_20)
sigflow_set_warnings(sigflow)

find_package(Threads REQUIRED)
target_link_libraries(sigflow PUBLIC Threads::Threads)

# Messages used by the simulators and the tests -----------------------------

sigflow_generate_messages(ran_messages
    SCHEMAS
        ${CMAKE_CURRENT_SOURCE_DIR}/schemas/session.msg
        ${CMAKE_CURRENT_SOURCE_DIR}/schemas/mobility.msg)

if(SIGFLOW_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(SIGFLOW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
