# ----------------------------------------------------------------------------------------------------------------------
# Install.cmake — install rules and the exported CMake package (find_package(UnrealLib)).
#
# Installs every module library (UnrealLib::<Module>), the umbrella target UnrealLib::UnrealLib, the public headers
# (plus the generated Version.hpp), the command-line tools (when built), and lib/cmake/UnrealLib/ with
# UnrealLibConfig.cmake, UnrealLibConfigVersion.cmake and UnrealLibTargets.cmake.
#
# The static libraries link fmt (public), spdlog and zlib (private), so those are installed into the same prefix by
# their own install rules (enabled in Dependencies.cmake when UNREALLIB_INSTALL is ON) and found by the package
# config with find_dependency(). Debug libraries carry a "d" postfix, so Debug and Release can share a prefix.
# ----------------------------------------------------------------------------------------------------------------------

include_guard(GLOBAL)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(UNREALLIB_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/UnrealLib"
    CACHE STRING "Install directory of the UnrealLib CMake package files, relative to the prefix")

get_property(_unreallib_modules GLOBAL PROPERTY UNREALLIB_MODULE_TARGETS)

install(TARGETS ${_unreallib_modules} unreallib
        EXPORT UnrealLibTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT UnrealLib_Development
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT UnrealLib_Runtime
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT UnrealLib_Runtime
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/UnrealLib"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT UnrealLib_Development
        FILES_MATCHING PATTERN "*.hpp")
install(FILES "${UNREALLIB_GENERATED_INCLUDE_DIR}/UnrealLib/Core/Version.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/UnrealLib/Core"
        COMPONENT UnrealLib_Development)

install(EXPORT UnrealLibTargets
        NAMESPACE UnrealLib::
        FILE UnrealLibTargets.cmake
        DESTINATION "${UNREALLIB_INSTALL_CMAKEDIR}"
        COMPONENT UnrealLib_Development)

configure_package_config_file(
        "${CMAKE_CURRENT_LIST_DIR}/UnrealLibConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/UnrealLibConfig.cmake"
        INSTALL_DESTINATION "${UNREALLIB_INSTALL_CMAKEDIR}")
# 0.x: minor versions may break the API.
write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/UnrealLibConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion)
install(FILES
            "${PROJECT_BINARY_DIR}/UnrealLibConfig.cmake"
            "${PROJECT_BINARY_DIR}/UnrealLibConfigVersion.cmake"
        DESTINATION "${UNREALLIB_INSTALL_CMAKEDIR}"
        COMPONENT UnrealLib_Development)

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/UnrealLib"
        COMPONENT UnrealLib_Runtime)
