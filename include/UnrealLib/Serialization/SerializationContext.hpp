#pragma once

/// @file
/// @brief Version and limit information that drives version-dependent serialization.

#include <cstdint>

#include <UnrealLib/Serialization/ByteOrder.hpp>
#include <UnrealLib/Serialization/LegacyVersion.hpp>
#include <UnrealLib/Serialization/SerializationLimits.hpp>

namespace UnrealLib
{
    /// @brief State shared by readers and writers that determines how version-dependent primitives are encoded.
    ///
    /// Game-specific behaviour does not live here; it belongs to GameProfile. The context only carries the facts
    /// the low-level primitives need (version numbers, byte order, limits).
    struct SerializationContext
    {
        std::uint16_t file_version = 0;     ///< Package file version (low 16 bits of the summary version field).
        std::uint16_t licensee_version = 0; ///< Licensee version (high 16 bits of the summary version field).
        Endian endian = Endian::Little;     ///< Byte order of multi-byte values.
        SerializationLimits limits{};       ///< Allocation / size limits.

        /// @brief Whether `file_version >= gate`.
        [[nodiscard]] constexpr bool at_least(LegacyVersion gate) const noexcept
        {
            return file_version >= to_value(gate);
        }

        /// @brief Whether "index" values use the variable-length CompactIndex encoding (pre-UE3, version < 178).
        [[nodiscard]] constexpr bool uses_compact_index() const noexcept
        {
            return !at_least(LegacyVersion::CompactIndexDeprecated);
        }

        /// @brief Whether name references carry an instance number (version >= 343).
        [[nodiscard]] constexpr bool names_have_number() const noexcept
        {
            return at_least(LegacyVersion::NumberAddedToName);
        }
    };
}
