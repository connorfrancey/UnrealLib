// T3D export of real Mirror's Edge levels.
// Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set; the full map corpus needs UNREALLIB_FIXTURE_SWEEP=1.
//
// For every package under Maps/ that has a Level export:
//   * export_level() succeeds and exports one Actor block per Actor-derived export of the level;
//   * the exported document survives serialize() -> parse() unchanged and re-serializes to the same text;
//   * validate() reports no missing Class/Name, duplicate names or unresolved component references.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>
#include <UnrealLib/T3D/T3D.hpp>

#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
using namespace UnrealLib::TestSupport;
namespace fs = std::filesystem;

namespace
{
    struct T3DTally
    {
        std::size_t levels = 0;
        std::size_t actors = 0;
        std::size_t subobjects = 0;
        std::size_t properties = 0;
        std::size_t raw_values = 0;
        std::size_t failures = 0;
        std::map<std::string, std::size_t> warnings;
    };

    void count(const T3D::T3DBlock& b, T3DTally& tally)
    {
        if (b.type == "Actor") ++tally.actors;
        if (b.type == "Object") ++tally.subobjects;
        tally.properties += b.properties.size();
        for (const auto& c : b.children) count(c, tally);
    }

    bool has_level(const Package& package)
    {
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            if (package.class_name(ObjectIndex::from_export(i)) == "Level" &&
                (package.exports()[i].object_flags & ObjectFlag::ClassDefaultObject) == 0)
            {
                return true;
            }
        }
        return false;
    }

    void check_level(Linker& linker, const Package& package, const PropertySchema* schema, T3DTally& tally)
    {
        if (!has_level(package)) return;
        try
        {
            Diagnostics diagnostics;
            T3D::ExportOptions options;
            options.schema = schema;
            const T3D::T3DDocument doc = T3D::export_level(linker, package, options, &diagnostics);
            tally.raw_values += diagnostics.entries().size();
            ++tally.levels;

            ASSERT_EQ(doc.blocks.size(), 1u);
            std::size_t expected_actors = 0;
            for (const auto& level : doc.blocks[0].children)
            {
                const std::string level_name = level.attribute("NAME").value_or("");
                for (std::size_t i = 0; i < package.exports().size(); ++i)
                {
                    const ExportEntry& e = package.exports()[i];
                    if (!e.outer.is_null() && package.object_name(e.outer) == level_name &&
                        package.class_name(e.outer) == "Level" &&
                        linker.is_a(ObjectRef{&package, ObjectIndex::from_export(i)}, "Engine.Actor").value_or(false))
                    {
                        ++expected_actors;
                    }
                }
            }
            const std::size_t before = tally.actors;
            count(doc.blocks[0], tally);
            EXPECT_EQ(tally.actors - before, expected_actors) << package.name();

            const std::string text = T3D::serialize(doc);
            const T3D::T3DDocument back = T3D::parse(text);
            if (!(back == doc) || T3D::serialize(back) != text)
            {
                ++tally.failures;
                ADD_FAILURE() << package.name() << ": T3D text does not round-trip";
            }
            for (const Diagnostic& d : T3D::validate(back).entries())
            {
                ++tally.warnings[d.code];
                if (tally.warnings[d.code] <= 3) std::cout << "  " << package.name() << ": " << d.message << '\n';
            }
        }
        catch (const Error& e)
        {
            ++tally.failures;
            ADD_FAILURE() << package.name() << ": " << e.what();
        }
    }

    std::vector<fs::path> map_packages()
    {
        std::vector<fs::path> out;
        for (const auto& file : packages(true))
        {
            const auto rel = fs::relative(file, fixture_dir()).generic_string();
            if (Text::to_lower_ascii(rel).starts_with("maps/")) out.push_back(file);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    void report(const T3DTally& tally)
    {
        std::cout << "  levels: " << tally.levels << ", actors: " << tally.actors << ", subobjects: "
                  << tally.subobjects << ", properties: " << tally.properties
                  << ", raw-valued properties: " << tally.raw_values << ", failures: " << tally.failures << '\n';
        for (const auto& [code, n] : tally.warnings) std::cout << "  " << n << "x " << code << '\n';
    }

    void run(const std::vector<fs::path>& files)
    {
        // Batches share one Linker and ReflectionSchema (script packages are loaded once per batch).
        constexpr std::size_t batch = 32;
        T3DTally tally;
        for (std::size_t first = 0; first < files.size(); first += batch)
        {
            Linker linker;
            linker.add_search_path(fixture_dir());
            ReflectionSchema schema(linker);
            for (const Package* script : load_script_packages(linker)) schema.add_package(*script);
            for (std::size_t f = first; f < std::min(files.size(), first + batch); ++f)
            {
                check_level(linker, linker.load(files[f]), &schema, tally);
            }
        }
        report(tally);
        EXPECT_GT(tally.levels, 0u);
        EXPECT_GT(tally.actors, 0u);
        EXPECT_EQ(tally.failures, 0u);
        EXPECT_EQ(tally.warnings.count("t3d.actor-without-class"), 0u);
        EXPECT_EQ(tally.warnings.count("t3d.actor-without-name"), 0u);
        EXPECT_EQ(tally.warnings.count("t3d.duplicate-name"), 0u);
        EXPECT_EQ(tally.warnings.count("t3d.unresolved-subobject"), 0u);
    }
}

TEST(MirrorsEdgeT3DTests, SampleLevels)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    auto files = map_packages();
    // Every 40th map package keeps this quick.
    std::vector<fs::path> sample;
    for (std::size_t i = 0; i < files.size(); i += 40) sample.push_back(files[i]);
    run(sample);
}

TEST(MirrorsEdgeT3DTests, FullCorpusLevels)
{
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    run(map_packages());
}
