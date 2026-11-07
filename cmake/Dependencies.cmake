# ----------------------------------------------------------------------------------------------------------------------
# Dependencies.cmake — every third-party dependency is declared here, through CPM.cmake.
#
# * Versions are pinned in Versions.cmake.
# * Packages are only requested when the feature that needs them is enabled.
# * CPM deduplicates by NAME: if a parent project already added e.g. fmt via CPMAddPackage, that copy is reused.
#   Consumers can also set CPM_USE_LOCAL_PACKAGES / CPM_<Name>_SOURCE to override sources.
# * Set CPM_SOURCE_CACHE (environment or cache variable) to share downloads between build trees.
# ----------------------------------------------------------------------------------------------------------------------

include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/Versions.cmake)

# Reuse the parent's CPM if it has already been included, otherwise bootstrap our pinned copy.
if (NOT COMMAND CPMAddPackage)
    include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)
endif ()

message(STATUS "UnrealLib: resolving dependencies through CPM.cmake")

# ----------------------------------------------------------------------------------------------------------------------
# Runtime dependencies (always required)
# ----------------------------------------------------------------------------------------------------------------------

# fmt must come before spdlog so spdlog uses this copy (SPDLOG_FMT_EXTERNAL) instead of its bundled one.
CPMAddPackage(
        NAME fmt
        GITHUB_REPOSITORY fmtlib/fmt
        GIT_TAG ${FMT_TAG}
        OPTIONS
            "FMT_INSTALL OFF"
            "FMT_TEST OFF"
            "FMT_DOC OFF"
            "FMT_MODULE OFF"
)

CPMAddPackage(
        NAME spdlog
        GITHUB_REPOSITORY gabime/spdlog
        GIT_TAG ${SPDLOG_TAG}
        OPTIONS
            "SPDLOG_FMT_EXTERNAL ON"
            "SPDLOG_INSTALL OFF"
            "SPDLOG_BUILD_EXAMPLE OFF"
            "SPDLOG_BUILD_TESTS OFF"
            "SPDLOG_BUILD_BENCH OFF"
)

# zlib (static only). Unreal packages may store zlib-compressed chunks.
CPMAddPackage(
        NAME zlib
        GITHUB_REPOSITORY madler/zlib
        GIT_TAG ${ZLIB_TAG}
        OPTIONS
            "ZLIB_BUILD_TESTING OFF"
            "ZLIB_BUILD_SHARED OFF"
            "ZLIB_BUILD_STATIC ON"
            "ZLIB_INSTALL OFF"
)

# zlib >= 1.4 exports ZLIB::ZLIBSTATIC for the static build; older/system zlib exports ZLIB::ZLIB.
if (TARGET ZLIB::ZLIBSTATIC)
    set(UNREALLIB_ZLIB_TARGET ZLIB::ZLIBSTATIC)
elseif (TARGET ZLIB::ZLIB)
    set(UNREALLIB_ZLIB_TARGET ZLIB::ZLIB)
else ()
    message(FATAL_ERROR "UnrealLib: no usable zlib target (expected ZLIB::ZLIBSTATIC or ZLIB::ZLIB)")
endif ()

# ----------------------------------------------------------------------------------------------------------------------
# Tool / test dependencies (only fetched when needed)
# ----------------------------------------------------------------------------------------------------------------------

if (UNREALLIB_BUILD_TOOLS OR UNREALLIB_BUILD_TESTS)
    CPMAddPackage(
            NAME nlohmann_json
            GITHUB_REPOSITORY nlohmann/json
            GIT_TAG ${JSON_TAG}
            OPTIONS
                "JSON_BuildTests OFF"
                "JSON_Install OFF"
    )
endif ()

if (UNREALLIB_BUILD_TOOLS)
    CPMAddPackage(
            NAME CLI11
            GITHUB_REPOSITORY CLIUtils/CLI11
            GIT_TAG ${CLI11_TAG}
            OPTIONS
                "CLI11_BUILD_TESTS OFF"
                "CLI11_BUILD_EXAMPLES OFF"
                "CLI11_BUILD_DOCS OFF"
                "CLI11_INSTALL OFF"
                "CLI11_PRECOMPILED OFF"
    )
endif ()

if (UNREALLIB_BUILD_TESTS)
    CPMAddPackage(
            NAME googletest
            GITHUB_REPOSITORY google/googletest
            GIT_TAG ${GOOGLETEST_TAG}
            OPTIONS
                "INSTALL_GTEST OFF"
                "BUILD_GMOCK ON"
                "gtest_force_shared_crt ON"
    )
endif ()

if (UNREALLIB_BUILD_TESTS AND UNREALLIB_BUILD_DIFFERENTIAL)
    # Source only: built by tests/CMakeLists.txt with `dotnet build`, never linked into UnrealLib.
    CPMAddPackage(
            NAME UELib
            GITHUB_REPOSITORY EliotVU/Unreal-Library
            GIT_TAG ${UELIB_TAG}
            DOWNLOAD_ONLY YES
    )
endif ()

if (UNREALLIB_BUILD_BENCHMARKS)
    CPMAddPackage(
            NAME benchmark
            GITHUB_REPOSITORY google/benchmark
            GIT_TAG ${GOOGLEBENCHMARK_TAG}
            OPTIONS
                "BENCHMARK_ENABLE_TESTING OFF"
                "BENCHMARK_ENABLE_GTEST_TESTS OFF"
                "BENCHMARK_ENABLE_INSTALL OFF"
                "BENCHMARK_INSTALL_DOCS OFF"
                "BENCHMARK_ENABLE_WERROR OFF"
    )
endif ()
