# ----------------------------------------------------------------------------------------------------------------------
# Benchmarks (Google Benchmark): -DUNREALLIB_BUILD_BENCHMARKS=ON, ideally in a Release build.
#   build/msvc-x64-release/benchmarks/UnrealLibBenchmarks.exe [--benchmark_filter=...]
# Fixture benchmarks run when UNREALLIB_MIRRORSEDGE_DIR is set (see Benchmarks.cpp).
# ----------------------------------------------------------------------------------------------------------------------

add_executable(unreallib_benchmarks
        Benchmarks.cpp
        ../tests/Support/SyntheticPackage.cpp)
target_include_directories(unreallib_benchmarks PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(unreallib_benchmarks PRIVATE UnrealLib::UnrealLib benchmark::benchmark)
set_target_properties(unreallib_benchmarks PROPERTIES OUTPUT_NAME "UnrealLibBenchmarks" FOLDER "UnrealLib/Benchmarks")
unreallib_set_compiler_options(unreallib_benchmarks)
