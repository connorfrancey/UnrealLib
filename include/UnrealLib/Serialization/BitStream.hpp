#pragma once

/// @file
/// @brief Bit-level reader and writer (Unreal FBitReader / FBitWriter bit order: LSB-first within each byte).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace UnrealLib
{
    /// @brief Reads individual bits from a byte range, least-significant bit of each byte first.
    ///
    /// All reads are bounds-checked against bit_count() and throw SerializationError on overrun.
    class BitReader
    {
    public:
        /// @param data     Source bytes (not owned).
        /// @param bit_count Number of valid bits; defaults to all bits of @p data. Must not exceed `data.size() * 8`.
        explicit BitReader(std::span<const std::byte> data, std::size_t bit_count = static_cast<std::size_t>(-1));

        [[nodiscard]] std::size_t bit_count() const noexcept { return m_bit_count; }  ///< Total valid bits.
        [[nodiscard]] std::size_t bit_position() const noexcept { return m_position; } ///< Bits consumed.
        [[nodiscard]] std::size_t bits_remaining() const noexcept { return m_bit_count - m_position; } ///< Bits left.

        /// @brief Reads one bit.
        bool read_bit();

        /// @brief Reads @p count bits (0–64) into the low bits of the result; the first bit read is bit 0.
        std::uint64_t read_bits(unsigned count);

        /// @brief Reads a value in `[0, max)` using Unreal's FBitReader::SerializeInt encoding.
        ///
        /// Bits are read LSB-first while `value + mask < max`, so exactly ceil(log2(max)) bits or fewer are consumed.
        /// @param max Exclusive upper bound; must be >= 1.
        std::uint32_t read_int(std::uint32_t max);

    private:
        std::span<const std::byte> m_data;
        std::size_t m_bit_count = 0;
        std::size_t m_position = 0;
    };

    /// @brief Writes individual bits into a growable buffer, least-significant bit of each byte first.
    class BitWriter
    {
    public:
        /// @brief Appends one bit.
        void write_bit(bool bit);

        /// @brief Appends the low @p count bits (0–64) of @p value, bit 0 first.
        void write_bits(std::uint64_t value, unsigned count);

        /// @brief Appends @p value (must be < @p max) using Unreal's FBitWriter::SerializeInt encoding.
        void write_int(std::uint32_t value, std::uint32_t max);

        [[nodiscard]] std::size_t bit_count() const noexcept { return m_bit_count; } ///< Bits written.

        /// @brief The written bytes; unused high bits of the final byte are zero.
        [[nodiscard]] std::span<const std::byte> data() const noexcept { return m_buffer; }

    private:
        std::vector<std::byte> m_buffer;
        std::size_t m_bit_count = 0;
    };
}
