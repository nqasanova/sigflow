# sigflow_generate_messages(<target> SCHEMAS <file>...)
#
# Runs msgc on the given schemas and wraps the generated C++ in a static
# library called <target>. The matching Python modules are written to
# ${SIGFLOW_PYTHON_OUT_DIR} so tests and scripts can import them.

set(SIGFLOW_MSGC_DIR "${CMAKE_CURRENT_LIST_DIR}/../tools/msgc" CACHE INTERNAL "")
set(SIGFLOW_PYTHON_OUT_DIR "${CMAKE_BINARY_DIR}/python" CACHE INTERNAL "")

function(sigflow_generate_messages target)
    cmake_parse_arguments(ARG "" "" "SCHEMAS" ${ARGN})
    if(NOT ARG_SCHEMAS)
        message(FATAL_ERROR "sigflow_generate_messages(${target}) needs at least one schema")
    endif()

    set(cpp_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}")
    set(sources)
    set(byproducts)
    foreach(schema IN LISTS ARG_SCHEMAS)
        get_filename_component(stem "${schema}" NAME_WE)
        list(APPEND sources "${cpp_dir}/${stem}_messages.cpp")
        list(APPEND byproducts
            "${cpp_dir}/${stem}_messages.hpp"
            "${cpp_dir}/${stem}_messages.cpp"
            "${SIGFLOW_PYTHON_OUT_DIR}/${stem}_messages.py")
    endforeach()

    # The generator itself is a dependency too: changing a backend has to
    # regenerate the code, not only changing a schema.
    file(GLOB_RECURSE msgc_sources CONFIGURE_DEPENDS "${SIGFLOW_MSGC_DIR}/msgc/*.py")

    # msgc only rewrites files whose content changed, which keeps their
    # timestamps stable and avoids needless recompiles. The stamp file is
    # what tells the build system that the step itself has run.
    set(stamp "${cpp_dir}/msgc.stamp")
    set(msgc ${CMAKE_COMMAND} -E env "PYTHONPATH=${SIGFLOW_MSGC_DIR}" ${Python3_EXECUTABLE} -m msgc)
    add_custom_command(
        OUTPUT "${stamp}"
        BYPRODUCTS ${byproducts}
        COMMAND ${msgc} --lang cpp --out-dir "${cpp_dir}" ${ARG_SCHEMAS}
        COMMAND ${msgc} --lang python --out-dir "${SIGFLOW_PYTHON_OUT_DIR}" ${ARG_SCHEMAS}
        COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
        DEPENDS ${ARG_SCHEMAS} ${msgc_sources}
        COMMENT "Generating messages for ${target}"
        VERBATIM)

    add_library(${target} STATIC ${sources} "${stamp}")
    target_include_directories(${target} PUBLIC "${cpp_dir}")
    target_link_libraries(${target} PUBLIC sigflow::sigflow)
    sigflow_set_warnings(${target})
endfunction()
