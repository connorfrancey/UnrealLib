#pragma once

/// @file
/// @brief Byte-order primitives.

#include <cstddef>
#include <concepts>
#include <cstdint>

namespace UnrealLib
{
    /// @brief Byte order of serialized data.
    enum class Endian : std::uint8_t
    {
        Little, ///< Little-endian (PC packages).
        Big,    ///< Big-endian (console packages; supported by the archive layer, not yet verified with fixtures).
    };

    /// @brief Reverses the byte order of an unsigned integer (C++20 replacement for C++23 std::byteswap).
    template <std::unsigned_integral T>
    [[nodiscard]] constexpr T byteswap(T value) noexcept
    {
        if constexpr (sizeof(T) == 1)
        {
            return value;
        }
        else
        {
            T result = 0;
            for (std::size_t i = 0; i < sizeof(T); ++i)
            {
                result = static_cast<T>((result << 8) | (value & 0xFFu));
                value = static_cast<T>(value >> 8);
            }
            return result;
        }
    }

    /// @brief Assembles an unsigned integer from bytes stored in @p order.
    template <std::unsigned_integral T>
    [[nodiscard]] constexpr T load_unsigned(const std::uint8_t* bytes, Endian order) noexcept
    {
        T result = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            const std::size_t index = order == Endian::Little ? sizeof(T) - 1 - i : i;
            result = static_cast<T>((result << 8) | bytes[index]);
        }
        return result;
    }

    /// @brief Stores an unsigned integer as bytes in @p order.
    template <std::unsigned_integral T>
    constexpr void store_unsigned(T value, std::uint8_t* bytes, Endian order) noexcept
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            const std::size_t index = order == Endian::Little ? i : sizeof(T) - 1 - i;
            bytes[index] = static_cast<std::uint8_t>(value & 0xFFu);
            value = static_cast<T>(value >> 8);
        }
    }
}
