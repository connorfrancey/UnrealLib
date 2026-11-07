# ----------------------------------------------------------------------------------------------------------------------
# UnrealLib test suite (GoogleTest only). Run with `ctest --preset <preset>`.
# ----------------------------------------------------------------------------------------------------------------------

include(GoogleTest)

add_library(unreallib_test_support STATIC
        Support/SyntheticPackage.cpp
        Support/SyntheticPackage.hpp
        Support/FixtureSupport.hpp)
target_include_directories(unreallib_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(unreallib_test_support PUBLIC UnrealLib::UnrealLib)
unreallib_set_compiler_options(unreallib_test_support)

add_executable(UnrealLibTests
        Support/TestMain.cpp
        Unit/Core/CoreTests.cpp
        Unit/Utilities/TextTests.cpp
        Unit/Serialization/ArchiveTests.cpp
        Unit/Serialization/BinaryReaderTests.cpp
        Unit/Serialization/BinaryWriterTests.cpp
        Unit/Serialization/BitStreamTests.cpp
        Unit/Serialization/CompactIndexTests.cpp
        Unit/Serialization/CompressionTests.cpp
        Unit/Serialization/FStringTests.cpp
        Unit/Serialization/PropertyBasedTests.cpp
        Unit/Names/NameTableTests.cpp
        Unit/Package/GameProfileTests.cpp
        Unit/Package/PackageLoadingTests.cpp
        Unit/Package/PackageSummaryTests.cpp
        Unit/Objects/ObjectHeaderTests.cpp
        Unit/Properties/PropertyTests.cpp
        Unit/Linker/LinkerTests.cpp
        Unit/Decompiler/DecompilerTests.cpp
        Unit/Assets/AssetTests.cpp
        Unit/Writer/WriterTests.cpp
        Unit/T3D/T3DTests.cpp
        Fuzz/MutationTests.cpp
        Integration/MirrorsEdgeFixtureTests.cpp
        Integration/MirrorsEdgeLinkerTests.cpp
        Integration/MirrorsEdgePropertyTests.cpp
        Integration/MirrorsEdgeReflectionTests.cpp
        Integration/MirrorsEdgeDecompilerTests.cpp
        Integration/MirrorsEdgeAssetTests.cpp
        Integration/MirrorsEdgeWriterTests.cpp
        Integration/MirrorsEdgeT3DTests.cpp
        Regression/ApprovalRegressionTests.cpp)

target_link_libraries(UnrealLibTests PRIVATE
        unreallib_test_support
        UnrealLib::UnrealLib
        GTest::gtest
        GTest::gmock
        rapidcheck_gtest
        ApprovalTests::ApprovalTests)   # main() is Support/TestMain.cpp (GoogleTest + ApprovalTests)
unreallib_set_compiler_options(UnrealLibTests)

set(_unreallib_test_env "")
if (UNREALLIB_MIRRORSEDGE_DIR)
    list(APPEND _unreallib_test_env "UNREALLIB_MIRRORSEDGE_DIR=${UNREALLIB_MIRRORSEDGE_DIR}")
endif ()

gtest_discover_tests(UnrealLibTests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES ENVIRONMENT "${_unreallib_test_env}")

# ----------------------------------------------------------------------------------------------------------------------
# Differential tests against upstream C# UELib (opt-in: -DUNREALLIB_BUILD_DIFFERENTIAL=ON).
#
#   1. `unreallib_uelib_dump` builds tests/Differential/UELibDump with `dotnet build` against the CPM-fetched,
#      commit-pinned upstream source (build output stays in the build tree via --artifacts-path).
#   2. CTest fixtures `Differential.UELibDump.Clean` + `.Run` run it over UNREALLIB_MIRRORSEDGE_DIR.
#   3. UnrealLibDifferentialTests (GoogleTest) compare UnrealLib's output with the dumps.
# ----------------------------------------------------------------------------------------------------------------------

if (UNREALLIB_BUILD_DIFFERENTIAL)
    find_program(UNREALLIB_DOTNET dotnet REQUIRED)

    set(_dump_artifacts "${CMAKE_CURRENT_BINARY_DIR}/uelib-dump")
    set(_dump_exe "${_dump_artifacts}/bin/UELibDump/release/UELibDump${CMAKE_EXECUTABLE_SUFFIX}")
    set(_dump_out "${CMAKE_CURRENT_BINARY_DIR}/uelib-dump-output")

    add_custom_command(
            OUTPUT "${_dump_exe}"
            COMMAND "${UNREALLIB_DOTNET}" build "${CMAKE_CURRENT_SOURCE_DIR}/Differential/UELibDump/UELibDump.csproj"
                    -c Release --nologo -v quiet -clp:ErrorsOnly
                    "-p:UELibSourceDir=${UELib_SOURCE_DIR}"
                    --artifacts-path "${_dump_artifacts}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/Differential/UELibDump/UELibDump.csproj"
                "${CMAKE_CURRENT_SOURCE_DIR}/Differential/UELibDump/Program.cs"
            COMMENT "Building UELibDump (upstream UELib ${UELIB_TAG}) with dotnet"
            VERBATIM)
    add_custom_target(unreallib_uelib_dump ALL DEPENDS "${_dump_exe}")

    add_executable(UnrealLibDifferentialTests Differential/DifferentialTests.cpp)
    target_link_libraries(UnrealLibDifferentialTests PRIVATE
            UnrealLib::UnrealLib
            nlohmann_json::nlohmann_json
            GTest::gtest_main)
    target_compile_definitions(UnrealLibDifferentialTests PRIVATE
            "UNREALLIB_DEFAULT_UELIB_DUMP_DIR=\"${_dump_out}\""
            "UNREALLIB_DEFAULT_MIRRORSEDGE_DIR=\"${UNREALLIB_MIRRORSEDGE_DIR}\"")
    unreallib_set_compiler_options(UnrealLibDifferentialTests)
    add_dependencies(UnrealLibDifferentialTests unreallib_uelib_dump)

    if (UNREALLIB_MIRRORSEDGE_DIR)
        set(_dump_args "${_dump_out}" "${UNREALLIB_MIRRORSEDGE_DIR}")
        if (UNREALLIB_DIFFERENTIAL_RECURSIVE)
            list(APPEND _dump_args --recursive)
        endif ()
        add_test(NAME Differential.UELibDump.Clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_dump_out}")
        add_test(NAME Differential.UELibDump.Run COMMAND "${_dump_exe}" ${_dump_args})
        set_tests_properties(Differential.UELibDump.Clean PROPERTIES FIXTURES_SETUP uelib_dump_clean)
        set_tests_properties(Differential.UELibDump.Run PROPERTIES
                FIXTURES_SETUP uelib_dump
                FIXTURES_REQUIRED uelib_dump_clean)
        gtest_discover_tests(UnrealLibDifferentialTests
                DISCOVERY_MODE PRE_TEST
                TEST_PREFIX "Differential."
                PROPERTIES FIXTURES_REQUIRED uelib_dump)
    else ()
        message(STATUS "UnrealLib: differential tests need UNREALLIB_MIRRORSEDGE_DIR; they will be skipped")
        gtest_discover_tests(UnrealLibDifferentialTests DISCOVERY_MODE PRE_TEST TEST_PREFIX "Differential.")
    endif ()
endif ()
