#pragma once

/// @file
/// @brief Block (de)compression used by Unreal Engine 3 packages and bulk data.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace UnrealLib
{
    /// @brief UE3 ECompressionFlags bits as stored in the package summary / bulk-data flags.
    /// Values from upstream UELib `UnrealFlags.cs` (enum CompressionFlags).
    enum class CompressionFlag : std::uint32_t
    {
        Zlib = 0x1, ///< zlib (deflate with zlib header).
        Lzo = 0x2,  ///< LZO1X — the method used by Mirror's Edge PC (verified on 1553 cooked packages).
        Lzx = 0x4,  ///< Xbox 360 LZX (not supported).
    };

    /// @brief A single compression algorithm.
    enum class CompressionMethod : std::uint8_t
    {
        None,
        Zlib,
        Lzo,
        Lzx,
    };

    /// @brief Human-readable method name ("none", "zlib", "lzo", "lzx").
    [[nodiscard]] std::string_view to_string(CompressionMethod method) noexcept;

    /// @brief Maps package compression flags to a single method.
    ///
    /// 0 maps to CompressionMethod::None. Exactly one of the known method bits must be set otherwise; any other
    /// combination throws UnsupportedError.
    [[nodiscard]] CompressionMethod compression_method_from_flags(std::uint32_t flags);

    /// @brief Whether UnrealLib can decompress @p method.
    [[nodiscard]] bool can_decompress(CompressionMethod method) noexcept;

    /// @brief Decompresses @p input into @p output.
    ///
    /// The output size must be known in advance (Unreal always stores it). The call fails unless the compressed
    /// stream decodes to exactly `output.size()` bytes and is fully consumed (trailing bytes are rejected for LZO;
    /// zlib must reach its end-of-stream marker).
    ///
    /// @throws CompressionError on malformed data or size mismatch.
    /// @throws UnsupportedError for CompressionMethod::Lzx.
    void decompress(CompressionMethod method, std::span<const std::byte> input, std::span<std::byte> output);

    /// @brief Compresses @p input with zlib (used for writing and tests).
    /// @param input Bytes to compress.
    /// @param level zlib level 0–9, or -1 for the default.
    [[nodiscard]] std::vector<std::byte> compress_zlib(std::span<const std::byte> input, int level = -1);
}
