#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

namespace
{
    SerializationContext ue3_context()
    {
        SerializationContext ctx;
        ctx.file_version = 536;
        ctx.licensee_version = 43;
        return ctx;
    }
}

TEST(BinaryReaderTests, ReadsUInt32)
{
    const std::array<std::byte, 4> data{std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}};
    BinaryReader reader(data);
    EXPECT_EQ(reader.read_u32(), 0x12345678u);
    EXPECT_TRUE(reader.at_end());
}

TEST(BinaryReaderTests, RejectsOutOfBoundsReads)
{
    const std::array<std::byte, 2> data{};
    BinaryReader reader(data);
    EXPECT_THROW((void)reader.read_u32(), SerializationError);
    // A failed read does not move the position.
    EXPECT_EQ(reader.position(), 0u);
    EXPECT_EQ(reader.read_u16(), 0u);
}

TEST(BinaryReaderTests, ReadsAllIntegerWidthsLittleEndian)
{
    const auto data = bytes_of({0xFF, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23,
                                0x01});
    BinaryReader reader(data);
    EXPECT_EQ(reader.read_i8(), -1);
    EXPECT_EQ(reader.read_u16(), 0x1234u);
    EXPECT_EQ(reader.read_u32(), 0x12345678u);
    EXPECT_EQ(reader.read_u64(), 0x0123456789ABCDEFull);
}

