// Unit tests for compression output (LZO1X encoder) and the package writer.

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <random>
#include <string>
#include <vector>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Serialization/Compression.hpp>
#include <UnrealLib/Writer/PackageWriter.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using namespace UnrealLib::Testing;

namespace
{
    std::vector<std::byte> round_trip(CompressionMethod method, const std::vector<std::byte>& input)
    {
        const auto packed = compress(method, input);
        std::vector<std::byte> out(input.size());
        decompress(method, packed, out);
        return out;
    }

    std::vector<std::byte> text(std::string_view s)
    {
        std::vector<std::byte> out;
        for (const char c : s) out.push_back(static_cast<std::byte>(c));
        return out;
    }

    Package load(const std::vector<std::byte>& bytes, const std::string& name = "TestPkg")
    {
        Package p;
        p.load_from_memory(bytes, name);
        return p;
    }
}

class LzoEncoderTests : public ::testing::TestWithParam<std::string>
{
};

TEST_P(LzoEncoderTests, RoundTrips)
{
    const auto input = text(GetParam());
    EXPECT_EQ(round_trip(CompressionMethod::Lzo, input), input);
}

INSTANTIATE_TEST_SUITE_P(Shapes, LzoEncoderTests,
                         ::testing::Values(std::string(), std::string("a"), std::string("abc"),
                                           std::string("abcabcabcabcabcabc"), std::string(300, 'x'),
                                           std::string("ab") + std::string(5000, 'q') + "tail",
                                           std::string("0123456789abcdefghij0123456789abcdefghij!")));

TEST(LzoEncoderTests, LongDistancesAndLengthsRoundTrip)
{
    // Repeats at distances beyond 0x4000 (M4 matches), long literal runs (> 238 first literals) and long matches.
    std::mt19937 rng(42);
    std::vector<std::byte> block(30000);
    for (auto& b : block) b = static_cast<std::byte>(rng() & 0xFF);
    std::vector<std::byte> input = block;
    input.insert(input.end(), block.begin(), block.end());          // distance 30000 (M4)
    input.insert(input.end(), block.begin(), block.begin() + 5000); // distance 60000 > max: literals again
    input.insert(input.end(), 70000, std::byte{7});                 // very long match
    EXPECT_EQ(round_trip(CompressionMethod::Lzo, input), input);
    EXPECT_LT(compress(CompressionMethod::Lzo, input).size(), input.size());
}

RC_GTEST_PROP(LzoEncoderProperties, ArbitraryBytesRoundTrip, (std::vector<std::uint8_t> data, std::uint8_t period))
{
    // Mix random bytes with a periodic pattern so both literals and matches occur.
    std::vector<std::byte> input;
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        input.push_back(static_cast<std::byte>(period > 0 && i % 3 == 0 ? data[i % (period % 7 + 1)] : data[i]));
    }
    RC_ASSERT(round_trip(CompressionMethod::Lzo, input) == input);
}

TEST(CompressApiTests, MethodsAndErrors)
{
    EXPECT_TRUE(can_compress(CompressionMethod::Zlib));
    EXPECT_TRUE(can_compress(CompressionMethod::Lzo));
    EXPECT_FALSE(can_compress(CompressionMethod::Lzx));
    const auto input = text("zlib zlib zlib zlib");
    EXPECT_EQ(round_trip(CompressionMethod::Zlib, input), input);
    EXPECT_THROW((void)compress(CompressionMethod::Lzx, input), UnsupportedError);
}

TEST(PackageWriterTests, UnmodifiedUncompressedPackageIsByteIdentical)
{
    const SyntheticPackage original = build_synthetic_package(make_basic_spec());
    const Package package = load(original.file);
    const PackageModel model = PackageModel::from_package(package);
    EXPECT_EQ(write_package(model), original.file);
}

class PackageWriterCompressionTests : public ::testing::TestWithParam<CompressionMethod>
{
};

