#include <UnrealLib/Names/NameTable.hpp>

#include <limits>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

namespace UnrealLib
{
    NameEntry read_name_entry(BinaryReader& reader)
    {
        NameEntry entry;
        if (reader.context().at_least(LegacyVersion::Release64))
        {
            entry.text = reader.read_fstring();
        }
        else
        {
            entry.text = FString{reader.read_ansi_null_string(), StringEncoding::Ansi, true};
        }

        if (reader.context().at_least(LegacyVersion::ObjectFlagsSizeExpandedTo64Bits))
        {
            entry.flags = reader.read_u64();
        }
        else
        {
            entry.flags = reader.read_u32();
        }
        return entry;
    }

    void write_name_entry(BinaryWriter& writer, const NameEntry& entry)
    {
        if (writer.context().at_least(LegacyVersion::Release64))
        {
            writer.write_fstring(entry.text);
        }
        else
        {
            writer.write_ansi_null_string(entry.text.text);
        }

        if (writer.context().at_least(LegacyVersion::ObjectFlagsSizeExpandedTo64Bits))
        {
            writer.write_u64(entry.flags);
        }
        else
        {
            if (entry.flags > std::numeric_limits<std::uint32_t>::max())
            {
                throw SerializationError(ErrorCode::InvalidArgument,
                    "name flags exceed 32 bits but the target version stores 32-bit flags");
            }
            writer.write_u32(static_cast<std::uint32_t>(entry.flags));
        }
    }

    NameTable::NameTable(std::vector<NameEntry> entries) : m_entries(std::move(entries))
    {
        rebuild_index();
    }

    void NameTable::rebuild_index()
    {
        m_lookup.clear();
        m_lookup.reserve(m_entries.size());
        for (std::size_t i = 0; i < m_entries.size(); ++i)
        {
            m_lookup.try_emplace(m_entries[i].text.text, static_cast<std::int32_t>(i)); // keeps the first duplicate
        }
    }

    const NameEntry& NameTable::at(std::int32_t index) const
    {
        if (!contains_index(index))
        {
            throw Error(ErrorCode::OutOfBounds,
                fmt::format("name index {} is out of range (table has {} entries)", index, m_entries.size()));
        }
        return m_entries[static_cast<std::size_t>(index)];
    }

    std::optional<std::int32_t> NameTable::find(std::string_view text) const
    {
        if (const auto it = m_lookup.find(text); it != m_lookup.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    std::int32_t NameTable::add(std::string_view text, std::uint64_t flags)
    {
        if (const auto existing = find(text))
        {
            return *existing;
        }
        if (m_entries.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        {
            throw Error(ErrorCode::LimitExceeded, "name table is full");
        }
        const auto index = static_cast<std::int32_t>(m_entries.size());
        m_entries.push_back(NameEntry{FString{std::string(text)}, flags});
        m_lookup.try_emplace(std::string(text), index);
        return index;
    }

    std::optional<std::string> NameTable::try_to_string(const FName& name) const
    {
        if (!contains_index(name.index))
        {
            return std::nullopt;
        }
        const std::string& base = m_entries[static_cast<std::size_t>(name.index)].text.text;
        if (!name.has_number())
        {
            return base;
        }
        return fmt::format("{}_{}", base, name.number - 1);
    }

    std::string NameTable::to_string(const FName& name) const
    {
        if (auto text = try_to_string(name))
        {
            return std::move(*text);
        }
        throw Error(ErrorCode::OutOfBounds,
            fmt::format("name index {} is out of range (table has {} entries)", name.index, m_entries.size()));
    }
}