TEST(BinaryReaderTests, ReadsSignedValues)
{
    const auto data = bytes_of({0xFE, 0xFF, 0xFD, 0xFF, 0xFF, 0xFF, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    BinaryReader reader(data);
    EXPECT_EQ(reader.read_i16(), -2);
    EXPECT_EQ(reader.read_i32(), -3);
    EXPECT_EQ(reader.read_i64(), -4);
}

TEST(BinaryReaderTests, ReadsBigEndian)
{
    const auto data = bytes_of({0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC});
    SerializationContext ctx;
    ctx.endian = Endian::Big;
    BinaryReader reader(data, ctx);
    EXPECT_EQ(reader.read_u32(), 0x12345678u);
    EXPECT_EQ(reader.read_u16(), 0x9ABCu);
}

TEST(BinaryReaderTests, ReadsFloatingPoint)
{
    // 1.5f = 0x3FC00000, -2.25 = 0xC002000000000000
    const auto data = bytes_of({0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xC0});
    BinaryReader reader(data);
    EXPECT_FLOAT_EQ(reader.read_f32(), 1.5f);
    EXPECT_DOUBLE_EQ(reader.read_f64(), -2.25);
}

TEST(BinaryReaderTests, ReadsGuidAndBool)
{
    const auto data = bytes_of({1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 7, 0, 0, 0});
    BinaryReader reader(data);
    EXPECT_EQ(reader.read_guid(), (Guid{1, 2, 3, 4}));
    EXPECT_TRUE(reader.read_bool32());
}

TEST(BinaryReaderTests, SeekAndSkipAreBoundsChecked)
{
    const auto data = bytes_of({1, 2, 3, 4});
    BinaryReader reader(data);
    reader.seek(4); // end is a valid position
    EXPECT_TRUE(reader.at_end());
    EXPECT_THROW(reader.seek(5), SerializationError);
    reader.seek(1);
    EXPECT_THROW(reader.skip(4), SerializationError);
    reader.skip(3);
    EXPECT_TRUE(reader.at_end());
}

TEST(BinaryReaderTests, ReportsAbsoluteOffsetInErrors)
{
    const auto data = bytes_of({1, 2});
    BinaryReader reader(data, {}, 0x1000);
    reader.skip(1);
    try
    {
        (void)reader.read_u32();
        FAIL() << "expected SerializationError";
    }
    catch (const SerializationError& e)
    {
        EXPECT_EQ(e.code(), ErrorCode::OutOfBounds);
        ASSERT_TRUE(e.offset().has_value());
        EXPECT_EQ(*e.offset(), 0x1001u);
    }
}

TEST(BinaryReaderTests, SubReaderIsBoundedAndKeepsAbsoluteOffsets)
{
    const auto data = bytes_of({0, 1, 2, 3, 4, 5});
    BinaryReader reader(data);
    BinaryReader sub = reader.sub_reader(2, 3);
    EXPECT_EQ(sub.size(), 3u);
    EXPECT_EQ(sub.absolute_position(), 2u);
    EXPECT_EQ(sub.read_u8(), 2u);
    sub.skip(2);
    EXPECT_THROW((void)sub.read_u8(), SerializationError);
    EXPECT_THROW((void)reader.sub_reader(4, 3), SerializationError);
    EXPECT_THROW((void)reader.sub_reader(7, 0), SerializationError);
}

TEST(BinaryReaderTests, ReadViewAndByteVector)
{
    const auto data = bytes_of({9, 8, 7, 6});
    BinaryReader reader(data);
    const auto peek = reader.peek_view(2);
    EXPECT_EQ(peek[0], std::byte{9});
    EXPECT_EQ(reader.position(), 0u);
    const auto view = reader.read_view(2);
    EXPECT_EQ(view[1], std::byte{8});
    const auto copy = reader.read_byte_vector(2);
    EXPECT_EQ(copy, bytes_of({7, 6}));
    EXPECT_THROW((void)reader.read_view(1), SerializationError);
}

TEST(BinaryReaderTests, ByteVectorRespectsAllocationLimit)
{
    std::vector<std::byte> data(64);
    SerializationContext ctx;
    ctx.limits.max_allocation_bytes = 16;
    BinaryReader reader(data, ctx);
    try
    {
        (void)reader.read_byte_vector(32);
        FAIL();
    }
    catch (const SerializationError& e)
    {
        EXPECT_EQ(e.code(), ErrorCode::LimitExceeded);
    }
}

TEST(BinaryReaderTests, ArrayCountValidation)
{
    {
        const auto data = bytes_of({0xFF, 0xFF, 0xFF, 0xFF});
        BinaryReader reader(data);
        EXPECT_THROW((void)reader.read_array_count(), SerializationError); // negative
    }
    {
        // Count 1000 with only 4 bytes remaining (min element size 4).
        const auto data = bytes_of({0xE8, 0x03, 0, 0, 1, 2, 3, 4});
        BinaryReader reader(data);
        EXPECT_THROW((void)reader.read_array_count(4), SerializationError);
    }
    {
        const auto data = bytes_of({0x10, 0, 0, 0});
        SerializationContext ctx;
        ctx.limits.max_array_count = 8;
        BinaryReader reader(data, ctx);
        try
        {
            (void)reader.read_array_count(0);
            FAIL();
        }
        catch (const SerializationError& e)
        {
            EXPECT_EQ(e.code(), ErrorCode::LimitExceeded);
        }
    }
    {
        const auto data = bytes_of({2, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8});
        BinaryReader reader(data);
        EXPECT_EQ(reader.read_array_count(4), 2u);
    }
}

TEST(BinaryReaderTests, ReadIndexDependsOnVersion)
{
    const auto data = bytes_of({0x45, 0x01, 0, 0});
    {
        BinaryReader reader(data, ue3_context());
        EXPECT_EQ(reader.read_index(), 0x145); // int32
    }
    {
        SerializationContext legacy;
        legacy.file_version = 100;
        BinaryReader reader(data, legacy);
        EXPECT_EQ(reader.read_index(), 69); // CompactIndex: 0x45 = more(0x40) | 5, then 0x01 -> 5 | 1 << 6
        EXPECT_EQ(reader.position(), 2u);
    }
}

TEST(BinaryReaderTests, ReadsAnsiNullString)
{
    const auto data = bytes_of({'A', 'b', 0xE9, 0, 'x'});
    BinaryReader reader(data);
    EXPECT_EQ(reader.read_ansi_null_string(), "Ab\xC3\xA9");
    EXPECT_EQ(reader.position(), 4u);
    EXPECT_THROW((void)reader.read_ansi_null_string(), SerializationError); // unterminated
}

TEST(BinaryReaderTests, AnsiNullStringRespectsLengthLimit)
{
    const auto data = bytes_of({'a', 'b', 'c', 'd', 0});
    SerializationContext ctx;
    ctx.limits.max_string_length = 3;
    BinaryReader reader(data, ctx);
    try
    {
        (void)reader.read_ansi_null_string();
        FAIL();
    }
    catch (const SerializationError& e)
    {
        EXPECT_EQ(e.code(), ErrorCode::LimitExceeded);
    }
}
