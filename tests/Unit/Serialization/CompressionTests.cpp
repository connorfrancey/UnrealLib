#include <gtest/gtest.h>

#include <numeric>
#include <random>
#include <string_view>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/Compression.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

namespace
{
    std::vector<std::byte> text_bytes(std::string_view text)
    {
        std::vector<std::byte> out;
        for (const char c : text)
        {
            out.push_back(static_cast<std::byte>(c));
        }
        return out;
    }

    std::vector<std::byte> lzo(const std::vector<std::byte>& input, std::size_t output_size)
    {
        std::vector<std::byte> output(output_size);
        decompress(CompressionMethod::Lzo, input, output);
        return output;
    }
}

TEST(CompressionTests, MethodFromFlags)
{
    EXPECT_EQ(compression_method_from_flags(0), CompressionMethod::None);
    EXPECT_EQ(compression_method_from_flags(1), CompressionMethod::Zlib);
    EXPECT_EQ(compression_method_from_flags(2), CompressionMethod::Lzo);
    EXPECT_EQ(compression_method_from_flags(4), CompressionMethod::Lzx);
    EXPECT_THROW((void)compression_method_from_flags(3), UnsupportedError);
    EXPECT_THROW((void)compression_method_from_flags(0x10), UnsupportedError);
    EXPECT_TRUE(can_decompress(CompressionMethod::Lzo));
    EXPECT_FALSE(can_decompress(CompressionMethod::Lzx));
}

TEST(CompressionTests, ZlibRoundTrip)
{
    std::vector<std::byte> input(100000);
    std::mt19937 rng(1234);
    for (std::size_t i = 0; i < input.size(); ++i)
    {
        // Compressible but not trivial.
        input[i] = static_cast<std::byte>((i % 97) ^ (rng() % 4));
    }
    const auto compressed = compress_zlib(input);
    EXPECT_LT(compressed.size(), input.size());
    std::vector<std::byte> output(input.size());
    decompress(CompressionMethod::Zlib, compressed, output);
    EXPECT_EQ(output, input);
}

TEST(CompressionTests, ZlibRejectsWrongSizeAndCorruption)
{
    const auto input = text_bytes("hello hello hello hello");
    const auto compressed = compress_zlib(input);

    std::vector<std::byte> too_small(input.size() - 1);
    EXPECT_THROW(decompress(CompressionMethod::Zlib, compressed, too_small), CompressionError);
    std::vector<std::byte> too_large(input.size() + 1);
    EXPECT_THROW(decompress(CompressionMethod::Zlib, compressed, too_large), CompressionError);

    auto corrupt = compressed;
    corrupt[corrupt.size() / 2] ^= std::byte{0x5A};
    std::vector<std::byte> output(input.size());
    EXPECT_THROW(decompress(CompressionMethod::Zlib, corrupt, output), CompressionError);

    std::vector<std::byte> truncated(compressed.begin(), compressed.begin() + 4);
    EXPECT_THROW(decompress(CompressionMethod::Zlib, truncated, output), CompressionError);
}

TEST(CompressionTests, LzxIsUnsupported)
{
    std::vector<std::byte> output(4);
    EXPECT_THROW(decompress(CompressionMethod::Lzx, bytes_of({1, 2, 3}), output), UnsupportedError);
}

TEST(CompressionTests, StoredBlockMustMatchSize)
{
    std::vector<std::byte> output(3);
    decompress(CompressionMethod::None, bytes_of({7, 8, 9}), output);
    EXPECT_EQ(output, bytes_of({7, 8, 9}));
    EXPECT_THROW(decompress(CompressionMethod::None, bytes_of({7, 8}), output), CompressionError);
}

// ---- LZO1X: hand-assembled streams (see the instruction table in src/Serialization/Lzo1x.cpp) ----

TEST(LzoTests, LiteralOnlyStream)
{
    // 17+5 => 5 literals; 0x11 0x00 0x00 => end of stream
    const auto stream = bytes_of({22, 'h', 'e', 'l', 'l', 'o', 0x11, 0x00, 0x00});
    EXPECT_EQ(lzo(stream, 5), text_bytes("hello"));
}

TEST(LzoTests, M2MatchWithOverlap)
{
    // "abc" literal, then M2: length 6, distance 3 => "abcabcabc".
    // t = (len-1) << 5 | ((dist-1) & 7) << 2 = 5<<5 | 2<<2 = 0xA8, next byte = (dist-1) >> 3 = 0.
    const auto stream = bytes_of({20, 'a', 'b', 'c', 0xA8, 0x00, 0x11, 0x00, 0x00});
    EXPECT_EQ(lzo(stream, 9), text_bytes("abcabcabc"));
}

