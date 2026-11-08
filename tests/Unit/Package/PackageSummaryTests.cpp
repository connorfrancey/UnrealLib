#include <gtest/gtest.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/ObjectTables.hpp>
#include <UnrealLib/Package/PackageSummary.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

namespace
{
    PackageSummary sample_summary(std::uint16_t version)
    {
        PackageSummary s;
        s.file_version = version;
        s.licensee_version = 43;
        s.header_size = 0x1BB1F;
        s.folder_name = FString("None");
        s.package_flags = 0x02A80008;
        s.name_count = 660;
        s.name_offset = 109;
        s.export_count = 1274;
        s.export_offset = 0x40E7;
        s.import_count = 19;
        s.import_offset = 0x3ED3;
        s.depends_offset = 0x1A737;
        s.guid = Guid{0x06C691FD, 0x417B4677, 0xBC776381, 0x583E4EE5};
        s.generations = {GenerationInfo{1274, 660, 1289}};
        s.engine_version = 3716;
        s.cooker_version = 60;
        s.compression_flags = 2;
        s.compressed_chunks = {CompressedChunkInfo{109, 0x2EF55, 125, 0xC4D8}};
        s.package_source = 0xC082C6B6;
        return s;
    }
}

TEST(PackageSummaryTests, MatchesMirrorsEdgeCoreHeaderBytes)
{
    // The first 125 bytes of Mirror's Edge (Steam) TdGame/CookedPC/Core.u, transcribed from a hex dump.
    // These are header *metadata* values only (counts, offsets, flags), not game content.
    const auto expected = bytes_of({
        0xC1, 0x83, 0x2A, 0x9E, 0x18, 0x02, 0x2B, 0x00, 0x1F, 0xBB, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00,
        0x4E, 0x6F, 0x6E, 0x65, 0x00, 0x08, 0x00, 0xA8, 0x02, 0x94, 0x02, 0x00, 0x00, 0x6D, 0x00, 0x00,
        0x00, 0xFA, 0x04, 0x00, 0x00, 0xE7, 0x40, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0xD3, 0x3E, 0x00,
        0x00, 0x37, 0xA7, 0x01, 0x00, 0xFD, 0x91, 0xC6, 0x06, 0x77, 0x46, 0x7B, 0x41, 0x81, 0x63, 0x77,
        0xBC, 0xE5, 0x4E, 0x3E, 0x58, 0x01, 0x00, 0x00, 0x00, 0xFA, 0x04, 0x00, 0x00, 0x94, 0x02, 0x00,
        0x00, 0x09, 0x05, 0x00, 0x00, 0x84, 0x0E, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x6D, 0x00, 0x00, 0x00, 0x55, 0xEF, 0x02, 0x00, 0x7D, 0x00, 0x00,
        0x00, 0xD8, 0xC4, 0x00, 0x00, 0xB6, 0xC6, 0x82, 0xC0, 0x00, 0x00, 0x00, 0x00,
    });
    ASSERT_EQ(expected.size(), 125u);

    BinaryReader reader(expected);
    const PackageSummary parsed = read_package_summary(reader);
    EXPECT_EQ(reader.position(), expected.size());
    EXPECT_EQ(parsed, sample_summary(536));

    BinaryWriter writer;
    write_package_summary(writer, parsed);
    EXPECT_EQ(std::vector<std::byte>(writer.data().begin(), writer.data().end()), expected);
}

TEST(PackageSummaryTests, RoundTripsAcrossVersionGates)
{
    for (const std::uint16_t version : std::initializer_list<std::uint16_t>{249, 269, 277, 322, 334, 343, 415, 482, 516, 536, 584, 623, 766, 767, 801, 868})
    {
        PackageSummary s = sample_summary(version);
        if (version >= 584)
        {
            s.thumbnail_table_offset = 1234;
        }
        if (version >= 623)
        {
            s.import_export_guids_offset = 99;
            s.import_guids_count = 2;
            s.export_guids_count = 3;
        }
        if (version >= 516)
        {
            s.additional_packages_to_cook = {FString("Foo"), FString("Bar")};
        }
        if (version >= 767)
        {
            s.texture_allocations = {TextureAllocation{1024, 512, 11, 5, 0x40, {3, 7, 9}},
                                     TextureAllocation{64, 64, 7, 2, 0, {}}};
        }
        // Zero fields that the version does not serialize, so the comparison is exact.
        if (version < 277) s.cooker_version = 0;
        if (version < 269) s.folder_name = FString();
        if (version < 334) { s.compression_flags = 0; s.compressed_chunks.clear(); }
        if (version < 322) s.generations[0].net_object_count = 0;
        if (version < 415) s.depends_offset = 0;
        if (version < 482) s.package_source = 0;

        BinaryWriter writer;
        write_package_summary(writer, s);
        BinaryReader reader(writer.data());
        const PackageSummary parsed = read_package_summary(reader);
        EXPECT_TRUE(reader.at_end()) << "version " << version;
        EXPECT_EQ(parsed, s) << "version " << version;
    }
}

