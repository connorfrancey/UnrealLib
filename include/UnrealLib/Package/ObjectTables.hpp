#pragma once

/// @file
/// @brief Import / export table entries and object indices.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <UnrealLib/Core/Guid.hpp>
#include <UnrealLib/Names/FName.hpp>

namespace UnrealLib
{
    class BinaryReader;
    class BinaryWriter;

    /// @brief A serialized object reference (UPackageIndex / PACKAGE_INDEX).
    ///
    /// 0 = null, N > 0 = export N-1, N < 0 = import -N-1.
    struct ObjectIndex
    {
        std::int32_t value = 0; ///< Raw serialized value.

        [[nodiscard]] constexpr bool is_null() const noexcept { return value == 0; }   ///< References nothing.
        [[nodiscard]] constexpr bool is_export() const noexcept { return value > 0; }  ///< References an export.
        [[nodiscard]] constexpr bool is_import() const noexcept { return value < 0; }  ///< References an import.

        /// @brief Zero-based export-table index. Precondition: is_export().
        [[nodiscard]] constexpr std::size_t export_index() const noexcept { return static_cast<std::size_t>(value) - 1; }

        /// @brief Zero-based import-table index. Precondition: is_import().
        [[nodiscard]] constexpr std::size_t import_index() const noexcept
        {
            return static_cast<std::size_t>(-(static_cast<std::int64_t>(value) + 1));
        }

        /// @brief Reference to export-table entry @p index.
        [[nodiscard]] static constexpr ObjectIndex from_export(std::size_t index) noexcept
        {
            return {static_cast<std::int32_t>(index + 1)};
        }

        /// @brief Reference to import-table entry @p index.
        [[nodiscard]] static constexpr ObjectIndex from_import(std::size_t index) noexcept
        {
            return {-static_cast<std::int32_t>(index) - 1};
        }

        /// @brief Memberwise comparison.
        friend constexpr auto operator<=>(const ObjectIndex&, const ObjectIndex&) noexcept = default;
    };

    /// @brief Export flag bits (ExportEntry::export_flags). Value from upstream UELib `UnrealFlags.cs` (ExportFlags).
    namespace ExportFlag
    {
        /// The export is a copy, made by seek-free cooking, of an object from another package. Top-level forced
        /// exports are UPackage stand-ins for those original packages (upstream `UnrealPackage.CreateObject`); objects
        /// and imports nested under them belong to the original package. Seen in every Mirror's Edge cooked map.
        inline constexpr std::uint32_t ForcedExport = 0x00000001u;
    }

    /// @brief One import-table entry (FObjectImport). Layout per upstream UImportTableItem.Deserialize.
    struct ImportEntry
    {
        FName class_package;   ///< Package containing the class (e.g. "Core").
        FName class_name;      ///< Class name (e.g. "Class", "Package").
        ObjectIndex outer;     ///< Outer object (always a 32-bit value on disk).
        FName object_name;     ///< Object name.

        /// @brief Memberwise comparison.
        friend bool operator==(const ImportEntry&, const ImportEntry&) = default;
    };

    /// @brief One (name, object) pair of an export's component map (versions 178..542).
    struct ComponentMapEntry
    {
        FName name;         ///< Component name.
        ObjectIndex object; ///< Component object.

        /// @brief Memberwise comparison.
        friend bool operator==(const ComponentMapEntry&, const ComponentMapEntry&) = default;
    };

    /// @brief One export-table entry (FObjectExport). Layout per upstream UExportTableItem.Deserialize (default
    ///        branch).
    struct ExportEntry
    {
        ObjectIndex class_index;  ///< Class (null = this export is a UClass).
        ObjectIndex super_index;  ///< Super struct/class.
        ObjectIndex outer;        ///< Outer object (always a 32-bit value on disk).
        FName object_name;        ///< Object name.
        ObjectIndex archetype;    ///< Archetype (>= 220).

        /// Object flags, exactly as stored (64-bit from version 195). Upstream UELib swaps the two 32-bit halves when
        /// exposing this value; UnrealLib keeps the raw on-disk value.
        std::uint64_t object_flags = 0;

        std::int32_t serial_size = 0;   ///< Size of the export's serialized data.
        std::int32_t serial_offset = 0; ///< Offset of the export's data (decompressed package).
        std::vector<ComponentMapEntry> component_map; ///< (178 <= version < 543)
        std::uint32_t export_flags = 0;               ///< (>= 247)
        std::vector<std::int32_t> generation_net_object_count; ///< (>= 322)
        Guid package_guid;             ///< (>= 322)
        std::uint32_t package_flags = 0; ///< (>= 475)

        /// @brief Memberwise comparison.
        friend bool operator==(const ExportEntry&, const ExportEntry&) = default;
    };

    /// @brief Reads one import entry.
    [[nodiscard]] ImportEntry read_import_entry(BinaryReader& reader);
    /// @brief Writes one import entry.
    void write_import_entry(BinaryWriter& writer, const ImportEntry& entry);

    /// @brief Reads one export entry.
    [[nodiscard]] ExportEntry read_export_entry(BinaryReader& reader);
    /// @brief Writes one export entry.
    void write_export_entry(BinaryWriter& writer, const ExportEntry& entry);
}
