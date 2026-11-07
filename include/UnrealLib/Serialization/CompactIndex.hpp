#pragma once

/// @file
/// @brief Unreal CompactIndex (FCompactIndex) variable-length signed integer encoding.
///
/// Byte 0: bit 7 = sign, bit 6 = "more bytes follow", bits 0–5 = lowest 6 magnitude bits.
/// Bytes 1–3: bit 7 = "more bytes follow", bits 0–6 = next 7 magnitude bits.
/// Byte 4 (if present): all 8 bits are used (matches upstream UELib UnrealReader.ReadCompactIndex).
/// Maximum encoded size is 5 bytes. Used for indices in packages older than version 178 (pre-UE3).

#include <array>
#include <cstddef>
#include <cstdint>

namespace UnrealLib
{
    /// @brief Maximum number of bytes in an encoded CompactIndex.
    inline constexpr std::size_t compact_index_max_size = 5;

    /// @brief Encodes @p value as a CompactIndex.
    /// @param value Value to encode; must not be INT32_MIN (its magnitude is not representable — upstream would
    ///              silently overflow, UnrealLib throws SerializationError).
    /// @param out   Receives the encoded bytes.
    /// @return Number of bytes written to @p out (1–5).
    std::size_t encode_compact_index(std::int32_t value, std::array<std::uint8_t, compact_index_max_size>& out);

    /// @brief Number of bytes encode_compact_index() produces for @p value.
    [[nodiscard]] std::size_t compact_index_size(std::int32_t value);
}
