#pragma once

/// @file
/// @brief Package name table.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Serialization/FString.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    class BinaryReader;
    class BinaryWriter;

    /// @brief One name-table entry.
    struct NameEntry
    {
        FString text;            ///< Name text (with its on-disk string encoding).
        std::uint64_t flags = 0; ///< Object flags associated with the name (32-bit on disk before version 195).

        /// @brief Memberwise comparison.
        friend bool operator==(const NameEntry&, const NameEntry&) = default;
    };

    /// @brief Reads one name-table entry.
    ///
    /// Version >= 64: FString; older: NUL-terminated ANSI string. Flags: uint64 from version 195, uint32 before.
    /// Mirrors upstream UELib UNameTableItem.Deserialize (default branch).
    [[nodiscard]] NameEntry read_name_entry(BinaryReader& reader);

    /// @brief Writes one name-table entry (inverse of read_name_entry()).
    void write_name_entry(BinaryWriter& writer, const NameEntry& entry);

    /// @brief An ordered list of names with case-insensitive lookup.
    ///
    /// Unreal compares names case-insensitively. When a table contains entries that differ only by case (possible
    /// in hand-edited packages), lookup returns the first one.
    class NameTable
    {
    public:
        NameTable() = default;

        /// @brief Builds a table from entries.
        explicit NameTable(std::vector<NameEntry> entries);

        /// @brief Number of entries.
        [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }

        /// @brief Whether the table is empty.
        [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

        /// @brief All entries in table order.
        [[nodiscard]] std::span<const NameEntry> entries() const noexcept { return m_entries; }

        /// @brief Entry at @p index. Throws Error (OutOfBounds) if the index is invalid.
        [[nodiscard]] const NameEntry& at(std::int32_t index) const;

        /// @brief Whether @p index refers to an entry.
        [[nodiscard]] bool contains_index(std::int32_t index) const noexcept
        {
            return index >= 0 && static_cast<std::size_t>(index) < m_entries.size();
        }

        /// @brief Index of the first entry equal (case-insensitively) to @p text.
        [[nodiscard]] std::optional<std::int32_t> find(std::string_view text) const;

        /// @brief Appends @p text if not present; returns its index.
        std::int32_t add(std::string_view text, std::uint64_t flags = 0);

        /// @brief Resolves a name reference to its display string ("Base" or "Base_<number-1>").
        /// @throws Error (OutOfBounds) if the index is invalid.
        [[nodiscard]] std::string to_string(const FName& name) const;

        /// @brief Like to_string() but returns std::nullopt instead of throwing.
        [[nodiscard]] std::optional<std::string> try_to_string(const FName& name) const;

    private:
        void rebuild_index();

        std::vector<NameEntry> m_entries;
        std::unordered_map<std::string, std::int32_t, Text::CaseInsensitiveHash, Text::CaseInsensitiveEqual> m_lookup;
    };
}
