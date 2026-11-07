#include <gtest/gtest.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BitStream.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

TEST(BitStreamTests, ReadsLsbFirst)
{
    const auto data = bytes_of({0b10110001, 0b00000001});
    BitReader reader(data);
    EXPECT_TRUE(reader.read_bit());
    EXPECT_FALSE(reader.read_bit());
    EXPECT_FALSE(reader.read_bit());
    EXPECT_FALSE(reader.read_bit());
    EXPECT_EQ(reader.read_bits(4), 0b1011u);
    EXPECT_EQ(reader.read_bits(1), 1u);
    EXPECT_EQ(reader.bits_remaining(), 7u);
}

TEST(BitStreamTests, RespectsBitCount)
{
    const auto data = bytes_of({0xFF});
    BitReader reader(data, 3);
    EXPECT_EQ(reader.read_bits(3), 0b111u);
    EXPECT_THROW((void)reader.read_bit(), SerializationError);
    EXPECT_THROW(BitReader(data, 9), SerializationError);
}

TEST(BitStreamTests, ReadBitsRejectsOverrunWithoutConsuming)
{
    const auto data = bytes_of({0xAA});
    BitReader reader(data);
    EXPECT_THROW((void)reader.read_bits(9), SerializationError);
    EXPECT_EQ(reader.bit_position(), 0u);
    EXPECT_THROW((void)reader.read_bits(65), SerializationError);
}

TEST(BitStreamTests, WriterRoundTrip)
{
    BitWriter writer;
    writer.write_bit(true);
    writer.write_bits(0x2D, 6);
    writer.write_bits(0x123456789ull, 36);
    writer.write_bit(false);
    EXPECT_EQ(writer.bit_count(), 44u);
    EXPECT_EQ(writer.data().size(), 6u);

    BitReader reader(writer.data(), writer.bit_count());
    EXPECT_TRUE(reader.read_bit());
    EXPECT_EQ(reader.read_bits(6), 0x2Du);
    EXPECT_EQ(reader.read_bits(36), 0x123456789ull);
    EXPECT_FALSE(reader.read_bit());
    EXPECT_EQ(reader.bits_remaining(), 0u);
}

class BitStreamSerializeIntTest : public ::testing::TestWithParam<std::uint32_t>
{
};

INSTANTIATE_TEST_SUITE_P(Maxima, BitStreamSerializeIntTest,
                         ::testing::Values(1u, 2u, 3u, 5u, 8u, 13u, 64u, 100u, 255u, 256u, 1000u, 65536u));

TEST_P(BitStreamSerializeIntTest, RoundTripsEveryValueBelowMax)
{
    const std::uint32_t max = GetParam();
    const std::uint32_t step = max > 2000 ? max / 997 : 1;
    BitWriter writer;
    for (std::uint32_t v = 0; v < max; v += step)
    {
        writer.write_int(v, max);
    }
    BitReader reader(writer.data(), writer.bit_count());
    for (std::uint32_t v = 0; v < max; v += step)
    {
        ASSERT_EQ(reader.read_int(max), v) << "max " << max;
    }
    EXPECT_EQ(reader.bits_remaining(), 0u);
}

TEST(BitStreamTests, SerializeIntUsesMinimalBits)
{
    BitWriter writer;
    writer.write_int(0, 1); // max 1: no bits needed
    EXPECT_EQ(writer.bit_count(), 0u);
    writer.write_int(3, 4); // 2 bits
    EXPECT_EQ(writer.bit_count(), 2u);
    EXPECT_THROW(writer.write_int(4, 4), SerializationError);
    EXPECT_THROW(writer.write_int(0, 0), SerializationError);
}
