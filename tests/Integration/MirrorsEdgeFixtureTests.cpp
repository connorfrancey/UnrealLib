// Integration tests against real Mirror's Edge PC packages.
//
// Game files are never committed. Point the tests at a legally obtained installation with either
//   cmake -DUNREALLIB_MIRRORSEDGE_DIR=".../mirrors edge/TdGame/CookedPC"   (stored in the CTest environment), or
//   set UNREALLIB_MIRRORSEDGE_DIR=...                                     (when running the test binary directly).
// Tests are skipped (not failed) when the directory is not configured.
//
// The full-corpus sweep (≈2400 packages, ≈6.5 GB) additionally requires UNREALLIB_FIXTURE_SWEEP=1.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Package/PackageValidator.hpp>
#include <UnrealLib/Serialization/Archive.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Utilities/Text.hpp>

using namespace UnrealLib;
namespace fs = std::filesystem;

namespace
{
    std::string env(const char* name)
    {
#ifdef _MSC_VER
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        {
            return {};
        }
        std::string result(value);
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
#endif
    }

    fs::path fixture_dir()
    {
        const std::string value = env("UNREALLIB_MIRRORSEDGE_DIR");
        return fs::path(std::u8string(value.begin(), value.end()));
    }

#define REQUIRE_FIXTURES()                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (fixture_dir().empty() || !fs::is_directory(fixture_dir()))                                                 \
        {                                                                                                              \
            GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set to a Mirror's Edge CookedPC directory";               \
        }                                                                                                              \
    } while (false)

    std::string describe(const Diagnostics& diagnostics)
    {
        std::string out;
        for (const auto& d : diagnostics.entries())
        {
            out += std::string(to_string(d.severity)) + " " + d.code + ": " + d.message + "\n";
        }
        return out;
    }

    bool is_package_file(const fs::path& path)
    {
        const std::string ext = Text::to_lower_ascii(path.extension().string());
        return ext == ".u" || ext == ".upk" || ext == ".me1";
    }

    // Checks every structural invariant that holds for the stock packages.
    void check_package(const fs::path& path)
    {
        SCOPED_TRACE(path.string());
        Package package;
        ASSERT_NO_THROW(package.load(path)) << path;
        const PackageSummary& s = package.summary();
        EXPECT_EQ(s.file_version, 536);
        EXPECT_EQ(s.engine_version, 3716);
        EXPECT_EQ(package.names().size(), static_cast<std::size_t>(s.name_count));
        EXPECT_EQ(package.imports().size(), static_cast<std::size_t>(s.import_count));
        EXPECT_EQ(package.exports().size(), static_cast<std::size_t>(s.export_count));

        const Diagnostics validation = validate_package(package);
        EXPECT_FALSE(validation.has_errors()) << describe(validation);

        // The first name of every UE3 package written by the engine is "None" is NOT guaranteed; but every package
        // must resolve all of its object paths without invalid references.
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const std::string p = package.object_path(ObjectIndex::from_export(i));
            ASSERT_EQ(p.find("<invalid>"), std::string::npos) << "export " << i;
            ASSERT_EQ(p.find("<cycle>"), std::string::npos) << "export " << i;
        }
    }
}

TEST(MirrorsEdgeFixtureTests, CoreIsLzoCompressedAndParses)
{
    REQUIRE_FIXTURES();
    const fs::path path = fixture_dir() / "Core.u";
    if (!fs::exists(path))
    {
        GTEST_SKIP() << "Core.u not found";
    }

    const Package package = Package::open(path);
    const PackageSummary& s = package.summary();

    // Header values verified by hand from the file (see docs/supported_versions.md).
    EXPECT_EQ(s.licensee_version, 43);
    EXPECT_EQ(s.cooker_version, 60);
    EXPECT_EQ(s.compression_flags, 2u);
    ASSERT_EQ(s.compressed_chunks.size(), 1u);
    EXPECT_EQ(s.compressed_chunks[0].uncompressed_offset, s.name_offset);
    EXPECT_EQ(s.name_count, 660);
    EXPECT_EQ(s.import_count, 19);
    EXPECT_EQ(s.export_count, 1274);
    EXPECT_EQ(package.profile().id, "mirrorsedge-pc");
    EXPECT_EQ(package.data().size(),
              static_cast<std::size_t>(s.compressed_chunks[0].uncompressed_offset +
                                       s.compressed_chunks[0].uncompressed_size));

    // Core.u defines Core.Object.
    const auto object = package.find_export("Object");
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(package.class_name(*object), "Class");
    EXPECT_TRUE(package.names().find("None").has_value());

    check_package(path);
}

TEST(MirrorsEdgeFixtureTests, SummaryRewriteIsByteIdentical)
{
    REQUIRE_FIXTURES();
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(fixture_dir()))
    {
        if (!entry.is_regular_file() || !is_package_file(entry.path()))
        {
            continue;
        }
        SCOPED_TRACE(entry.path().string());
        const auto bytes = read_file(entry.path(), 1ull << 31);
        BinaryReader reader(bytes);
        const PackageSummary summary = read_package_summary(reader);
        BinaryWriter writer;
        write_package_summary(writer, summary);
        ASSERT_EQ(writer.size(), reader.position());
        EXPECT_TRUE(std::equal(writer.data().begin(), writer.data().end(), bytes.begin()));
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

TEST(MirrorsEdgeFixtureTests, ScriptPackagesLoadAndValidate)
{
    REQUIRE_FIXTURES();
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(fixture_dir()))
    {
        if (entry.is_regular_file() && Text::to_lower_ascii(entry.path().extension().string()) == ".u")
        {
            check_package(entry.path());
            ++checked;
        }
    }
    EXPECT_GT(checked, 0);
}

TEST(MirrorsEdgeFixtureTests, TableRewriteIsByteIdenticalForCore)
{
    REQUIRE_FIXTURES();
    const fs::path path = fixture_dir() / "Core.u";
    if (!fs::exists(path))
    {
        GTEST_SKIP() << "Core.u not found";
    }
    const Package package = Package::open(path);
    const auto data = package.data();
    const PackageSummary& s = package.summary();

    BinaryWriter names(package.context());
    for (const auto& n : package.names().entries())
    {
        write_name_entry(names, n);
    }
    EXPECT_TRUE(std::equal(names.data().begin(), names.data().end(),
                           data.begin() + s.name_offset));

    BinaryWriter imports(package.context());
    for (const auto& i : package.imports())
    {
        write_import_entry(imports, i);
    }
    EXPECT_TRUE(std::equal(imports.data().begin(), imports.data().end(), data.begin() + s.import_offset));

    BinaryWriter exports(package.context());
    for (const auto& e : package.exports())
    {
        write_export_entry(exports, e);
    }
    EXPECT_TRUE(std::equal(exports.data().begin(), exports.data().end(), data.begin() + s.export_offset));
}

TEST(MirrorsEdgeFixtureTests, FullCorpusSweep)
{
    REQUIRE_FIXTURES();
    if (env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "set UNREALLIB_FIXTURE_SWEEP=1 to load every package under the fixture directory";
    }
    std::map<std::string, int> profiles;
    int checked = 0;
    for (const auto& entry : fs::recursive_directory_iterator(fixture_dir()))
    {
        if (entry.is_regular_file() && is_package_file(entry.path()))
        {
            check_package(entry.path());
            ++checked;
        }
    }
    RecordProperty("packages", checked);
    EXPECT_GT(checked, 0);
}
