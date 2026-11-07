#pragma once

/// @file
/// @brief Structural validation of a loaded package.

#include <UnrealLib/Core/Diagnostics.hpp>

namespace UnrealLib
{
    class Package;

    /// @brief Checks the structural consistency of a loaded package.
    ///
    /// Checks performed:
    /// - summary: header size, table offsets within the package, generation counts vs. table sizes;
    /// - names: every FName in the import/export/component tables refers to an existing entry;
    /// - references: class/super/outer/archetype/component/depends indices are null or in range;
    /// - outer chains: no cycles;
    /// - exports: serial data within the package, no overlap between exports' data ranges, data after the header.
    ///
    /// Problems are reported as Severity::Error (structural) or Severity::Warning (suspicious but loadable). The
    /// package's own load diagnostics are not included; append Package::diagnostics() if needed.
    [[nodiscard]] Diagnostics validate_package(const Package& package);
}
