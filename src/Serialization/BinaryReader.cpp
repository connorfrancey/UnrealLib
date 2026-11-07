#include <UnrealLib/Serialization/BinaryReader.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    BinaryReader::BinaryReader(std::span<const std::byte> data, SerializationContext context,
                               std::uint64_t base_offset) noexcept
        : m_data(data), m_base_offset(base_offset), m_context(context)
    {
    }

    void BinaryReader::fail_out_of_bounds(std::size_t requested) const
    {
        throw SerializationError(ErrorCode::OutOfBounds,
            fmt::format("read of {} byte(s) exceeds the data ({} byte(s) remaining of {})", requested, remaining(),
                        m_data.size()),
            absolute_position());
    }

    const std::uint8_t* BinaryReader::require(std::size_t count)
    {
        if (count > remaining())
        {
            fail_out_of_bounds(count);
        }
        const auto* ptr = reinterpret_cast<const std::uint8_t*>(m_data.data()) + m_position; // byte -> uint8 view
        m_position += count;
        return ptr;
    }

    void BinaryReader::seek(std::size_t position)
    {
        if (position > m_data.size())
        {
            throw SerializationError(ErrorCode::OutOfBounds,
                fmt::format("seek to {} is beyond the end of the data ({} bytes)", position, m_data.size()),
                m_base_offset + position);
        }
        m_position = position;
    }

    void BinaryReader::skip(std::size_t count)
    {
        (void)require(count);
    }

    std::uint8_t BinaryReader::read_u8()
    {
        return *require(1);
    }

    std::uint16_t BinaryReader::read_u16()
    {
        return load_unsigned<std::uint16_t>(require(2), m_context.endian);
    }

    std::uint32_t BinaryReader::read_u32()
    {
        return load_unsigned<std::uint32_t>(require(4), m_context.endian);
    }

    std::uint64_t BinaryReader::read_u64()
    {
        return load_unsigned<std::uint64_t>(require(8), m_context.endian);
    }

    std::int8_t BinaryReader::read_i8()
    {
        return std::bit_cast<std::int8_t>(read_u8());
    }

    std::int16_t BinaryReader::read_i16()
    {
        return std::bit_cast<std::int16_t>(read_u16());
    }

    std::int32_t BinaryReader::read_i32()
    {
        return std::bit_cast<std::int32_t>(read_u32());
    }

    std::int64_t BinaryReader::read_i64()
    {
        return std::bit_cast<std::int64_t>(read_u64());
    }

    float BinaryReader::read_f32()
    {
        return std::bit_cast<float>(read_u32());
    }

    double BinaryReader::read_f64()
    {
        return std::bit_cast<double>(read_u64());
    }

    bool BinaryReader::read_bool32()
    {
        return read_u32() != 0;
    }

    Guid BinaryReader::read_guid()
    {
        Guid guid;
        guid.a = read_u32();
        guid.b = read_u32();
        guid.c = read_u32();
        guid.d = read_u32();
        return guid;
    }

    void BinaryReader::read_bytes(std::span<std::byte> out)
    {
        const auto* src = require(out.size());
        if (!out.empty())
        {
            std::memcpy(out.data(), src, out.size());
        }
    }

    std::span<const std::byte> BinaryReader::read_view(std::size_t count)
    {
        const std::size_t start = m_position;
        (void)require(count);
        return m_data.subspan(start, count);
    }

    std::span<const std::byte> BinaryReader::peek_view(std::size_t count) const
    {
        if (count > remaining())
        {
            fail_out_of_bounds(count);
        }
        return m_data.subspan(m_position, count);
    }

    std::vector<std::byte> BinaryReader::read_byte_vector(std::size_t count)
    {
        if (count > m_context.limits.max_allocation_bytes)
        {
            throw SerializationError(ErrorCode::LimitExceeded,
                fmt::format("byte buffer of {} bytes exceeds the allocation limit ({} bytes)", count,
                            m_context.limits.max_allocation_bytes),
                absolute_position());
        }
        const auto view = read_view(count);
        return {view.begin(), view.end()};
    }

    std::int32_t BinaryReader::read_compact_index()
    {
        const std::uint64_t start = absolute_position();
        const std::uint8_t b0 = read_u8();
        std::uint64_t magnitude = 0;
        if ((b0 & 0x40u) != 0)
        {
            const std::uint8_t b1 = read_u8();
            if ((b1 & 0x80u) != 0)
            {
                const std::uint8_t b2 = read_u8();
                if ((b2 & 0x80u) != 0)
                {
                    const std::uint8_t b3 = read_u8();
                    if ((b3 & 0x80u) != 0)
                    {
                        magnitude = read_u8();
                    }
                    magnitude = (magnitude << 7) | (b3 & 0x7Fu);
                }
                magnitude = (magnitude << 7) | (b2 & 0x7Fu);
            }
            magnitude = (magnitude << 7) | (b1 & 0x7Fu);
        }
        magnitude = (magnitude << 6) | (b0 & 0x3Fu);

        if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        {
            throw SerializationError(ErrorCode::InvalidData,
                fmt::format("CompactIndex magnitude {} does not fit in int32", magnitude), start);
        }
        const auto value = static_cast<std::int32_t>(magnitude);
        return (b0 & 0x80u) != 0 ? -value : value;
    }

    std::int32_t BinaryReader::read_index()
    {
        return m_context.uses_compact_index() ? read_compact_index() : read_i32();
    }

    FString BinaryReader::read_fstring()
    {
        const std::uint64_t start = absolute_position();
        const std::int32_t length = read_index();
        if (length == 0)
        {
            return FString{std::string{}, StringEncoding::Ansi, false};
        }
        if (length == std::numeric_limits<std::int32_t>::min())
        {
            throw SerializationError(ErrorCode::InvalidData, "invalid FString length INT32_MIN", start);
        }

        const auto count = static_cast<std::uint32_t>(length < 0 ? -length : length);
        if (count > m_context.limits.max_string_length)
        {
            throw SerializationError(ErrorCode::LimitExceeded,
                fmt::format("FString length {} exceeds the limit ({})", count, m_context.limits.max_string_length),
                start);
        }

        if (length > 0)
        {
            const auto view = read_view(count);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(view.data());
            const bool terminated = bytes[count - 1] == 0;
            const std::size_t text_length = terminated ? count - 1 : count;
            return FString{Text::cp1252_to_utf8({bytes, text_length}), StringEncoding::Ansi, terminated};
        }

        if (static_cast<std::uint64_t>(count) * 2 > remaining())
        {
            fail_out_of_bounds(static_cast<std::size_t>(count) * 2);
        }
        std::u16string units(count, u'\0');
        for (auto& unit : units)
        {
            unit = static_cast<char16_t>(read_u16());
        }
        const bool terminated = units.back() == u'\0';
        if (terminated)
        {
            units.pop_back();
        }
        return FString{Text::utf16_to_utf8(units), StringEncoding::Utf16, terminated};
    }

    std::string BinaryReader::read_string()
    {
        return read_fstring().text;
    }

    std::string BinaryReader::read_ansi_null_string()
    {
        const std::uint64_t start = absolute_position();
        const auto* begin = reinterpret_cast<const std::uint8_t*>(m_data.data()) + m_position;
        const std::size_t limit = std::min<std::size_t>(remaining(), m_context.limits.max_string_length);
        std::size_t length = 0;
        while (length < limit && begin[length] != 0)
        {
            ++length;
        }
        if (length == limit)
        {
            if (limit == remaining())
            {
                throw SerializationError(ErrorCode::OutOfBounds, "unterminated NUL-terminated string", start);
            }
            throw SerializationError(ErrorCode::LimitExceeded, "NUL-terminated string exceeds the length limit", start);
        }
        std::string text = Text::cp1252_to_utf8({begin, length});
        m_position += length + 1;
        return text;
    }

    std::uint32_t BinaryReader::read_array_count(std::size_t min_element_size)
    {
        const std::uint64_t start = absolute_position();
        const std::int32_t count = read_i32();
        if (count < 0)
        {
            throw SerializationError(ErrorCode::InvalidData, fmt::format("negative array count {}", count), start);
        }
        const auto ucount = static_cast<std::uint32_t>(count);
        if (ucount > m_context.limits.max_array_count)
        {
            throw SerializationError(ErrorCode::LimitExceeded,
                fmt::format("array count {} exceeds the limit ({})", ucount, m_context.limits.max_array_count), start);
        }
        if (static_cast<std::uint64_t>(ucount) * min_element_size > remaining())
        {
            throw SerializationError(ErrorCode::OutOfBounds,
                fmt::format("array count {} (>= {} bytes each) exceeds the remaining {} bytes", ucount,
                            min_element_size, remaining()),
                start);
        }
        return ucount;
    }

    BinaryReader BinaryReader::sub_reader(std::size_t offset, std::size_t count) const
    {
        if (offset > m_data.size() || count > m_data.size() - offset)
        {
            throw SerializationError(ErrorCode::OutOfBounds,
                fmt::format("sub-range [{}, +{}) exceeds the data ({} bytes)", offset, count, m_data.size()),
                m_base_offset + offset);
        }
        return BinaryReader(m_data.subspan(offset, count), m_context, m_base_offset + offset);
    }
}