TEST(PackageSummaryTests, RoundTripsBigEndian)
{
    PackageSummary s = sample_summary(536);
    s.endian = Endian::Big;
    s.tag = package_signature_swapped;
    BinaryWriter writer;
    write_package_summary(writer, s);
    EXPECT_EQ(writer.data()[0], std::byte{0x9E});
    BinaryReader reader(writer.data());
    EXPECT_EQ(read_package_summary(reader), s);
}

TEST(PackageSummaryTests, RejectsBadSignature)
{
    const auto data = bytes_of({0, 0, 0, 0, 0x18, 0x02, 0x2B, 0x00});
    BinaryReader reader(data);
    EXPECT_THROW((void)read_package_summary(reader), SerializationError);
}

TEST(PackageSummaryTests, RejectsUnsupportedVersions)
{
    // UE4 legacy version -7
    const auto ue4 = bytes_of({0xC1, 0x83, 0x2A, 0x9E, 0xF9, 0xFF, 0xFF, 0xFF});
    BinaryReader r1(ue4);
    EXPECT_THROW((void)read_package_summary(r1), UnsupportedError);
    // Version 69 (UE2)
    const auto ue2 = bytes_of({0xC1, 0x83, 0x2A, 0x9E, 0x45, 0x00, 0x00, 0x00});
    BinaryReader r2(ue2);
    EXPECT_THROW((void)read_package_summary(r2), UnsupportedError);
    // Version 869 (past the last UE3 file version, 868)
    const auto late = bytes_of({0xC1, 0x83, 0x2A, 0x9E, 0x65, 0x03, 0x00, 0x00});
    BinaryReader r3(late);
    EXPECT_THROW((void)read_package_summary(r3), UnsupportedError);
}

TEST(PackageSummaryTests, RejectsTruncationAtEveryByte)
{
    BinaryWriter writer;
    write_package_summary(writer, sample_summary(536));
    const std::vector<std::byte> full(writer.data().begin(), writer.data().end());
    for (std::size_t length = 0; length < full.size(); ++length)
    {
        const std::vector<std::byte> truncated(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(length));
        BinaryReader reader(truncated);
        EXPECT_THROW((void)read_package_summary(reader), SerializationError) << "length " << length;
    }
}

TEST(PackageSummaryTests, DescribesFlags)
{
    EXPECT_EQ(describe_package_flags(0), "None");
    EXPECT_EQ(describe_package_flags(PackageFlag::Cooked | PackageFlag::ContainsScript), "Cooked|ContainsScript");
    EXPECT_EQ(describe_package_flags(0x00000100u), "0x00000100");
}

TEST(ObjectTablesTests, ObjectIndexEncoding)
{
    EXPECT_TRUE(ObjectIndex{}.is_null());
    EXPECT_EQ(ObjectIndex::from_export(0).value, 1);
    EXPECT_EQ(ObjectIndex::from_import(0).value, -1);
    EXPECT_EQ(ObjectIndex{5}.export_index(), 4u);
    EXPECT_EQ(ObjectIndex{-5}.import_index(), 4u);
    EXPECT_TRUE(ObjectIndex{-1}.is_import());
    EXPECT_TRUE(ObjectIndex{1}.is_export());
}

TEST(ObjectTablesTests, ImportEntryRoundTrip)
{
    SerializationContext ctx;
    ctx.file_version = 536;
    const ImportEntry entry{FName{1, 0}, FName{2, 0}, ObjectIndex{-3}, FName{4, 5}};
    BinaryWriter writer(ctx);
    write_import_entry(writer, entry);
    EXPECT_EQ(writer.size(), 28u);
    BinaryReader reader(writer.data(), ctx);
    EXPECT_EQ(read_import_entry(reader), entry);
}

TEST(ObjectTablesTests, ExportEntryRoundTripAcrossVersions)
{
    ExportEntry entry;
    entry.class_index = ObjectIndex{-2};
    entry.super_index = ObjectIndex{3};
    entry.outer = ObjectIndex{1};
    entry.object_name = FName{4, 2};
    entry.archetype = ObjectIndex{-7};
    entry.object_flags = 0x0007000400000000ull;
    entry.serial_size = 100;
    entry.serial_offset = 2000;
    entry.component_map = {ComponentMapEntry{FName{5, 0}, ObjectIndex{9}}};
    entry.export_flags = 1;
    entry.generation_net_object_count = {3, 4};
    entry.package_guid = Guid{1, 2, 3, 4};
    entry.package_flags = 0x80;

    for (const std::uint16_t version : std::initializer_list<std::uint16_t>{249, 322, 343, 475, 536, 543, 600})
    {
        ExportEntry e = entry;
        if (version >= 543) e.component_map.clear();
        if (version < 475) e.package_flags = 0;
        if (version < 343) e.object_name.number = 0;
        if (version < 343) e.component_map[0].name.number = 0;
        if (version < 322) { e.generation_net_object_count.clear(); e.package_guid = Guid{}; }

        SerializationContext ctx;
        ctx.file_version = version;
        BinaryWriter writer(ctx);
        write_export_entry(writer, e);
        BinaryReader reader(writer.data(), ctx);
        EXPECT_EQ(read_export_entry(reader), e) << "version " << version;
        EXPECT_TRUE(reader.at_end()) << "version " << version;
    }
}