TEST_P(PackageWriterCompressionTests, CompressedOutputReloadsWithSameContents)
{
    SyntheticPackageSpec spec = make_basic_spec();
    spec.compression = CompressionMethod::Lzo; // also covers reading a compressed source
    const Package package = load(build_synthetic_package(spec).file);
    const PackageModel model = PackageModel::from_package(package);

    WriteOptions options;
    options.compression = GetParam();
    options.chunk_size = 64; // several chunks and blocks even for a tiny package
    options.block_size = 16;
    const auto written = write_package(model, options);
    const Package reloaded = load(written);
    EXPECT_EQ(reloaded.summary().compressed_chunks.empty(), GetParam() == CompressionMethod::None);
    ASSERT_EQ(reloaded.exports().size(), package.exports().size());
    for (std::size_t i = 0; i < package.exports().size(); ++i)
    {
        const auto a = package.export_data(i);
        const auto b = reloaded.export_data(i);
        EXPECT_TRUE(std::equal(a.begin(), a.end(), b.begin(), b.end())) << "export " << i;
        EXPECT_EQ(reloaded.object_path(ObjectIndex::from_export(i)), package.object_path(ObjectIndex::from_export(i)));
    }
    EXPECT_EQ(reloaded.depends(), package.depends());
    // Writing the reloaded package uncompressed reproduces the uncompressed rewrite of the original.
    EXPECT_EQ(write_package(PackageModel::from_package(reloaded)), write_package(model));
}

TEST_P(PackageWriterCompressionTests, RelocatesUe3ThumbnailImagesAndGuidTables)
{
    auto spec = make_basic_spec();
    spec.file_version = 868;
    spec.licensee_version = 0;
    const auto base = load(build_synthetic_package(spec).file);
    auto model = PackageModel::from_package(base);
    ASSERT_TRUE(model.header_tail.empty());
    ASSERT_EQ(model.header_tail_offset, static_cast<std::size_t>(base.summary().header_size));
    const auto tail_start = base.summary().header_size;
    const auto image = bytes_of({0x89, 0x50, 0x4E, 0x47});
    const Guid import_guid{1, 2, 3, 4};
    const Guid export_guid{5, 6, 7, 8};
    const auto export_index = ObjectIndex::from_export(0);

    BinaryWriter tail(base.context());
    model.summary.import_export_guids_offset = tail_start;
    model.summary.import_guids_count = 1;
    model.summary.export_guids_count = 1;
    tail.write_fstring("TestLevel");
    tail.write_i32(1);
    tail.write_guid(import_guid);
    tail.write_guid(export_guid);
    tail.write_i32(export_index.value);

    const auto image_offset = tail_start + static_cast<std::int32_t>(tail.position());
    tail.write_i32(64);
    tail.write_i32(32);
    tail.write_i32(static_cast<std::int32_t>(image.size()));
    tail.write_bytes(image);
    model.summary.thumbnail_table_offset = tail_start + static_cast<std::int32_t>(tail.position());
    tail.write_i32(2);
    tail.write_fstring("Texture2D");
    tail.write_fstring("TestPkg.MyTexture");
    tail.write_i32(image_offset);
    tail.write_fstring("Texture2D");
    tail.write_fstring("TestPkg.Empty");
    tail.write_i32(0);
    model.header_tail = tail.take();

    const auto original = write_package(model);
    const auto original_package = load(original);
    auto edited = PackageModel::from_package(original_package);
    EXPECT_EQ(write_package(edited), original);
    edited.add_name("AnExtraNameToMoveTheHeader");

    WriteOptions options;
    options.compression = GetParam();
    options.chunk_size = 128;
    options.block_size = 32;
    const auto reloaded = load(write_package(edited, options));
    EXPECT_GT(reloaded.summary().import_export_guids_offset, original_package.summary().import_export_guids_offset);
    EXPECT_GT(reloaded.summary().thumbnail_table_offset, original_package.summary().thumbnail_table_offset);
    ASSERT_EQ(reloaded.thumbnails().size(), 2u);
    EXPECT_GT(reloaded.thumbnails()[0].data_offset, image_offset);
    EXPECT_EQ(reloaded.thumbnails()[0].object_path.text, "TestPkg.MyTexture");
    const auto thumbnail = reloaded.thumbnail_image(0);
    ASSERT_TRUE(thumbnail);
    EXPECT_EQ(thumbnail->width, 64);
    EXPECT_EQ(thumbnail->height, 32);
    EXPECT_EQ(std::vector<std::byte>(thumbnail->encoded.begin(), thumbnail->encoded.end()), image);
    EXPECT_FALSE(reloaded.thumbnail_image(1));
    ASSERT_EQ(reloaded.import_guids().size(), 1u);
    EXPECT_EQ(reloaded.import_guids()[0].level_name.text, "TestLevel");
    EXPECT_EQ(reloaded.import_guids()[0].object_guids, std::vector<Guid>{import_guid});
    ASSERT_EQ(reloaded.export_guids().size(), 1u);
    EXPECT_EQ(reloaded.export_guids()[0].guid, export_guid);
    EXPECT_EQ(reloaded.export_guids()[0].object, export_index);
}

