#include <UnrealLib/Serialization/Compression.hpp>

#include <algorithm>
#include <limits>

#include <fmt/format.h>
#include <zlib.h>

#include <UnrealLib/Core/Error.hpp>

#include "Lzo1x.hpp"

namespace UnrealLib
{
    namespace
    {
        std::span<const std::uint8_t> as_u8(std::span<const std::byte> bytes) noexcept
        {
            return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
        }

        std::span<std::uint8_t> as_u8(std::span<std::byte> bytes) noexcept
        {
            return {reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()};
        }

        void inflate_zlib(std::span<const std::byte> input, std::span<std::byte> output)
        {
            constexpr auto max_uint = static_cast<std::size_t>(std::numeric_limits<uInt>::max());
            if (input.size() > max_uint || output.size() > max_uint)
            {
                throw CompressionError("zlib block exceeds 4 GiB");
            }

            z_stream stream{};
            stream.next_in = const_cast<Bytef*>(as_u8(input).data()); // zlib's API is not const-correct
            stream.avail_in = static_cast<uInt>(input.size());
            stream.next_out = as_u8(output).data();
            stream.avail_out = static_cast<uInt>(output.size());

            if (inflateInit(&stream) != Z_OK)
            {
                throw CompressionError("zlib: inflateInit failed");
            }
            const int result = inflate(&stream, Z_FINISH);
            const auto produced = static_cast<std::size_t>(stream.total_out);
            const auto consumed = static_cast<std::size_t>(stream.total_in);
            inflateEnd(&stream);

            if (result != Z_STREAM_END)
            {
                throw CompressionError(fmt::format("zlib: inflate failed (code {}, produced {}/{} bytes)", result,
                                                   produced, output.size()),
                                       consumed);
            }
            if (produced != output.size())
            {
                throw CompressionError(fmt::format("zlib: decompressed {} bytes, expected {}", produced, output.size()));
            }
        }
    }

    std::string_view to_string(CompressionMethod method) noexcept
    {
        switch (method)
        {
        case CompressionMethod::None: return "none";
        case CompressionMethod::Zlib: return "zlib";
        case CompressionMethod::Lzo: return "lzo";
        case CompressionMethod::Lzx: return "lzx";
        }
        return "unknown";
    }

    CompressionMethod compression_method_from_flags(std::uint32_t flags)
    {
        switch (flags)
        {
        case 0: return CompressionMethod::None;
        case static_cast<std::uint32_t>(CompressionFlag::Zlib): return CompressionMethod::Zlib;
        case static_cast<std::uint32_t>(CompressionFlag::Lzo): return CompressionMethod::Lzo;
        case static_cast<std::uint32_t>(CompressionFlag::Lzx): return CompressionMethod::Lzx;
        default:
            throw UnsupportedError(fmt::format("unsupported compression flags 0x{:X}", flags));
        }
    }

    bool can_decompress(CompressionMethod method) noexcept
    {
        return method == CompressionMethod::None || method == CompressionMethod::Zlib ||
               method == CompressionMethod::Lzo;
    }

    void decompress(CompressionMethod method, std::span<const std::byte> input, std::span<std::byte> output)
    {
        switch (method)
        {
        case CompressionMethod::None:
            if (input.size() != output.size())
            {
                throw CompressionError(fmt::format("stored block is {} bytes, expected {}", input.size(),
                                                   output.size()));
            }
            std::copy(input.begin(), input.end(), output.begin());
            return;
        case CompressionMethod::Zlib:
            inflate_zlib(input, output);
            return;
        case CompressionMethod::Lzo:
        {
            const std::size_t produced = detail::lzo1x_decompress(as_u8(input), as_u8(output));
            if (produced != output.size())
            {
                throw CompressionError(fmt::format("LZO1X: decompressed {} bytes, expected {}", produced,
                                                   output.size()));
            }
            return;
        }
        case CompressionMethod::Lzx:
            throw UnsupportedError("LZX decompression is not supported");
        }
        throw UnsupportedError("unknown compression method");
    }

    std::vector<std::byte> compress_zlib(std::span<const std::byte> input, int level)
    {
        if (input.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max()))
        {
            throw CompressionError("zlib input too large");
        }
        uLongf bound = compressBound(static_cast<uLong>(input.size()));
        std::vector<std::byte> output(bound);
        const int result = compress2(as_u8(std::span<std::byte>(output)).data(), &bound,
                                     as_u8(input).data(), static_cast<uLong>(input.size()), level);
        if (result != Z_OK)
        {
            throw CompressionError(fmt::format("zlib: compress2 failed (code {})", result));
        }
        output.resize(bound);
        return output;
    }

    bool can_compress(CompressionMethod method) noexcept
    {
        return method == CompressionMethod::Zlib || method == CompressionMethod::Lzo;
    }

    std::vector<std::byte> compress(CompressionMethod method, std::span<const std::byte> input)
    {
        switch (method)
        {
        case CompressionMethod::Zlib:
            return compress_zlib(input);
        case CompressionMethod::Lzo:
        {
            const std::vector<std::uint8_t> encoded = detail::lzo1x_compress(as_u8(input));
            std::vector<std::byte> out(encoded.size());
            std::transform(encoded.begin(), encoded.end(), out.begin(), [](std::uint8_t b) { return std::byte{b}; });
            return out;
        }
        default:
            throw UnsupportedError(fmt::format("{} compression is not supported", to_string(method)));
        }
    }
}
