# ----------------------------------------------------------------------------------------------------------------------
# libFuzzer executables (UNREALLIB_BUILD_FUZZERS; clang / clang-cl only — preset clang-cl-x64-fuzz).
#
# Every UnrealLib module is compiled with -fsanitize=fuzzer-no-link (coverage) by unreallib_set_compiler_options();
# each executable here links libFuzzer's main. With UNREALLIB_ENABLE_ASAN the targets also run under AddressSanitizer.
#
#   build/clang-cl-x64-fuzz/tests/Fuzz/UnrealLibFuzzSeeds.exe corpus
#   build/clang-cl-x64-fuzz/tests/Fuzz/UnrealLibFuzz_package.exe corpus/package -max_total_time=600
# ----------------------------------------------------------------------------------------------------------------------

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "UNREALLIB_BUILD_FUZZERS needs clang or clang-cl (libFuzzer); use the clang-cl-x64-fuzz preset")
endif ()

set(_fuzz_targets package serialization properties bytecode compression assets text)

# The targets, their seeds and the synthetic package builder, shared by all executables.
add_library(unreallib_fuzz_targets STATIC
        FuzzTargets.hpp
        Seeds.cpp
        Targets/AssetsTarget.cpp
        Targets/BytecodeTarget.cpp
        Targets/CompressionTarget.cpp
        Targets/PackageTarget.cpp
        Targets/PropertiesTarget.cpp
        Targets/SerializationTarget.cpp
        Targets/TextTarget.cpp
        ../Support/SyntheticPackage.cpp)
target_include_directories(unreallib_fuzz_targets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(unreallib_fuzz_targets PUBLIC UnrealLib::UnrealLib)
unreallib_set_compiler_options(unreallib_fuzz_targets)

if (MSVC)
    # CMake links clang-cl programs with lld-link directly, so name libFuzzer's runtime the way clang's driver would.
    set(_fuzzer_main "clang_rt.fuzzer-x86_64.lib")
    set(_fuzzer_no_main "clang_rt.fuzzer_no_main-x86_64.lib")
else ()
    set(_fuzzer_main "-fsanitize=fuzzer")
    set(_fuzzer_no_main "-fsanitize=fuzzer-no-link")
endif ()

foreach (target IN LISTS _fuzz_targets)
    add_executable(unreallib_fuzz_${target} LibFuzzerMain.cpp)
    target_compile_definitions(unreallib_fuzz_${target} PRIVATE UNREALLIB_LIBFUZZER_TARGET=${target})
    target_link_libraries(unreallib_fuzz_${target} PRIVATE unreallib_fuzz_targets)
    target_link_options(unreallib_fuzz_${target} PRIVATE ${_fuzzer_main})
    set_target_properties(unreallib_fuzz_${target} PROPERTIES OUTPUT_NAME "UnrealLibFuzz_${target}"
                                                              FOLDER "UnrealLib/Fuzz")
    unreallib_set_compiler_options(unreallib_fuzz_${target})
endforeach ()

add_executable(unreallib_fuzz_seeds SeedWriter.cpp)
target_link_libraries(unreallib_fuzz_seeds PRIVATE unreallib_fuzz_targets)
target_link_options(unreallib_fuzz_seeds PRIVATE ${_fuzzer_no_main})
set_target_properties(unreallib_fuzz_seeds PROPERTIES OUTPUT_NAME "UnrealLibFuzzSeeds" FOLDER "UnrealLib/Fuzz")
unreallib_set_compiler_options(unreallib_fuzz_seeds)
