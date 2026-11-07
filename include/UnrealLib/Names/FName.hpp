#pragma once

/// @file
/// @brief Serialized name reference (index into a package name table plus instance number).

#include <compare>
#include <cstdint>

namespace UnrealLib
{
    class BinaryReader;
    class BinaryWriter;

    /// @brief A reference to a name-table entry, as serialized in a package.
    ///
    /// `number` holds the value exactly as stored on disk (UE3 FName::Number): 0 means "no instance suffix",
    /// N > 0 means the suffix "_<N-1>" (e.g. index→"Mesh", number 3 → "Mesh_2"). Packages older than version 343
    /// have no number field; it is then always 0.
    ///
    /// Upstream UELib stores `number - 1` instead (UName.Number, -1 meaning none); UnrealLib keeps the on-disk value
    /// so that writing is the identity.
    struct FName
    {
        std::int32_t index = 0;  ///< Index into the owning package's NameTable.
        std::int32_t number = 0; ///< On-disk instance number (0 = none).

        /// @brief Whether an instance suffix is present.
        [[nodiscard]] constexpr bool has_number() const noexcept { return number > 0; }

        /// @brief Memberwise comparison.
        friend constexpr auto operator<=>(const FName&, const FName&) noexcept = default;
    };

    /// @brief Reads a name reference: `index` (CompactIndex before v178, int32 after) and, from v343, int32 number.
    [[nodiscard]] FName read_fname(BinaryReader& reader);

    /// @brief Writes a name reference (inverse of read_fname()).
    void write_fname(BinaryWriter& writer, const FName& name);
}
