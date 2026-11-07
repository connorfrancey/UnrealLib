#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Serialization/CompactIndex.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

namespace
{
    struct CompactIndexCase
    {
        std::int32_t value;
        std::vector<int> encoded;
    };

    class CompactIndexEncodingTest : public ::testing::TestWithParam<CompactIndexCase>
    {
    };
}

// Known encodings, derived by hand from the FCompactIndex layout (and consistent with upstream WriteCompactIndex).
INSTANTIATE_TEST_SUITE_P(
    KnownValues, CompactIndexEncodingTest,
    ::testing::Values(
        CompactIndexCase{0, {0x00}},
        CompactIndexCase{1, {0x01}},
        CompactIndexCase{-1, {0x81}},
        CompactIndexCase{63, {0x3F}},
        CompactIndexCase{-63, {0xBF}},
        CompactIndexCase{64, {0x40, 0x01}},
        CompactIndexCase{-64, {0xC0, 0x01}},
        CompactIndexCase{8191, {0x7F, 0x7F}},
        CompactIndexCase{8192, {0x40, 0x80, 0x01}},
        CompactIndexCase{1048575, {0x7F, 0xFF, 0x7F}},
        CompactIndexCase{1048576, {0x40, 0x80, 0x80, 0x01}},
        CompactIndexCase{134217727, {0x7F, 0xFF, 0xFF, 0x7F}},
        CompactIndexCase{134217728, {0x40, 0x80, 0x80, 0x80, 0x01}},
        CompactIndexCase{std::numeric_limits<std::int32_t>::max(), {0x7F, 0xFF, 0xFF, 0xFF, 0x0F}},
        CompactIndexCase{-std::numeric_limits<std::int32_t>::max(), {0xFF, 0xFF, 0xFF, 0xFF, 0x0F}}),
    [](const ::testing::TestParamInfo<CompactIndexCase>& info) {
        const std::int32_t v = info.param.value;
        return (v < 0 ? std::string("Minus") : std::string()) + std::to_string(v < 0 ? -static_cast<std::int64_t>(v) : v);
    });

TEST_P(CompactIndexEncodingTest, EncodesToKnownBytes)
{
    const auto& param = GetParam();
    BinaryWriter writer;
    writer.write_compact_index(param.value);
    std::vector<int> actual;
    for (const std::byte b : writer.data())
    {
        actual.push_back(static_cast<int>(b));
    }
    EXPECT_EQ(actual, param.encoded);
    EXPECT_EQ(compact_index_size(param.value), param.encoded.size());
}

TEST_P(CompactIndexEncodingTest, DecodesKnownBytes)
{
    const auto& param = GetParam();
    std::vector<std::byte> data;
    for (const int b : param.encoded)
    {
        data.push_back(static_cast<std::byte>(b));
    }
    BinaryReader reader(data);
    EXPECT_EQ(reader.read_compact_index(), param.value);
    EXPECT_TRUE(reader.at_end());
}

TEST(CompactIndexTests, RoundTripsSweep)
{
    std::vector<std::int32_t> values;
    for (std::int64_t v = -70000; v <= 70000; v += 7)
    {
        values.push_back(static_cast<std::int32_t>(v));
    }
    for (int shift = 0; shift < 31; ++shift)
    {
        const std::int32_t p = std::int32_t{1} << shift;
        values.insert(values.end(), {p, p - 1, -p, -(p - 1)});
    }
    BinaryWriter writer;
    for (const auto v : values)
    {
        writer.write_compact_index(v);
    }
    BinaryReader reader(writer.data());
    for (const auto v : values)
    {
        ASSERT_EQ(reader.read_compact_index(), v);
    }
    EXPECT_TRUE(reader.at_end());
}

TEST(CompactIndexTests, RejectsInt32Min)
{
    BinaryWriter writer;
    EXPECT_THROW(writer.write_compact_index(std::numeric_limits<std::int32_t>::min()), SerializationError);
}

TEST(CompactIndexTests, RejectsOverflowingMagnitude)
{
    // Fifth byte 0x10 makes the magnitude 2^31, which does not fit in int32.
    const auto data = bytes_of({0x40, 0x80, 0x80, 0x80, 0x10});
    BinaryReader reader(data);
    EXPECT_THROW((void)reader.read_compact_index(), SerializationError);
}

TEST(CompactIndexTests, TruncatedInputThrows)
{
    for (const auto& data : {bytes_of({0x40}), bytes_of({0x40, 0x80}), bytes_of({0x40, 0x80, 0x80}),
                             bytes_of({0x40, 0x80, 0x80, 0x80})})
    {
        BinaryReader reader(data);
        EXPECT_THROW((void)reader.read_compact_index(), SerializationError);
    }
}

TEST(CompactIndexTests, NegativeZeroDecodesAsZero)
{
    const auto data = bytes_of({0x80});
    BinaryReader reader(data);
    EXPECT_EQ(reader.read_compact_index(), 0);
}
