# Installs the built UnrealLib into a scratch prefix inside the build tree, then configures, builds and runs
# tests/Install/Consumer against it with find_package(UnrealLib). Run by CTest (test "Install.Consumer"):
#
#   cmake -DBINARY_DIR=<build> -DCONFIG=<cfg> -DSOURCE_DIR=<consumer> -DWORK_DIR=<dir> -DGENERATOR=<gen>
#         -DCXX_COMPILER=<cxx> -DEXPECTED_VERSION=<x.y.z> -P RunConsumerTest.cmake

foreach (var BINARY_DIR CONFIG SOURCE_DIR WORK_DIR GENERATOR CXX_COMPILER EXPECTED_VERSION)
    if (NOT DEFINED ${var})
        message(FATAL_ERROR "RunConsumerTest.cmake: ${var} is not set")
    endif ()
endforeach ()

set(prefix "${WORK_DIR}/prefix")
set(consumer_build "${WORK_DIR}/consumer")
file(REMOVE_RECURSE "${WORK_DIR}")

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "Failed (${result}): ${ARGN}\n${out}\n${err}")
    endif ()
    set(last_output "${out}" PARENT_SCOPE)
endfunction()

run("${CMAKE_COMMAND}" --install "${BINARY_DIR}" --prefix "${prefix}" --config "${CONFIG}")

# The prefix must hold exactly UnrealLib and its runtime dependencies (no test frameworks).
foreach (unexpected gtest gmock rapidcheck ApprovalTests benchmark nlohmann CLI)
    file(GLOB found "${prefix}/include/${unexpected}*" "${prefix}/lib/cmake/${unexpected}*")
    if (found)
        message(FATAL_ERROR "install prefix contains a development dependency: ${found}")
    endif ()
endforeach ()

run("${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${consumer_build}" -G "${GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_PREFIX_PATH=${prefix}"
    "-DUNREALLIB_EXPECTED_VERSION=${EXPECTED_VERSION}")
run("${CMAKE_COMMAND}" --build "${consumer_build}" --config "${CONFIG}")

find_program(consumer_exe consumer PATHS "${consumer_build}" "${consumer_build}/${CONFIG}" NO_DEFAULT_PATH REQUIRED)
run("${consumer_exe}")
message(STATUS "${last_output}")
