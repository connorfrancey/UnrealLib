cmake_minimum_required(VERSION 3.25)

# ----------------------------------------------------------------------------------------------------------------------
# UnrealLib — native C++20 Unreal Engine 3 package library (primary target: Mirror's Edge PC).
#
# Works both as a top-level project and as a subproject (add_subdirectory / CPMAddPackage).
# When consumed as a subproject, tests/tools/examples/benchmarks are OFF by default and only the
# runtime dependencies (fmt, spdlog, zlib) are requested through CPM.
# ----------------------------------------------------------------------------------------------------------------------

project(UnrealLib
        VERSION 0.1.0
        DESCRIPTION "Native C++20 Unreal Engine 3 package library"
        HOMEPAGE_URL "https://github.com/EliotVU/Unreal-Library"
        LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" "${PROJECT_SOURCE_DIR}/cmake/modules")

include(UnrealLibOptions)
include(Ccache) # before any target (including CPM dependencies) is created
include(CompilerOptions)
include(Dependencies)
include(AddUnrealLibModule)

# Generated headers (version information).
set(UNREALLIB_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated/include")
configure_file(
        "${PROJECT_SOURCE_DIR}/include/UnrealLib/Core/Version.hpp.in"
        "${UNREALLIB_GENERATED_INCLUDE_DIR}/UnrealLib/Core/Version.hpp"
        @ONLY)

add_subdirectory(src)

if (UNREALLIB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (UNREALLIB_BUILD_TOOLS)
    add_subdirectory(tools)
endif ()

if (UNREALLIB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

if (UNREALLIB_BUILD_DOCS)
    include(Documentation)
endif ()
