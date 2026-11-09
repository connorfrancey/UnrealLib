# Consumer that builds UnrealLib from source as part of its own build, the way a downstream project would:
#
#   UNREALLIB_CONSUMER_MODE=CPM           CPMAddPackage(NAME UnrealLib SOURCE_DIR ...), after the parent has added fmt
#                                         itself (UnrealLib must reuse that copy, not add a second one)
#   UNREALLIB_CONSUMER_MODE=SUBDIRECTORY  add_subdirectory(<UnrealLib source>)
#
# Configured, built and run by tests/Subproject/RunSubprojectTest.cmake (CTest: Subproject.CPM, Subproject.AddSubdirectory).
cmake_minimum_required(VERSION 3.25)
project(UnrealLibSubprojectConsumer LANGUAGES CXX)

foreach (var UNREALLIB_SOURCE UNREALLIB_CONSUMER_MODE UNREALLIB_EXPECTED_VERSION)
    if (NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is not set")
    endif ()
endforeach ()

if (UNREALLIB_CONSUMER_MODE STREQUAL "CPM")
    include("${UNREALLIB_SOURCE}/cmake/CPM.cmake")
    include("${UNREALLIB_SOURCE}/cmake/Versions.cmake")
    # The parent's own fmt, added before UnrealLib.
    CPMAddPackage(NAME fmt GITHUB_REPOSITORY fmtlib/fmt GIT_TAG ${FMT_TAG}
                  OPTIONS "FMT_INSTALL OFF" "FMT_TEST OFF" "FMT_DOC OFF" "FMT_MODULE OFF")
    get_target_property(_parent_fmt_dir fmt SOURCE_DIR)

    CPMAddPackage(NAME UnrealLib SOURCE_DIR "${UNREALLIB_SOURCE}")
    if (NOT UnrealLib_ADDED)
        message(FATAL_ERROR "CPMAddPackage did not add UnrealLib")
    endif ()
    get_target_property(_fmt_dir fmt SOURCE_DIR)
    if (NOT _fmt_dir STREQUAL _parent_fmt_dir)
        message(FATAL_ERROR "UnrealLib did not reuse the parent's fmt (${_parent_fmt_dir} vs ${_fmt_dir})")
    endif ()
elseif (UNREALLIB_CONSUMER_MODE STREQUAL "SUBDIRECTORY")
    add_subdirectory("${UNREALLIB_SOURCE}" UnrealLib)
else ()
    message(FATAL_ERROR "unknown UNREALLIB_CONSUMER_MODE '${UNREALLIB_CONSUMER_MODE}'")
endif ()

# As a subproject only the library is built: no tests, tools, examples, install rules or their dependencies.
foreach (option UNREALLIB_BUILD_TESTS UNREALLIB_BUILD_TOOLS UNREALLIB_BUILD_EXAMPLES UNREALLIB_INSTALL
         UNREALLIB_USE_CCACHE)
    if (${option})
        message(FATAL_ERROR "${option} defaults to ON for a subproject")
    endif ()
endforeach ()
foreach (target UnrealLibTests unreallib_header_check GTest::gtest CLI11::CLI11 nlohmann_json::nlohmann_json)
    if (TARGET ${target})
        message(FATAL_ERROR "subproject build created development target ${target}")
    endif ()
endforeach ()

add_executable(consumer "${CMAKE_CURRENT_LIST_DIR}/../../Install/Consumer/main.cpp")
target_compile_features(consumer PRIVATE cxx_std_20)
target_link_libraries(consumer PRIVATE UnrealLib::T3D UnrealLib::UnrealLib)
target_compile_definitions(consumer PRIVATE UNREALLIB_EXPECTED_VERSION="${UNREALLIB_EXPECTED_VERSION}")
