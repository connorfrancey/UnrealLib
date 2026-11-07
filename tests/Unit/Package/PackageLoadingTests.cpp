#include <gtest/gtest.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/CompressedChunk.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Package/PackageValidator.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using namespace UnrealLib::Testing;

namespace
{
    Package load(const SyntheticPackage& synthetic, LoadOptions options = {})
    {
        Package package;
        package.load_from_memory(synthetic.file, "TestPkg", options);
        return package;
    }

    bool has_code(const Diagnostics& diagnostics, std::string_view code)
    {
        for (const auto& d : diagnostics.entries())
        {
            if (d.code == code)
            {
                return true;
            }
        }
        return false;
    }

    void expect_basic_contents(const Package& package)
    {
        EXPECT_EQ(package.name(), "TestPkg");
        EXPECT_EQ(package.profile().id, "mirrorsedge-pc");
        EXPECT_EQ(package.names().size(), 7u);
        ASSERT_EQ(package.imports().size(), 2u);
        ASSERT_EQ(package.exports().size(), 3u);

        EXPECT_EQ(package.object_path(ObjectIndex::from_import(1)), "Core.Object");
        EXPECT_EQ(package.class_name(ObjectIndex::from_import(1)), "Class");
        EXPECT_EQ(package.object_path(ObjectIndex::from_export(1)), "MyObject.MyObject_2");
        EXPECT_EQ(package.class_name(ObjectIndex::from_export(0)), "Object");
        EXPECT_EQ(package.class_name(ObjectIndex::from_export(2)), "Class");
        EXPECT_EQ(package.object_name(ObjectIndex{}), "None");

        EXPECT_EQ(package.find_export("myobject.myobject_2"), std::optional(ObjectIndex::from_export(1)));
        EXPECT_EQ(package.find_export("MyObject"), std::optional(ObjectIndex::from_export(0)));
        EXPECT_EQ(package.find_import("Core.Object"), std::optional(ObjectIndex::from_import(1)));
        EXPECT_EQ(package.find_export("Nope"), std::nullopt);

        const auto data0 = package.export_data(0);
        ASSERT_EQ(data0.size(), 8u);
        EXPECT_EQ(data0[7], std::byte{0x08});
        EXPECT_EQ(package.export_data(1).size(), 300u);
        EXPECT_TRUE(package.export_data(2).empty());
        EXPECT_THROW((void)package.export_data(3), Error);

        ASSERT_EQ(package.depends().size(), 3u);
        EXPECT_EQ(package.depends()[0], std::vector<ObjectIndex>{ObjectIndex::from_import(1)});

        const Diagnostics validation = validate_package(package);
        EXPECT_FALSE(validation.has_errors()) << (validation.empty() ? "" : validation.entries()[0].message);
        EXPECT_EQ(validation.count(Severity::Warning), 0u);
    }
}

TEST(PackageLoadingTests, LoadsUncompressedSyntheticPackage)
{
    const auto synthetic = build_synthetic_package(make_basic_spec());
    const Package package = load(synthetic);
    EXPECT_FALSE(package.summary().is_compressed());
    EXPECT_EQ(package.data().size(), synthetic.file.size());
    expect_basic_contents(package);
}

TEST(PackageLoadingTests, LoadsZlibCompressedPackageWithManyBlocks)
{
    SyntheticPackageSpec spec = make_basic_spec();
    spec.compression = CompressionMethod::Zlib;
    spec.block_size = 64; // forces several blocks
    const auto synthetic = build_synthetic_package(spec);
    ASSERT_LT(synthetic.file.size(), synthetic.uncompressed.size() + 200);

    const Package package = load(synthetic);
    EXPECT_TRUE(package.summary().is_compressed());
    ASSERT_EQ(package.data().size(), synthetic.uncompressed.size());
    EXPECT_TRUE(std::equal(package.data().begin(), package.data().end(), synthetic.uncompressed.begin()));
    expect_basic_contents(package);
    // zlib is not a Mirror's Edge compression method -> load diagnostic.
    EXPECT_TRUE(has_code(package.diagnostics(), "compression.unexpected-method"));
}

TEST(PackageLoadingTests, ExplicitProfileSelection)
{
    const auto synthetic = build_synthetic_package(make_basic_spec());
    LoadOptions options;
    options.profile = "Generic UE3";
    const Package package = load(synthetic, options);
    EXPECT_EQ(package.profile().id, "ue3-generic");
    EXPECT_TRUE(has_code(package.diagnostics(), "profile.mismatch"));

    options.profile = "does-not-exist";
    EXPECT_THROW((void)load(synthetic, options), Error);
}

TEST(PackageLoadingTests, UnverifiedProfileProducesWarning)
{
    SyntheticPackageSpec spec = make_basic_spec();
    spec.licensee_version = 0;
    const Package package = load(build_synthetic_package(spec));
    EXPECT_EQ(package.profile().id, "ue3-generic");
    EXPECT_TRUE(has_code(package.diagnostics(), "profile.unverified"));
}

TEST(PackageLoadingTests, AccessorsRequireLoadedPackage)
{
    const Package package;
    EXPECT_FALSE(package.is_loaded());
    EXPECT_THROW((void)package.summary(), Error);
    EXPECT_THROW((void)package.names(), Error);
}

