# Configures, builds and runs tests/Subproject/Consumer, which builds UnrealLib from this source tree as a subproject
# (CPMAddPackage or add_subdirectory). Run by CTest (tests "Subproject.CPM", "Subproject.AddSubdirectory"):
#
#   cmake -DMODE=<CPM|SUBDIRECTORY> -DUNREALLIB_SOURCE=<repo> -DCONFIG=<cfg> -DWORK_DIR=<dir> -DGENERATOR=<gen>
#         -DC_COMPILER=<cc> -DCXX_COMPILER=<cxx> -DEXPECTED_VERSION=<x.y.z> -P RunSubprojectTest.cmake
#
# Dependencies come from CPM_SOURCE_CACHE when it is set (environment), otherwise they are downloaded.

foreach (var MODE UNREALLIB_SOURCE CONFIG WORK_DIR GENERATOR C_COMPILER CXX_COMPILER EXPECTED_VERSION)
    if (NOT DEFINED ${var})
        message(FATAL_ERROR "RunSubprojectTest.cmake: ${var} is not set")
    endif ()
endforeach ()

file(REMOVE_RECURSE "${WORK_DIR}")

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "Failed (${result}): ${ARGN}\n${out}\n${err}")
    endif ()
    set(last_output "${out}" PARENT_SCOPE)
endfunction()

run("${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/Consumer" -B "${WORK_DIR}" -G "${GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}" "-DCMAKE_C_COMPILER=${C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
    "-DUNREALLIB_SOURCE=${UNREALLIB_SOURCE}" "-DUNREALLIB_CONSUMER_MODE=${MODE}"
    "-DUNREALLIB_EXPECTED_VERSION=${EXPECTED_VERSION}")
run("${CMAKE_COMMAND}" --build "${WORK_DIR}" --config "${CONFIG}")

find_program(consumer_exe consumer PATHS "${WORK_DIR}" "${WORK_DIR}/${CONFIG}" NO_DEFAULT_PATH REQUIRED)
run("${consumer_exe}")
message(STATUS "${last_output}")
