#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include <bit>
#include <cstring>
#include <array>
#include <limits>
#include <utility>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/CompactIndex.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    BinaryWriter::BinaryWriter(SerializationContext context) noexcept : m_context(context)
    {
    }

    void BinaryWriter::seek(std::size_t position)
    {
        if (position > m_buffer.size())
        {
            throw SerializationError(ErrorCode::OutOfBounds,
                fmt::format("seek to {} is beyond the end of the written data ({} bytes)", position, m_buffer.size()));
        }
        m_position = position;
    }

    std::uint8_t* BinaryWriter::reserve_at_position(std::size_t count)
    {
        if (m_position + count > m_buffer.size())
        {
            m_buffer.resize(m_position + count);
        }
        auto* ptr = reinterpret_cast<std::uint8_t*>(m_buffer.data()) + m_position; // byte -> uint8 view
        m_position += count;
        return ptr;
    }

    void BinaryWriter::write_u8(std::uint8_t value)
    {
        *reserve_at_position(1) = value;
    }

    void BinaryWriter::write_u16(std::uint16_t value)
    {
        store_unsigned(value, reserve_at_position(2), m_context.endian);
    }

    void BinaryWriter::write_u32(std::uint32_t value)
    {
        store_unsigned(value, reserve_at_position(4), m_context.endian);
    }

    void BinaryWriter::write_u64(std::uint64_t value)
    {
        store_unsigned(value, reserve_at_position(8), m_context.endian);
    }

    void BinaryWriter::write_i8(std::int8_t value)
    {
        write_u8(std::bit_cast<std::uint8_t>(value));
    }

    void BinaryWriter::write_i16(std::int16_t value)
    {
        write_u16(std::bit_cast<std::uint16_t>(value));
    }

    void BinaryWriter::write_i32(std::int32_t value)
    {
        write_u32(std::bit_cast<std::uint32_t>(value));
    }

    void BinaryWriter::write_i64(std::int64_t value)
    {
        write_u64(std::bit_cast<std::uint64_t>(value));
    }

    void BinaryWriter::write_f32(float value)
    {
        write_u32(std::bit_cast<std::uint32_t>(value));
    }

    void BinaryWriter::write_f64(double value)
    {
        write_u64(std::bit_cast<std::uint64_t>(value));
    }

    void BinaryWriter::write_bool32(bool value)
    {
        write_u32(value ? 1u : 0u);
    }

    void BinaryWriter::write_guid(const Guid& value)
    {
        write_u32(value.a);
        write_u32(value.b);
        write_u32(value.c);
        write_u32(value.d);
    }

    void BinaryWriter::write_bytes(std::span<const std::byte> bytes)
    {
        auto* dst = reserve_at_position(bytes.size());
        if (!bytes.empty())
        {
            std::memcpy(dst, bytes.data(), bytes.size());
        }
    }

    void BinaryWriter::write_compact_index(std::int32_t value)
    {
        std::array<std::uint8_t, compact_index_max_size> encoded{};
        const std::size_t count = encode_compact_index(value, encoded);
        for (std::size_t i = 0; i < count; ++i)
        {
            write_u8(encoded[i]);
        }
    }

    void BinaryWriter::write_index(std::int32_t value)
    {
        if (m_context.uses_compact_index())
        {
            write_compact_index(value);
        }
        else
        {
            write_i32(value);
        }
    }

    void BinaryWriter::write_fstring(const FString& value)
    {
        const std::size_t terminator = value.null_terminated ? 1 : 0;
        if (value.text.empty() && terminator == 0)
        {
            write_index(0);
            return;
        }

        if (value.encoding == StringEncoding::Ansi)
        {
            if (auto bytes = Text::utf8_to_cp1252(value.text))
            {
                const std::size_t count = bytes->size() + terminator;
                if (count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                {
                    throw SerializationError(ErrorCode::LimitExceeded, "FString too long to serialize");
                }
                write_index(static_cast<std::int32_t>(count));
                for (const std::uint8_t b : *bytes)
                {
                    write_u8(b);
                }
                if (terminator != 0)
                {
                    write_u8(0);
                }
                return;
            }
            // Not representable in Windows-1252: fall through to UTF-16 (matches upstream's automatic switch).
        }

        const auto units = Text::utf8_to_utf16(value.text);
        if (!units)
        {
            throw SerializationError(ErrorCode::InvalidArgument, "FString text is not valid UTF-8");
        }
        const std::size_t count = units->size() + terminator;
        if (count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        {
            throw SerializationError(ErrorCode::LimitExceeded, "FString too long to serialize");
        }
        write_index(-static_cast<std::int32_t>(count));
        for (const char16_t unit : *units)
        {
            write_u16(static_cast<std::uint16_t>(unit));
        }
        if (terminator != 0)
        {
            write_u16(0);
        }
    }

    void BinaryWriter::write_ansi_null_string(std::string_view text)
    {
        const auto bytes = Text::utf8_to_cp1252(text);
        if (!bytes)
        {
            throw SerializationError(ErrorCode::InvalidArgument,
                "string is not representable in Windows-1252 (required for NUL-terminated ANSI strings)");
        }
        for (const std::uint8_t b : *bytes)
        {
            if (b == 0)
            {
                throw SerializationError(ErrorCode::InvalidArgument, "NUL-terminated string contains an embedded NUL");
            }
            write_u8(b);
        }
        write_u8(0);
    }

    void BinaryWriter::patch_u32(std::size_t offset, std::uint32_t value)
    {
        if (offset > m_buffer.size() || m_buffer.size() - offset < 4)
        {
            throw SerializationError(ErrorCode::OutOfBounds,
                fmt::format("patch at {} exceeds the written data ({} bytes)", offset, m_buffer.size()));
        }
        store_unsigned(value, reinterpret_cast<std::uint8_t*>(m_buffer.data()) + offset, m_context.endian);
    }

    void BinaryWriter::patch_i32(std::size_t offset, std::int32_t value)
    {
        patch_u32(offset, std::bit_cast<std::uint32_t>(value));
    }

    std::vector<std::byte> BinaryWriter::take() noexcept
    {
        m_position = 0;
        return std::exchange(m_buffer, {});
    }
}