TEST(PackageLoadingTests, FailedLoadLeavesPackageUnloaded)
{
    Package package;
    package.load_from_memory(build_synthetic_package(make_basic_spec()).file, "A");
    EXPECT_TRUE(package.is_loaded());
    EXPECT_THROW(package.load_from_memory(bytes_of({1, 2, 3, 4}), "B"), Error);
    EXPECT_FALSE(package.is_loaded());
}

TEST(PackageLoadingTests, RejectsTableOffsetOutsidePackage)
{
    auto synthetic = build_synthetic_package(make_basic_spec());
    PackageSummary s = synthetic.summary;
    s.export_offset = 1 << 30;
    BinaryWriter writer;
    write_package_summary(writer, s);
    std::copy(writer.data().begin(), writer.data().end(), synthetic.file.begin());
    EXPECT_THROW((void)load(synthetic), Error);
}

TEST(PackageLoadingTests, RejectsHugeTableCountWithoutAllocating)
{
    auto synthetic = build_synthetic_package(make_basic_spec());
    PackageSummary s = synthetic.summary;
    s.name_count = 0x7FFFFFFF;
    BinaryWriter writer;
    write_package_summary(writer, s);
    std::copy(writer.data().begin(), writer.data().end(), synthetic.file.begin());
    EXPECT_THROW((void)load(synthetic), Error);
}

TEST(PackageLoadingTests, ValidatorReportsBrokenReferences)
{
    SyntheticPackageSpec spec = make_basic_spec();
    spec.exports[0].entry.class_index = ObjectIndex{-50};          // import out of range
    spec.exports[1].entry.object_name = FName{99, 0};              // name out of range
    spec.exports[2].entry.outer = ObjectIndex::from_export(2);     // self outer -> cycle
    spec.exports[0].depends = {ObjectIndex{77}};                   // depends out of range
    const Package package = load(build_synthetic_package(spec));
    const Diagnostics d = validate_package(package);
    EXPECT_TRUE(has_code(d, "reference.out-of-range"));
    EXPECT_TRUE(has_code(d, "name.out-of-range"));
    EXPECT_TRUE(has_code(d, "reference.self-outer"));
    EXPECT_TRUE(has_code(d, "reference.outer-cycle"));
    EXPECT_EQ(package.object_path(ObjectIndex::from_export(2)), "<cycle>.TestPkg");
}

TEST(PackageLoadingTests, ValidatorReportsSerialProblems)
{
    // Re-create with overlapping payloads by editing the export table directly.
    SyntheticPackageSpec spec = make_basic_spec();
    auto base = build_synthetic_package(spec);
    Package original;
    original.load_from_memory(base.file, "TestPkg");
    std::vector<ExportEntry> exports(original.exports().begin(), original.exports().end());
    exports[1].serial_offset = exports[0].serial_offset + 4; // overlaps export 0
    BinaryWriter writer(original.context());
    for (const auto& e : exports)
    {
        write_export_entry(writer, e);
    }
    std::copy(writer.data().begin(), writer.data().end(),
              base.file.begin() + original.summary().export_offset);
    Package overlapped;
    overlapped.load_from_memory(base.file, "TestPkg");
    EXPECT_TRUE(has_code(validate_package(overlapped), "export.serial-overlap"));

    exports[1].serial_offset = 1 << 30;
    writer = BinaryWriter(original.context());
    for (const auto& e : exports)
    {
        write_export_entry(writer, e);
    }
    std::copy(writer.data().begin(), writer.data().end(),
              base.file.begin() + original.summary().export_offset);
    Package out_of_bounds;
    out_of_bounds.load_from_memory(base.file, "TestPkg");
    EXPECT_TRUE(has_code(validate_package(out_of_bounds), "export.serial-out-of-bounds"));
    EXPECT_THROW((void)out_of_bounds.export_data(1), Error);
}

TEST(PackageLoadingTests, CompressedChunkHeaderValidation)
{
    SyntheticPackageSpec spec = make_basic_spec();
    spec.compression = CompressionMethod::Zlib;
    spec.block_size = 64;
    auto synthetic = build_synthetic_package(spec);
    const auto chunk_offset = static_cast<std::size_t>(synthetic.summary.compressed_chunks[0].compressed_offset);

    {
        auto bad = synthetic;
        bad.file[chunk_offset] = std::byte{0}; // bad tag
        EXPECT_THROW((void)load(bad), CompressionError);
    }
    {
        auto bad = synthetic;
        bad.file[chunk_offset + 16 + 4] ^= std::byte{1}; // first block's uncompressed size
        EXPECT_THROW((void)load(bad), CompressionError);
    }
    {
        auto bad = synthetic;
        bad.file.back() ^= std::byte{0xFF}; // corrupt the last compressed block
        EXPECT_THROW((void)load(bad), Error);
    }
}

TEST(PackageLoadingTests, DecompressionRespectsSizeLimit)
{
    SyntheticPackageSpec spec = make_basic_spec();
    spec.compression = CompressionMethod::Zlib;
    const auto synthetic = build_synthetic_package(spec);
    LoadOptions options;
    options.limits.max_decompressed_size = 64;
    try
    {
        (void)load(synthetic, options);
        FAIL();
    }
    catch (const Error& e)
    {
        EXPECT_EQ(e.code(), ErrorCode::LimitExceeded);
    }
}
