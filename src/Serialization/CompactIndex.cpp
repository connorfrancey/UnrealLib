#include <UnrealLib/Serialization/CompactIndex.hpp>

#include <limits>

#include <UnrealLib/Core/Error.hpp>

namespace UnrealLib
{
    std::size_t encode_compact_index(std::int32_t value, std::array<std::uint8_t, compact_index_max_size>& out)
    {
        if (value == std::numeric_limits<std::int32_t>::min())
        {
            throw SerializationError(ErrorCode::InvalidArgument, "CompactIndex cannot encode INT32_MIN");
        }

        const bool negative = value < 0;
        auto magnitude = static_cast<std::uint32_t>(negative ? -value : value);

        std::size_t count = 0;
        std::uint8_t b0 = static_cast<std::uint8_t>(magnitude & 0x3Fu);
        if (negative)
        {
            b0 |= 0x80u;
        }
        magnitude >>= 6;
        if (magnitude != 0)
        {
            b0 |= 0x40u;
        }
        out[count++] = b0;

        // Bytes 1..3 carry 7 bits each with a continuation bit; byte 4 carries the remaining bits verbatim.
        while (magnitude != 0 && count < 4)
        {
            auto b = static_cast<std::uint8_t>(magnitude & 0x7Fu);
            magnitude >>= 7;
            if (magnitude != 0)
            {
                b |= 0x80u;
            }
            out[count++] = b;
        }
        if (magnitude != 0)
        {
            out[count++] = static_cast<std::uint8_t>(magnitude);
        }
        return count;
    }

    std::size_t compact_index_size(std::int32_t value)
    {
        std::array<std::uint8_t, compact_index_max_size> buffer{};
        return encode_compact_index(value, buffer);
    }
}
