// Script property integration tests against real Mirror's Edge packages.
// Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set; the whole-corpus variant also needs UNREALLIB_FIXTURE_SWEEP=1.
//
// Invariants checked for every non-class export:
//   * the tagged property list parses to its "None" terminator within the export data;
//   * writing the parsed list reproduces the original bytes exactly (lossless round trip);
//   * (reported, and bounded by the test) values kept as raw bytes because no type information was available.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Utilities/Text.hpp>

#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
namespace fs = std::filesystem;
using namespace UnrealLib::TestSupport;

namespace
{
    struct PropertyTally
    {
        std::size_t objects = 0;
        std::size_t properties = 0;
        std::size_t failures = 0;
        std::size_t round_trip_failures = 0;
        std::size_t raw_values = 0;
        std::size_t raw_arrays = 0;
        std::size_t decoded_arrays = 0;
        std::map<std::string, std::size_t> raw_by_kind;
        std::map<std::string, std::size_t> raw_arrays_by_property; ///< "<class>.<property>[.<field>…]"
    };

    void count_arrays(const PropertyValue& value, PropertyTally& tally, const std::string& where)
    {
        if (const auto* array = std::get_if<ArrayValue>(&value.value))
        {
            if (array->decoded())
            {
                ++tally.decoded_arrays;
            }
            else
            {
                ++tally.raw_arrays;
                ++tally.raw_arrays_by_property[where];
            }
            for (const auto& element : array->elements) count_arrays(element, tally, where);
        }
        else if (const auto* s = std::get_if<StructValue>(&value.value))
        {
            for (const auto& field : s->fields) count_arrays(field.value, tally, where + "." + field.name);
        }
    }

    void check_properties(Linker& linker, const Package& package, PropertyTally& tally,
                          const PropertySchema* schema = nullptr)
    {
        const auto none = package.names().find("None");
        ASSERT_TRUE(none.has_value()) << package.name();
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ExportEntry& e = package.exports()[i];
            if (e.serial_size <= 0 || e.class_index.is_null())
            {
                continue;
            }
            const ObjectRef ref{&package, ObjectIndex::from_export(i)};
            ++tally.objects;
            try
            {
                const ObjectProperties result = linker.read_properties(ref, schema);
                tally.properties += result.properties.size();
                for (const auto& d : result.diagnostics.entries())
                {
                    ++tally.raw_values;
                    // Key: "<owner>.<property>" prefix of the message (up to the type).
                    const auto key = d.message.substr(0, d.message.find(" ("));
                    ++tally.raw_by_kind[key];
                }
                const std::string owner = package.class_name(ref.index);
                for (const auto& p : result.properties) count_arrays(p.value, tally, owner + "." + p.name);

                // Lossless round trip of the property block.
                BinaryWriter writer(package.context());
                write_properties(writer, result.properties, FName{*none, 0});
                const auto bytes = writer.take();
                const auto data = package.export_data(i);
                const auto original =
                    data.subspan(result.header.properties_offset, result.end_offset - result.header.properties_offset);
                if (!std::equal(bytes.begin(), bytes.end(), original.begin(), original.end()) &&
                    tally.round_trip_failures++ < 10)
                {
                    ADD_FAILURE() << linker.qualified_path(ref) << ": property round trip differs (" << bytes.size()
                                  << " vs " << original.size() << " bytes)";
                }
            }
            catch (const Error& error)
            {
                if (tally.failures++ < 10)
                {
                    ADD_FAILURE() << linker.qualified_path(ref) << " (class " << package.class_name(ref.index)
                                  << "): " << error.what();
                }
            }
        }
    }

    void report(const PropertyTally& tally)
    {
        std::cout << "  objects: " << tally.objects << ", properties: " << tally.properties
                  << ", failures: " << tally.failures << ", round-trip failures: " << tally.round_trip_failures
                  << "\n  raw values: " << tally.raw_values << ", arrays decoded/raw: " << tally.decoded_arrays << "/"
                  << tally.raw_arrays << '\n';
        std::vector<std::pair<std::size_t, std::string>> top;
        for (const auto& [key, count] : tally.raw_by_kind) top.emplace_back(count, key);
        std::sort(top.rbegin(), top.rend());
        for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 15); ++i)
        {
            std::cout << "    raw " << top[i].first << "x " << top[i].second << '\n';
        }
        std::vector<std::pair<std::size_t, std::string>> arrays;
        for (const auto& [key, count] : tally.raw_arrays_by_property) arrays.emplace_back(count, key);
        std::sort(arrays.rbegin(), arrays.rend());
        if (!arrays.empty()) std::cout << "  raw arrays in " << arrays.size() << " properties, most frequent:\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(arrays.size(), 15); ++i)
        {
            std::cout << "    " << arrays[i].first << "x " << arrays[i].second << '\n';
        }
        ::testing::Test::RecordProperty("objects", static_cast<int>(tally.objects));
        ::testing::Test::RecordProperty("properties", static_cast<int>(tally.properties));
        ::testing::Test::RecordProperty("raw_values", static_cast<int>(tally.raw_values));
        ::testing::Test::RecordProperty("raw_arrays", static_cast<int>(tally.raw_arrays));
    }
}

TEST(MirrorsEdgePropertyTests, ScriptPackageProperties)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    PropertyTally tally;
    for (const auto& file : packages(false))
    {
        const Package* package = linker.find_package(fs::path(file).stem().string());
        ASSERT_NE(package, nullptr) << file;
        check_properties(linker, *package, tally);
    }
    report(tally);
    EXPECT_EQ(tally.failures, 0u);
    EXPECT_EQ(tally.round_trip_failures, 0u);
    EXPECT_GT(tally.properties, 0u);
}

TEST(MirrorsEdgePropertyTests, FullCorpusProperties)
{
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    PropertyTally tally;
    for (const auto& file : packages(true))
    {
        Linker linker;
        linker.add_search_path(fixture_dir());
        const Package& package = linker.load(file);
        check_properties(linker, package, tally);
    }
    report(tally);
    EXPECT_EQ(tally.failures, 0u);
    EXPECT_EQ(tally.round_trip_failures, 0u);
}

TEST(MirrorsEdgePropertyTests, FullCorpusPropertiesWithReflectionSchema)
{
    // As FullCorpusProperties, with type information from the game's script packages (ReflectionSchema) instead of
    // the built-in schema, so arrays and structs declared in script are decoded too. Packages are processed in
    // batches that share one Linker and schema: loading the script packages once per corpus file would be slow, and
    // one Linker for the whole corpus would keep every package in memory.
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    constexpr std::size_t batch = 32;
    const auto files = packages(true);
    PropertyTally tally;
    std::size_t schema_problems = 0;
    for (std::size_t first = 0; first < files.size(); first += batch)
    {
        Linker linker;
        linker.add_search_path(fixture_dir());
        ReflectionSchema schema(linker);
        for (const Package* script : load_script_packages(linker)) schema.add_package(*script);
        for (std::size_t i = first; i < std::min(files.size(), first + batch); ++i)
        {
            check_properties(linker, linker.load(files[i]), tally, &schema);
        }
        schema_problems += schema.diagnostics().entries().size();
    }
    report(tally);
    std::cout << "  schema problems: " << schema_problems << '\n';
    EXPECT_EQ(tally.failures, 0u);
    EXPECT_EQ(tally.round_trip_failures, 0u);
    // With script type information every value and every array decodes (verified: 1,639,154 arrays).
    EXPECT_EQ(tally.raw_values, 0u);
    EXPECT_EQ(tally.raw_arrays, 0u);
    EXPECT_EQ(schema_problems, 0u);
}
