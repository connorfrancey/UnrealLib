# ----------------------------------------------------------------------------------------------------------------------
# UnrealLibOptions.cmake — user-facing build options.
#
# Development targets default to ON only when UnrealLib is the top-level project, so consumers that pull UnrealLib in
# through CPMAddPackage / add_subdirectory don't build (or download dependencies for) tests and tools.
# ----------------------------------------------------------------------------------------------------------------------

include_guard(GLOBAL)

option(UNREALLIB_BUILD_TESTS "Build the GoogleTest test suite" ${PROJECT_IS_TOP_LEVEL})
option(UNREALLIB_BUILD_TOOLS "Build the command-line tools" ${PROJECT_IS_TOP_LEVEL})
option(UNREALLIB_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(UNREALLIB_BUILD_BENCHMARKS "Build the benchmarks (Google Benchmark)" OFF)
option(UNREALLIB_BUILD_DOCS "Add a 'unreallib_docs' target that runs Doxygen" OFF)
option(UNREALLIB_ENABLE_ASAN "Build with AddressSanitizer where supported" OFF)
option(UNREALLIB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(UNREALLIB_BUILD_DIFFERENTIAL "Build differential tests against upstream C# UELib (requires the .NET SDK)" OFF)
option(UNREALLIB_DIFFERENTIAL_RECURSIVE "Differential tests cover every package below UNREALLIB_MIRRORSEDGE_DIR" OFF)
option(UNREALLIB_USE_CCACHE "Compile through ccache when it is installed" ${PROJECT_IS_TOP_LEVEL})

# Optional directory containing legally obtained Mirror's Edge PC packages (e.g. ".../mirrors edge/TdGame/CookedPC").
# Integration tests that need real fixtures are skipped when this is empty. Never commit game packages.
set(UNREALLIB_MIRRORSEDGE_DIR "" CACHE PATH "Mirror's Edge PC CookedPC directory used by fixture tests")
