#pragma once

// Internal: LZO1X decompressor.

#include <cstddef>
#include <cstdint>
#include <span>

namespace UnrealLib::detail
{
    // Decodes a complete LZO1X stream (as produced by lzo1x_1_compress / lzo1x_999_compress) from `input` into
    // `output`. Returns the number of bytes produced. Throws CompressionError on any malformed input: truncated
    // input, output overrun, look-behind before the start of the output, missing end-of-stream marker, or trailing
    // input after the end-of-stream marker.
    std::size_t lzo1x_decompress(std::span<const std::uint8_t> input, std::span<std::uint8_t> output);
}