TEST(LzoTests, M2MatchWithTrailingLiterals)
{
    // As above but low 2 bits of t = 2 trailing literals "xy", then end marker.
    const auto stream = bytes_of({20, 'a', 'b', 'c', 0xAA, 0x00, 'x', 'y', 0x11, 0x00, 0x00});
    EXPECT_EQ(lzo(stream, 11), text_bytes("abcabcabcxy"));
}

TEST(LzoTests, M3MatchWithExtendedLength)
{
    // 4 literals "abcd" via first-byte form, then M3 with extended length:
    // t = 0x20 (length field 0) -> length = 31 + next(=10) = 41, copy 41 + 2 = 43 bytes, distance 4:
    // LE16 = (dist-1) << 2 = 12 -> bytes 0x0C 0x00.
    const auto stream = bytes_of({21, 'a', 'b', 'c', 'd', 0x20, 10, 0x0C, 0x00, 0x11, 0x00, 0x00});
    std::string expected = "abcd";
    for (int i = 0; i < 43; ++i)
    {
        expected.push_back("abcd"[i % 4]);
    }
    EXPECT_EQ(lzo(stream, expected.size()), text_bytes(expected));
}

TEST(LzoTests, LiteralRunInstructionAndFirstRunMatch)
{
    // First byte form needs >= 4 literals; use instruction form instead:
    // t = 0x01 => 1 + 3 = 4 literals "wxyz";
    // then (after a literal run) t < 16 => 3-byte match at distance 0x801 + (t>>2) + (next<<2): needs >= 0x801 bytes
    // of history, so here we use an M2 instead: dist 4, len 3: t = 2<<5 | 3<<2 = 0x4C, next 0.
    const auto stream = bytes_of({0x01, 'w', 'x', 'y', 'z', 0x4C, 0x00, 0x11, 0x00, 0x00});
    EXPECT_EQ(lzo(stream, 7), text_bytes("wxyzwxy"));
}

TEST(LzoTests, LongLiteralRunWithExtendedLength)
{
    // t = 0 => extended: 15 + next(=2) => 17, +3 => 20 literals
    std::vector<std::byte> stream = bytes_of({0x00, 0x02});
    std::string literals = "0123456789ABCDEFGHIJ";
    for (const char c : literals)
    {
        stream.push_back(static_cast<std::byte>(c));
    }
    for (const int b : {0x11, 0x00, 0x00})
    {
        stream.push_back(static_cast<std::byte>(b));
    }
    EXPECT_EQ(lzo(stream, 20), text_bytes(literals));
}

TEST(LzoTests, RejectsMatchBeforeStartOfOutput)
{
    // M2 with distance 8 after only 3 bytes of output.
    const auto stream = bytes_of({20, 'a', 'b', 'c', 0x5C, 0x00, 0x11, 0x00, 0x00});
    EXPECT_THROW((void)lzo(stream, 6), CompressionError);
}

TEST(LzoTests, RejectsOutputOverrun)
{
    const auto stream = bytes_of({22, 'h', 'e', 'l', 'l', 'o', 0x11, 0x00, 0x00});
    EXPECT_THROW((void)lzo(stream, 4), CompressionError);
}

TEST(LzoTests, RejectsShortOutput)
{
    const auto stream = bytes_of({22, 'h', 'e', 'l', 'l', 'o', 0x11, 0x00, 0x00});
    EXPECT_THROW((void)lzo(stream, 6), CompressionError);
}

TEST(LzoTests, RejectsTruncationAtEveryPoint)
{
    const auto stream = bytes_of({20, 'a', 'b', 'c', 0xAA, 0x00, 'x', 'y', 0x11, 0x00, 0x00});
    for (std::size_t length = 0; length < stream.size(); ++length)
    {
        const std::vector<std::byte> truncated(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(length));
        EXPECT_THROW((void)lzo(truncated, 11), CompressionError) << "length " << length;
    }
}

TEST(LzoTests, RejectsTrailingData)
{
    const auto stream = bytes_of({22, 'h', 'e', 'l', 'l', 'o', 0x11, 0x00, 0x00, 0x00});
    EXPECT_THROW((void)lzo(stream, 5), CompressionError);
}

TEST(LzoTests, RandomGarbageNeverCrashes)
{
    std::mt19937 rng(42);
    std::vector<std::byte> output(4096);
    for (int iteration = 0; iteration < 5000; ++iteration)
    {
        std::vector<std::byte> input(1 + rng() % 64);
        for (auto& b : input)
        {
            b = static_cast<std::byte>(rng());
        }
        try
        {
            decompress(CompressionMethod::Lzo, input, output);
        }
        catch (const CompressionError&)
        {
            // expected for almost every input
        }
    }
    SUCCEED();
}
