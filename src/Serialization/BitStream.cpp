#include <UnrealLib/Serialization/BitStream.hpp>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>

namespace UnrealLib
{
    BitReader::BitReader(std::span<const std::byte> data, std::size_t bit_count) : m_data(data)
    {
        const std::size_t available = data.size() * 8;
        if (bit_count == static_cast<std::size_t>(-1))
        {
            bit_count = available;
        }
        if (bit_count > available)
        {
            throw SerializationError(ErrorCode::InvalidArgument,
                fmt::format("bit count {} exceeds the {} bits of data", bit_count, available));
        }
        m_bit_count = bit_count;
    }

    bool BitReader::read_bit()
    {
        if (m_position >= m_bit_count)
        {
            throw SerializationError(ErrorCode::OutOfBounds,
                fmt::format("bit read past the end ({} bits)", m_bit_count), m_position / 8);
        }
        const auto byte = static_cast<std::uint8_t>(m_data[m_position / 8]);
        const bool bit = ((byte >> (m_position % 8)) & 1u) != 0;
        ++m_position;
        return bit;
    }

    std::uint64_t BitReader::read_bits(unsigned count)
    {
        if (count > 64)
        {
            throw SerializationError(ErrorCode::InvalidArgument, "cannot read more than 64 bits at once");
        }
        if (count > bits_remaining())
        {
            throw SerializationError(ErrorCode::OutOfBounds,
                fmt::format("read of {} bits exceeds the {} remaining bits", count, bits_remaining()),
                m_position / 8);
        }
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            if (read_bit())
            {
                value |= std::uint64_t{1} << i;
            }
        }
        return value;
    }

    std::uint32_t BitReader::read_int(std::uint32_t max)
    {
        if (max == 0)
        {
            throw SerializationError(ErrorCode::InvalidArgument, "read_int requires max >= 1");
        }
        std::uint32_t value = 0;
        for (std::uint32_t mask = 1; mask != 0 && static_cast<std::uint64_t>(value) + mask < max; mask <<= 1)
        {
            if (read_bit())
            {
                value |= mask;
            }
        }
        return value;
    }

    void BitWriter::write_bit(bool bit)
    {
        if (m_bit_count % 8 == 0)
        {
            m_buffer.push_back(std::byte{0});
        }
        if (bit)
        {
            m_buffer.back() |= static_cast<std::byte>(1u << (m_bit_count % 8));
        }
        ++m_bit_count;
    }

    void BitWriter::write_bits(std::uint64_t value, unsigned count)
    {
        if (count > 64)
        {
            throw SerializationError(ErrorCode::InvalidArgument, "cannot write more than 64 bits at once");
        }
        for (unsigned i = 0; i < count; ++i)
        {
            write_bit(((value >> i) & 1u) != 0);
        }
    }

    void BitWriter::write_int(std::uint32_t value, std::uint32_t max)
    {
        if (max == 0 || value >= max)
        {
            throw SerializationError(ErrorCode::InvalidArgument,
                fmt::format("write_int value {} is not below max {}", value, max));
        }
        for (std::uint32_t mask = 1; mask != 0 && static_cast<std::uint64_t>(value & (mask - 1)) + mask < max;
             mask <<= 1)
        {
            write_bit((value & mask) != 0);
        }
    }
}
