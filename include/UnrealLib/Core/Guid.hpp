#pragma once

/// @file
/// @brief Unreal FGuid (four 32-bit components).

#include <cstdint>
#include <string>

namespace UnrealLib
{
    /// @brief Unreal Engine GUID: four 32-bit unsigned integers, serialized in order A, B, C, D.
    struct Guid
    {
        std::uint32_t a = 0; ///< First component.
        std::uint32_t b = 0; ///< Second component.
        std::uint32_t c = 0; ///< Third component.
        std::uint32_t d = 0; ///< Fourth component.

        /// @brief Whether all components are zero.
        [[nodiscard]] constexpr bool is_zero() const noexcept { return (a | b | c | d) == 0; }

        /// @brief Formats as 32 upper-case hexadecimal digits ("AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD"), matching Unreal's
        ///        FGuid::String().
        [[nodiscard]] std::string to_string() const;

        /// @brief Memberwise comparison.
        friend constexpr bool operator==(const Guid&, const Guid&) noexcept = default;
    };
}
