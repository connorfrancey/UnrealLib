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

# With UNREALLIB_INSTALL, fmt, spdlog and zlib install themselves next to UnrealLib (its static libraries need them).
# fmt must come before spdlog so spdlog uses this copy (SPDLOG_FMT_EXTERNAL) instead of its bundled one.
CPMAddPackage(
        NAME fmt
        GITHUB_REPOSITORY fmtlib/fmt
        GIT_TAG ${FMT_TAG}
        OPTIONS
            "FMT_INSTALL ${UNREALLIB_INSTALL}"
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
            "SPDLOG_INSTALL ${UNREALLIB_INSTALL}"
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
            "ZLIB_INSTALL ${UNREALLIB_INSTALL}"
)

# Fetch private header-only dependencies without running their upstream build or install rules.
CPMAddPackage(
        NAME magic_enum
        GITHUB_REPOSITORY Neargye/magic_enum
        GIT_TAG ${MAGICENUM_TAG}
        DOWNLOAD_ONLY YES
)

# fast_float: Fast and exact implementation of the C++ from_chars functions for number types: 4x to 10x faster than strtod, part of GCC 12, MySQL, DuckDB, Chromium, Redis and WebKit/Safari
CPMAddPackage(
        NAME fast_float
        GITHUB_REPOSITORY fastfloat/fast_float
        GIT_TAG ${FASTFLOAT_TAG}
        DOWNLOAD_ONLY YES
)

# unordered_dense
CPMAddPackage(
        NAME unordered_dense
        GITHUB_REPOSITORY martinus/unordered_dense
        GIT_TAG ${UNORDEREDDENSE_TAG}
        DOWNLOAD_ONLY YES
)

if (WIN32)
    CPMAddPackage(
            NAME mio
            GITHUB_REPOSITORY vimpunk/mio
            GIT_TAG ${MIO_TAG}
            DOWNLOAD_ONLY YES
    )
endif ()

function(unreallib_use_private_headers consumer package upstream_target)
    if (${package}_SOURCE_DIR)
        target_include_directories(${consumer} SYSTEM PRIVATE "${${package}_SOURCE_DIR}/include")
    elseif (TARGET ${upstream_target})
        target_include_directories(${consumer} SYSTEM PRIVATE
                "$<TARGET_PROPERTY:${upstream_target},INTERFACE_INCLUDE_DIRECTORIES>")
    else ()
        message(FATAL_ERROR "UnrealLib: no source directory or header target for ${package}")
    endif ()
endfunction()

# zlib >= 1.4 exports ZLIB::ZLIBSTATIC for the static build; older/system zlib exports ZLIB::ZLIB.
if (TARGET ZLIB::ZLIBSTATIC)
    set(UNREALLIB_ZLIB_TARGET ZLIB::ZLIBSTATIC)
elseif (TARGET ZLIB::ZLIB)
    set(UNREALLIB_ZLIB_TARGET ZLIB::ZLIB)
else ()
    message(FATAL_ERROR "UnrealLib: no usable zlib target (expected ZLIB::ZLIBSTATIC or ZLIB::ZLIB)")
endif ()

# ----------------------------------------------------------------------------------------------------------------------
# Tool / test dependencies (only fetched when needed). EXCLUDE_FROM_ALL keeps them out of `install` (and out of the
# default build except where a target links them).
# ----------------------------------------------------------------------------------------------------------------------

if (UNREALLIB_BUILD_TOOLS OR UNREALLIB_BUILD_TESTS)
    CPMAddPackage(
            NAME nlohmann_json
            EXCLUDE_FROM_ALL YES
            GITHUB_REPOSITORY nlohmann/json
            GIT_TAG ${JSON_TAG}
            OPTIONS
                "JSON_BuildTests OFF"
                "JSON_Install OFF"
    )
endif ()

if (UNREALLIB_BUILD_TOOLS)
    CPMAddPackage(
            NAME expected
            GITHUB_REPOSITORY TartanLlama/expected
            GIT_TAG ${TL_EXPECTED_TAG}
            DOWNLOAD_ONLY YES
    )
    CPMAddPackage(
            NAME RapidJSON
            GITHUB_REPOSITORY Tencent/rapidjson
            GIT_TAG ${RAPIDJSON_TAG}
            DOWNLOAD_ONLY YES
    )
    CPMAddPackage(
            NAME CLI11
            EXCLUDE_FROM_ALL YES
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
            EXCLUDE_FROM_ALL YES
            GITHUB_REPOSITORY google/googletest
            GIT_TAG ${GOOGLETEST_TAG}
            OPTIONS
                "INSTALL_GTEST OFF"
                "BUILD_GMOCK ON"
                "gtest_force_shared_crt ON"
    )

    # Property-based testing (RC_GTEST_PROP): generated inputs with shrinking, run as ordinary GoogleTest tests.
    CPMAddPackage(
            NAME rapidcheck
            EXCLUDE_FROM_ALL YES
            GITHUB_REPOSITORY emil-e/rapidcheck
            GIT_TAG ${RAPIDCHECK_TAG}
            OPTIONS
                "RC_ENABLE_GTEST ON"
                "RC_ENABLE_GMOCK OFF"
                "RC_ENABLE_BOOST OFF"
                "RC_ENABLE_CATCH OFF"
                "RC_ENABLE_DOCTEST OFF"
                "RC_ENABLE_TESTS OFF"
                "RC_ENABLE_EXAMPLES OFF"
                "RC_INSTALL_ALL_EXTRAS OFF"
    )

    # Approval (golden master) tests: output compared against committed tests/Regression/approved/*.approved.txt.
    # Uses our GoogleTest; its own tests, examples and bundled third-party frameworks are off.
    CPMAddPackage(
            NAME ApprovalTests
            EXCLUDE_FROM_ALL YES
            GITHUB_REPOSITORY approvals/ApprovalTests.cpp
            GIT_TAG ${APPROVALTESTS_TAG}
            OPTIONS
                "APPROVAL_TESTS_BUILD_TESTING OFF"
                "APPROVAL_TESTS_BUILD_EXAMPLES OFF"
                "APPROVAL_TESTS_BUILD_DOCS OFF"
                "APPROVAL_TESTS_ENABLE_CODE_COVERAGE OFF"
                "APPROVAL_TESTS_BUILD_THIRD_PARTY_CATCH2 OFF"
                "APPROVAL_TESTS_BUILD_THIRD_PARTY_DOCTEST OFF"
                "APPROVAL_TESTS_BUILD_THIRD_PARTY_UT OFF"
    )

    # Third-party headers: consumers include them as system headers, so our /W4 doesn't report their warnings
    # (e.g. C4324 in rapidcheck/detail/Variant.h on x86).
    set_target_properties(rapidcheck ApprovalTests PROPERTIES SYSTEM TRUE)
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
            EXCLUDE_FROM_ALL YES
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
