include(FetchContent)

# Use an installed GoogleTest when there is one, otherwise download a
# pinned release. Pinning keeps CI runs reproducible.
find_package(GTest 1.14 QUIET)
if(NOT GTest_FOUND)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# C++ unit tests -------------------------------------------------------------

add_executable(sigflow_tests
    unit/byte_io_test.cpp
    unit/endpoint_test.cpp
    unit/frame_test.cpp
    unit/generated_messages_test.cpp
    unit/tlv_test.cpp
    unit/transport_test.cpp)
target_link_libraries(sigflow_tests PRIVATE ran_messages GTest::gtest_main)
sigflow_set_warnings(sigflow_tests)
gtest_discover_tests(sigflow_tests DISCOVERY_TIMEOUT 30)

# Generator tests (plain unittest, no extra packages needed) -----------------

add_test(NAME msgc_unit
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${PROJECT_SOURCE_DIR}/tools/msgc/tests -t ${PROJECT_SOURCE_DIR}/tools/msgc -v)
set_tests_properties(msgc_unit PROPERTIES
    ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/tools/msgc")

# System tests: real binaries talking over UDP --------------------------------

if(SIGFLOW_BUILD_APPS)
    add_test(NAME system_attach_flow
        COMMAND ${Python3_EXECUTABLE} -m unittest -v test_attach_flow
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/system)
    set_tests_properties(system_attach_flow PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "SIGFLOW_ENB=$<TARGET_FILE:sigflow-enb>;SIGFLOW_UE=$<TARGET_FILE:sigflow-ue>;PYTHONPATH=${PROJECT_SOURCE_DIR}/python:${SIGFLOW_PYTHON_OUT_DIR}")
endif()
