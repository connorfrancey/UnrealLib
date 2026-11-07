#include <gtest/gtest.h>

#include <unordered_map>

#include <UnrealLib/Utilities/Text.hpp>

using namespace UnrealLib;

TEST(TextTests, Cp1252RoundTripsAllBytes)
{
    std::vector<std::uint8_t> all(256);
    for (int i = 0; i < 256; ++i)
    {
        all[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    }
    const std::string utf8 = Text::cp1252_to_utf8(all);
    const auto back = Text::utf8_to_cp1252(utf8);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, all);
}

TEST(TextTests, Cp1252SpecialCharacters)
{
    const std::vector<std::uint8_t> euro{0x80};
    EXPECT_EQ(Text::cp1252_to_utf8(euro), "\xE2\x82\xAC");
    const std::vector<std::uint8_t> trademark{0x99};
    EXPECT_EQ(Text::cp1252_to_utf8(trademark), "\xE2\x84\xA2");
    EXPECT_FALSE(Text::utf8_to_cp1252("\xE4\xB8\xAD").has_value()); // U+4E2D not in 1252
    EXPECT_FALSE(Text::utf8_to_cp1252("\xC3").has_value());         // truncated sequence
    EXPECT_FALSE(Text::utf8_to_cp1252("\xC0\x80").has_value());     // overlong
}

TEST(TextTests, Utf16RoundTrip)
{
    const std::u16string units = u"Aé中\U0001F600";
    const std::string utf8 = Text::utf16_to_utf8(units);
    EXPECT_EQ(utf8, "A\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80");
    const auto back = Text::utf8_to_utf16(utf8);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, units);
}

TEST(TextTests, UnpairedSurrogatesArePreserved)
{
    const std::u16string units{u'a', static_cast<char16_t>(0xD800), u'b', static_cast<char16_t>(0xDC00)};
    const auto back = Text::utf8_to_utf16(Text::utf16_to_utf8(units));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, units);
}

TEST(TextTests, CaseInsensitiveHelpers)
{
    EXPECT_TRUE(Text::iequals("PlayerController", "playercontroller"));
    EXPECT_FALSE(Text::iequals("Actor", "Actors"));
    EXPECT_EQ(Text::to_lower_ascii("TdGame.TdPawn"), "tdgame.tdpawn");

    std::unordered_map<std::string, int, Text::CaseInsensitiveHash, Text::CaseInsensitiveEqual> map;
    map.emplace("Engine", 1);
    EXPECT_EQ(map.count("ENGINE"), 1u);
    EXPECT_EQ(map.find(std::string_view("engine"))->second, 1);
}
