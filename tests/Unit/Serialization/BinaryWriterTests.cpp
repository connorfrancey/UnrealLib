#include <gtest/gtest.h>

#include <limits>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

TEST(BinaryWriterTests, WritesLittleEndianIntegers)
{
    BinaryWriter writer;
    writer.write_u8(0xAB);
    writer.write_u16(0x1234);
    writer.write_u32(0x12345678);
    writer.write_i64(-2);
    EXPECT_EQ(std::vector<std::byte>(writer.data().begin(), writer.data().end()),
              bytes_of({0xAB, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(BinaryWriterTests, WritesBigEndianIntegers)
{
    SerializationContext ctx;
    ctx.endian = Endian::Big;
    BinaryWriter writer(ctx);
    writer.write_u32(0x12345678);
    EXPECT_EQ(std::vector<std::byte>(writer.data().begin(), writer.data().end()), bytes_of({0x12, 0x34, 0x56, 0x78}));
}

TEST(BinaryWriterTests, SeekOverwritesAndPatches)
{
    BinaryWriter writer;
    writer.write_u32(0);
    writer.write_u32(0xFFFFFFFF);
    writer.patch_u32(0, 0xAABBCCDD);
    EXPECT_EQ(writer.position(), 8u);
    writer.seek(4);
    writer.write_u8(1);
    EXPECT_EQ(writer.size(), 8u);
    EXPECT_THROW(writer.seek(9), SerializationError);
    EXPECT_THROW(writer.patch_u32(5, 1), SerializationError);

    BinaryReader reader(writer.data());
    EXPECT_EQ(reader.read_u32(), 0xAABBCCDDu);
    EXPECT_EQ(reader.read_u32(), 0xFFFFFF01u);
}

TEST(BinaryWriterTests, RoundTripsAllPrimitives)
{
    BinaryWriter writer;
    writer.write_i8(-5);
    writer.write_i16(-300);
    writer.write_i32(std::numeric_limits<std::int32_t>::min());
    writer.write_u64(std::numeric_limits<std::uint64_t>::max());
    writer.write_f32(3.25f);
    writer.write_f64(-1e300);
    writer.write_bool32(true);
    writer.write_guid(Guid{5, 6, 7, 8});

    BinaryReader reader(writer.data());
    EXPECT_EQ(reader.read_i8(), -5);
    EXPECT_EQ(reader.read_i16(), -300);
    EXPECT_EQ(reader.read_i32(), std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(reader.read_u64(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_FLOAT_EQ(reader.read_f32(), 3.25f);
    EXPECT_DOUBLE_EQ(reader.read_f64(), -1e300);
    EXPECT_TRUE(reader.read_bool32());
    EXPECT_EQ(reader.read_guid(), (Guid{5, 6, 7, 8}));
    EXPECT_TRUE(reader.at_end());
}

TEST(BinaryWriterTests, TakeEmptiesWriter)
{
    BinaryWriter writer;
    writer.write_u32(1);
    const auto bytes = writer.take();
    EXPECT_EQ(bytes.size(), 4u);
    EXPECT_EQ(writer.size(), 0u);
    EXPECT_EQ(writer.position(), 0u);
}

TEST(BinaryWriterTests, AnsiNullStringRejectsUnrepresentableText)
{
    BinaryWriter writer;
    writer.write_ansi_null_string("abc");
    EXPECT_EQ(writer.size(), 4u);
    EXPECT_THROW(writer.write_ansi_null_string("\xE4\xB8\xAD"), SerializationError); // U+4E2D
}
