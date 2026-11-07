#include <gtest/gtest.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

namespace
{
    SerializationContext ue3()
    {
        SerializationContext ctx;
        ctx.file_version = 536;
        return ctx;
    }

    std::vector<std::byte> write(const FString& s, SerializationContext ctx = ue3())
    {
        BinaryWriter writer(ctx);
        writer.write_fstring(s);
        return writer.take();
    }
}

TEST(FStringTests, ReadsAnsiString)
{
    const auto data = bytes_of({5, 0, 0, 0, 'N', 'o', 'n', 'e', 0});
    BinaryReader reader(data, ue3());
    const FString s = reader.read_fstring();
    EXPECT_EQ(s.text, "None");
    EXPECT_EQ(s.encoding, StringEncoding::Ansi);
    EXPECT_TRUE(s.null_terminated);
    EXPECT_TRUE(reader.at_end());
}

TEST(FStringTests, ReadsUtf16String)
{
    // length -3: 'h', U+00E9, NUL
    const auto data = bytes_of({0xFD, 0xFF, 0xFF, 0xFF, 'h', 0, 0xE9, 0, 0, 0});
    BinaryReader reader(data, ue3());
    const FString s = reader.read_fstring();
    EXPECT_EQ(s.text, "h\xC3\xA9");
    EXPECT_EQ(s.encoding, StringEncoding::Utf16);
    EXPECT_TRUE(s.null_terminated);
}

TEST(FStringTests, ReadsEmptyString)
{
    const auto data = bytes_of({0, 0, 0, 0});
    BinaryReader reader(data, ue3());
    const FString s = reader.read_fstring();
    EXPECT_TRUE(s.text.empty());
    EXPECT_FALSE(s.null_terminated);
    EXPECT_EQ(write(s), data);
}

TEST(FStringTests, DecodesWindows1252)
{
    // 0x80 = EURO SIGN, 0xE9 = LATIN SMALL E WITH ACUTE
    const auto data = bytes_of({3, 0, 0, 0, 0x80, 0xE9, 0});
    BinaryReader reader(data, ue3());
    EXPECT_EQ(reader.read_string(), "\xE2\x82\xAC\xC3\xA9");
}

TEST(FStringTests, KeepsStringsWithoutTerminator)
{
    const auto data = bytes_of({2, 0, 0, 0, 'a', 'b'});
    BinaryReader reader(data, ue3());
    const FString s = reader.read_fstring();
    EXPECT_EQ(s.text, "ab");
    EXPECT_FALSE(s.null_terminated);
    EXPECT_EQ(write(s), data);
}

TEST(FStringTests, RoundTripsByteExact)
{
    const std::vector<std::vector<std::byte>> inputs = {
        bytes_of({5, 0, 0, 0, 'N', 'o', 'n', 'e', 0}),
        bytes_of({3, 0, 0, 0, 0x80, 0xE9, 0}),
        bytes_of({0xFD, 0xFF, 0xFF, 0xFF, 'h', 0, 0xE9, 0, 0, 0}),
        bytes_of({0xFE, 0xFF, 0xFF, 0xFF, 0x00, 0xD8, 0, 0}), // unpaired high surrogate (WTF-8 preserved)
        bytes_of({0xFD, 0xFF, 0xFF, 0xFF, 0x3D, 0xD8, 0x00, 0xDE, 0, 0}), // U+1F600 as a surrogate pair
        bytes_of({1, 0, 0, 0, 0}),
    };
    for (const auto& input : inputs)
    {
        BinaryReader reader(input, ue3());
        const FString s = reader.read_fstring();
        EXPECT_EQ(write(s), input);
    }
}

TEST(FStringTests, WriterSwitchesToUtf16ForUnrepresentableText)
{
    const auto bytes = write(FString("\xE4\xB8\xAD")); // U+4E2D
    EXPECT_EQ(bytes, bytes_of({0xFE, 0xFF, 0xFF, 0xFF, 0x2D, 0x4E, 0, 0}));
}

TEST(FStringTests, WriterRejectsInvalidUtf8)
{
    BinaryWriter writer(ue3());
    EXPECT_THROW(writer.write_fstring(FString(std::string("\xFF\xFE"), StringEncoding::Utf16, true)),
                 SerializationError);
}

TEST(FStringTests, RejectsTruncatedPayload)
{
    const auto ansi_data = bytes_of({10, 0, 0, 0, 'a', 'b'});
    BinaryReader ansi(ansi_data, ue3());
    EXPECT_THROW((void)ansi.read_fstring(), SerializationError);
    const auto utf16_data = bytes_of({0xF6, 0xFF, 0xFF, 0xFF, 'a', 0});
    BinaryReader utf16(utf16_data, ue3());
    EXPECT_THROW((void)utf16.read_fstring(), SerializationError);
}

TEST(FStringTests, RejectsHugeLengthsBeforeAllocating)
{
    const auto data = bytes_of({0xFF, 0xFF, 0xFF, 0x7F});
    BinaryReader reader(data, ue3());
    try
    {
        (void)reader.read_fstring();
        FAIL();
    }
    catch (const SerializationError& e)
    {
        EXPECT_EQ(e.code(), ErrorCode::LimitExceeded);
    }
    const auto min_data = bytes_of({0x00, 0x00, 0x00, 0x80});
    BinaryReader min_reader(min_data, ue3());
    EXPECT_THROW((void)min_reader.read_fstring(), SerializationError);
}

TEST(FStringTests, UsesCompactIndexLengthForOldVersions)
{
    SerializationContext legacy;
    legacy.file_version = 100;
    const auto data = bytes_of({3, 'h', 'i', 0});
    BinaryReader reader(data, legacy);
    EXPECT_EQ(reader.read_string(), "hi");
    EXPECT_EQ(write(FString("hi"), legacy), data);
}
