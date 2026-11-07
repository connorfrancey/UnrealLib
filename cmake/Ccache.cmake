# ----------------------------------------------------------------------------------------------------------------------
# Ccache.cmake — optional compiler caching with ccache (https://ccache.dev).
#
# Enabled by UNREALLIB_USE_CCACHE (default: ON when UnrealLib is the top-level project). When ccache is not installed
# this is a no-op. Must be included before any target is created (including CPM dependencies) so every target,
# third-party ones included, is compiled through the cache.
#
# MSVC / clang-cl: ccache cannot cache compilations that write a shared PDB (/Zi, /ZI), so debug information is
# switched to embedded /Z7 via CMAKE_MSVC_DEBUG_INFORMATION_FORMAT (policy CMP0141, NEW since CMake 3.25).
# Requires a single-config generator such as Ninja (the presets use Ninja).
#
# Overrides: -DUNREALLIB_USE_CCACHE=OFF, or -DUNREALLIB_CCACHE_PROGRAM=<path>. The cache directory and size are
# controlled by ccache itself (CCACHE_DIR, `ccache -M 10G`).
# ----------------------------------------------------------------------------------------------------------------------

include_guard(GLOBAL)

if (NOT UNREALLIB_USE_CCACHE)
    return()
endif ()

# Respect a launcher the user or a parent project already configured.
if (CMAKE_CXX_COMPILER_LAUNCHER OR CMAKE_C_COMPILER_LAUNCHER)
    message(STATUS "UnrealLib: compiler launcher already set (${CMAKE_CXX_COMPILER_LAUNCHER}); not adding ccache")
    return()
endif ()

if (CMAKE_GENERATOR MATCHES "Visual Studio" OR CMAKE_GENERATOR STREQUAL "Xcode")
    message(STATUS "UnrealLib: ccache requires a Ninja or Makefile generator; not enabled for '${CMAKE_GENERATOR}'")
    return()
endif ()

# clang-cl: CMake's Ninja dependency flags (-clang:-MD/-MT/-MF) are rejected by ccache ("Compiler option -clang:-MD is
# unsupported", ccache 4.14), so every compile falls back uncached. Forcing /showIncludes instead makes ccache work
# but breaks Ninja's header dependency tracking (#deps 0), so ccache is simply not used with clang-cl.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(STATUS "UnrealLib: ccache is not used with clang-cl (incompatible dependency-file flags)")
    return()
endif ()

find_program(UNREALLIB_CCACHE_PROGRAM NAMES ccache DOC "Path to the ccache executable")
if (NOT UNREALLIB_CCACHE_PROGRAM)
    message(STATUS "UnrealLib: ccache not found; compiling without a cache (install ccache to enable it)")
    return()
endif ()

set(CMAKE_C_COMPILER_LAUNCHER "${UNREALLIB_CCACHE_PROGRAM}")
set(CMAKE_CXX_COMPILER_LAUNCHER "${UNREALLIB_CCACHE_PROGRAM}")

if (MSVC)
    # Embedded debug info (/Z7) instead of a shared PDB, which ccache cannot cache.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif ()

message(STATUS "UnrealLib: using ccache (${UNREALLIB_CCACHE_PROGRAM})")
