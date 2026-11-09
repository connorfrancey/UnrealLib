// Fuzz target "compression": zlib and LZO1X decompression of arbitrary blocks into a buffer of a given size, and
// compress -> decompress round trips of the input.

#include <UnrealLib/Serialization/Compression.hpp>

#include "../FuzzTargets.hpp"

UNREALLIB_FUZZ_TARGET(compression)
{
    using namespace UnrealLib;
    if (size < 3) return;
    // Byte 0: method; bytes 1-2: output size (up to 64 KiB); the rest: compressed block.
    const CompressionMethod method = (data[0] & 1) != 0 ? CompressionMethod::Lzo : CompressionMethod::Zlib;
    const std::size_t output_size = static_cast<std::size_t>(data[1]) | (static_cast<std::size_t>(data[2]) << 8);
    const std::span<const std::byte> input = Fuzz::as_bytes(data + 3, size - 3);

    // Round trip first: it must hold for every input, whatever the decompression below does.
    const std::vector<std::byte> packed = compress(method, input);
    std::vector<std::byte> unpacked(input.size());
    decompress(method, packed, unpacked);
    Fuzz::fuzz_check(std::equal(unpacked.begin(), unpacked.end(), input.begin(), input.end()),
                     "decompress(compress(x)) == x");

    std::vector<std::byte> output(output_size);
    decompress(method, input, output);
}
