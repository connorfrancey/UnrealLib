#include <gtest/gtest.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

namespace
{
    SerializationContext context_for(std::uint16_t version)
    {
        SerializationContext ctx;
        ctx.file_version = version;
        return ctx;
    }
}

TEST(NameTableTests, ReadsUe3NameEntry)
{
    // FString "None" + uint64 flags 0x0007001000000000
    const auto data = bytes_of({5, 0, 0, 0, 'N', 'o', 'n', 'e', 0, 0, 0, 0, 0, 0x10, 0, 0x07, 0});
    BinaryReader reader(data, context_for(536));
    const NameEntry entry = read_name_entry(reader);
    EXPECT_EQ(entry.text.text, "None");
    EXPECT_EQ(entry.flags, 0x0007001000000000ull);
    EXPECT_TRUE(reader.at_end());

    BinaryWriter writer(context_for(536));
    write_name_entry(writer, entry);
    EXPECT_EQ(std::vector<std::byte>(writer.data().begin(), writer.data().end()), data);
}

TEST(NameTableTests, ReadsLegacyNameEntries)
{
    // Version < 64: NUL-terminated ANSI + 32-bit flags.
    const auto data = bytes_of({'A', 'b', 0, 0x10, 0, 0x07, 0});
    BinaryReader reader(data, context_for(61));
    const NameEntry entry = read_name_entry(reader);
    EXPECT_EQ(entry.text.text, "Ab");
    EXPECT_EQ(entry.flags, 0x00070010u);

    BinaryWriter writer(context_for(61));
    write_name_entry(writer, entry);
    EXPECT_EQ(std::vector<std::byte>(writer.data().begin(), writer.data().end()), data);

    BinaryWriter narrow(context_for(61));
    EXPECT_THROW(write_name_entry(narrow, NameEntry{FString("x"), 1ull << 40}), SerializationError);
}

TEST(NameTableTests, FNameSerializationDependsOnVersion)
{
    {
        BinaryWriter writer(context_for(536));
        write_fname(writer, FName{7, 3});
        EXPECT_EQ(writer.size(), 8u);
        BinaryReader reader(writer.data(), context_for(536));
        EXPECT_EQ(read_fname(reader), (FName{7, 3}));
    }
    {
        BinaryWriter writer(context_for(300)); // < 343: no number
        write_fname(writer, FName{7, 0});
        EXPECT_EQ(writer.size(), 4u);
        BinaryReader reader(writer.data(), context_for(300));
        EXPECT_EQ(read_fname(reader), (FName{7, 0}));
    }
    {
        BinaryWriter writer(context_for(100)); // < 178: CompactIndex
        write_fname(writer, FName{7, 0});
        EXPECT_EQ(writer.size(), 1u);
    }
}

TEST(NameTableTests, LookupIsCaseInsensitiveAndFirstWins)
{
    NameTable table({NameEntry{FString("None")}, NameEntry{FString("Actor")}, NameEntry{FString("ACTOR")}});
    EXPECT_EQ(table.size(), 3u);
    EXPECT_EQ(table.find("actor"), std::optional<std::int32_t>(1));
    EXPECT_EQ(table.find("Missing"), std::nullopt);
    EXPECT_EQ(table.add("none"), 0);
    EXPECT_EQ(table.add("Pawn"), 3);
    EXPECT_EQ(table.find("PAWN"), std::optional<std::int32_t>(3));
}

TEST(NameTableTests, ResolvesNamesWithNumbers)
{
    NameTable table({NameEntry{FString("None")}, NameEntry{FString("Mesh")}});
    EXPECT_EQ(table.to_string(FName{1, 0}), "Mesh");
    EXPECT_EQ(table.to_string(FName{1, 1}), "Mesh_0");
    EXPECT_EQ(table.to_string(FName{1, 3}), "Mesh_2");
    EXPECT_EQ(table.try_to_string(FName{2, 0}), std::nullopt);
    EXPECT_EQ(table.try_to_string(FName{-1, 0}), std::nullopt);
    EXPECT_THROW((void)table.to_string(FName{5, 0}), Error);
    EXPECT_THROW((void)table.at(2), Error);
}