TEST(PackageWriterTests, RejectsThumbnailTableOutsidePreservedHeader)
{
    auto spec = make_basic_spec();
    spec.file_version = 868;
    spec.licensee_version = 0;
    auto model = PackageModel::from_package(load(build_synthetic_package(spec).file));
    model.summary.thumbnail_table_offset = model.exports.front().serial_offset;
    EXPECT_THROW((void)write_package(model), UnsupportedError);
}

INSTANTIATE_TEST_SUITE_P(Methods, PackageWriterCompressionTests,
                         ::testing::Values(CompressionMethod::None, CompressionMethod::Lzo, CompressionMethod::Zlib));

TEST(PackageWriterTests, EditsNamesImportsExportsAndData)
{
    const Package package = load(build_synthetic_package(make_basic_spec()).file);
    PackageModel model = PackageModel::from_package(package);
    const std::size_t names = model.names.size();

    EXPECT_EQ(model.make_name("Foo_3"), (FName{model.add_name("Foo"), 4}));
    EXPECT_EQ(model.name_text(model.make_name("Bar_07")), "Bar_07"); // leading zero: not an instance number
    EXPECT_EQ(model.add_name("none"), *package.names().find("None")); // case-insensitive reuse
    EXPECT_GT(model.names.size(), names);

    const ObjectIndex core = *model.find_import("Package", ObjectIndex{}, "Core");
    const ObjectIndex texture_class = model.add_import("Engine", "Class", core, "Texture2D");
    EXPECT_EQ(model.find_import("class", core, "texture2d"), texture_class);

    ExportEntry entry;
    entry.class_index = texture_class;
    entry.object_name = model.make_name("NewObject");
    const ObjectIndex added = model.add_export(entry, text("payload"));
    model.rename(ObjectIndex::from_export(0), "Renamed");

    const Package reloaded = load(write_package(model));
    EXPECT_EQ(reloaded.exports().size(), package.exports().size() + 1);
    EXPECT_EQ(reloaded.object_name(added), "NewObject");
    EXPECT_EQ(reloaded.class_name(added), "Texture2D");
    EXPECT_EQ(reloaded.object_name(ObjectIndex::from_export(0)), "Renamed");
    const auto data = reloaded.export_data(added.export_index());
    EXPECT_EQ(std::vector<std::byte>(data.begin(), data.end()), text("payload"));
    EXPECT_EQ(reloaded.summary().generations.back().export_count, reloaded.summary().export_count);
}

TEST(PackageWriterTests, ValidateRejectsInconsistentModels)
{
    PackageModel model = PackageModel::from_package(load(build_synthetic_package(make_basic_spec()).file));
    PackageModel bad_ref = model;
    bad_ref.exports[0].outer = ObjectIndex{999};
    EXPECT_THROW(bad_ref.validate(), Error);
    PackageModel bad_data = model;
    bad_data.export_data.pop_back();
    EXPECT_THROW((void)write_package(bad_data), Error);
    PackageModel bad_name = model;
    bad_name.imports[0].object_name.index = 12345;
    EXPECT_THROW(bad_name.validate(), Error);
}

TEST(PackageWriterTests, ReplacePropertiesKeepsSurroundingBytes)
{
    PackageModel model = PackageModel::from_package(load(build_synthetic_package(make_basic_spec()).file));
    model.export_data[0] = text("HEADtail"); // 4 header bytes, empty property range, 4 native bytes
    Property p;
    p.name = "Count";
    PropertyTag tag;
    tag.name = model.make_name("Count");
    tag.type_name = model.make_name("IntProperty");
    tag.type = PropertyType::Int;
    tag.size = 4;
    p.tag = tag;
    p.value.value = std::int32_t{7};
    replace_properties(model, ObjectIndex::from_export(0), 4, 4, {p});
    const auto& data = model.export_data[0];
    ASSERT_GT(data.size(), 8u);
    EXPECT_EQ(std::vector<std::byte>(data.begin(), data.begin() + 4), text("HEAD"));
    EXPECT_EQ(std::vector<std::byte>(data.end() - 4, data.end()), text("tail"));
    EXPECT_THROW(replace_properties(model, ObjectIndex::from_export(0), 5, 4, {}), Error);
}
