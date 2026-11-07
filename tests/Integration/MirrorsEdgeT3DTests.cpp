// T3D export of real Mirror's Edge levels.
// Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set; the full map corpus needs UNREALLIB_FIXTURE_SWEEP=1.
//
// For every package under Maps/ that has a Level export:
//   * export_level() succeeds and exports one Actor block per Actor-derived export of the level;
//   * the exported document survives serialize() -> parse() unchanged and re-serializes to the same text;
//   * validate() reports no missing Class/Name, duplicate names, unresolved component references or degenerate
//     polygons;
//   * every brush (Model subobject) is exported as a Begin Brush / PolyList block;
//   * import_document() of the unmodified text changes nothing (every value parses back exactly).
// EditRoundTrip edits one level through T3D, writes the package, reloads it and re-exports it.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>
#include <UnrealLib/T3D/T3D.hpp>
#include <UnrealLib/Writer/PackageWriter.hpp>

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
        std::size_t brushes = 0;
        std::size_t polygons = 0;
        std::size_t brushes_not_decoded = 0;
        std::size_t imported_objects = 0;
        std::size_t import_failures = 0;
        std::size_t failures = 0;
        std::map<std::string, std::size_t> warnings;
    };

    void count(const T3D::T3DBlock& b, T3DTally& tally)
    {
        if (b.type == "Actor") ++tally.actors;
        if (b.type == "Object") ++tally.subobjects;
        if (b.type == "Brush") ++tally.brushes;
        if (b.type == "Polygon") ++tally.polygons;
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
            for (const Diagnostic& d : diagnostics.entries())
            {
                if (d.code == "t3d.brush-not-decoded")
                {
                    if (tally.brushes_not_decoded++ < 3) std::cout << "  " << d.message << '\n';
                }
                else
                {
                    ++tally.raw_values;
                }
            }
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

            // Importing the unmodified text must parse every value back to exactly what is stored: no object
            // changes, nothing skipped.
            PackageModel model = PackageModel::from_package(package);
            Diagnostics import_diagnostics;
            T3D::ImportOptions import_options;
            import_options.schema = schema;
            const T3D::ImportResult imported = T3D::import_document(model, linker, package, back, import_options,
                                                                    &import_diagnostics);
            tally.imported_objects += imported.objects;
            if (imported.modified != 0 || imported.brushes_modified != 0 || imported.skipped != 0 ||
                !import_diagnostics.entries().empty())
            {
                ++tally.import_failures;
                std::string details;
                for (const Diagnostic& d : import_diagnostics.entries()) details += "\n    " + d.code + ": " + d.message;
                ADD_FAILURE() << package.name() << ": import of the unmodified export changed " << imported.modified
                              << " objects and " << imported.brushes_modified << " brushes, skipped "
                              << imported.skipped << details;
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
                  << ", raw-valued properties: " << tally.raw_values << ", failures: " << tally.failures << '\n'
                  << "  brushes: " << tally.brushes << ", polygons: " << tally.polygons
                  << ", brushes not decoded: " << tally.brushes_not_decoded << '\n'
                  << "  imported objects: " << tally.imported_objects << ", import failures: " << tally.import_failures
                  << '\n';
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
        EXPECT_EQ(tally.warnings.count("t3d.degenerate-polygon"), 0u);
        EXPECT_GT(tally.brushes, 0u);
        EXPECT_GT(tally.polygons, 0u);
        EXPECT_EQ(tally.brushes_not_decoded, 0u);
        EXPECT_GT(tally.imported_objects, 0u);
        EXPECT_EQ(tally.import_failures, 0u);
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

namespace
{
    T3D::T3DBlock* find_block(T3D::T3DBlock& b, const std::function<bool(const T3D::T3DBlock&)>& match)
    {
        if (match(b)) return &b;
        for (T3D::T3DBlock& c : b.children)
        {
            if (T3D::T3DBlock* found = find_block(c, match)) return found;
        }
        return nullptr;
    }
}

TEST(MirrorsEdgeT3DTests, EditRoundTrip)
{
    // Moves an actor, gives another one a property it did not have (typed from the schema), moves a brush vertex;
    // writes the package, reloads it and re-exports: the result must be exactly the edited document.
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    const auto files = map_packages();
    const auto it = std::find_if(files.begin(), files.end(),
                                 [](const fs::path& f) { return Text::iequals(f.stem().string(), "Subway_Train"); });
    if (it == files.end())
    {
        GTEST_SKIP() << "Subway_Train is not in the fixtures";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    ReflectionSchema schema(linker);
    for (const Package* script : load_script_packages(linker)) schema.add_package(*script);
    const Package& package = linker.load(*it);
    T3D::ExportOptions export_options;
    export_options.schema = &schema;
    T3D::T3DDocument doc = T3D::export_level(linker, package, export_options);

    T3D::T3DBlock& map = doc.blocks.at(0);
    T3D::T3DBlock* moved = find_block(map, [](const T3D::T3DBlock& b) {
        return b.type == "Actor" && b.property("Location") != nullptr;
    });
    ASSERT_NE(moved, nullptr);
    for (T3D::T3DProperty& p : moved->properties)
    {
        if (p.name == "Location") p.value = "(X=1.0,Y=-2.5,Z=3.0)";
    }
    T3D::T3DBlock* scaled = find_block(map, [&](const T3D::T3DBlock& b) {
        return b.type == "Actor" && &b != moved && b.property("DrawScale") == nullptr;
    });
    ASSERT_NE(scaled, nullptr);
    scaled->properties.push_back(T3D::T3DProperty{"DrawScale", std::nullopt, false, "2.5", 0});
    scaled->layout += 'p';
    T3D::T3DBlock* polygon = find_block(map, [](const T3D::T3DBlock& b) { return b.type == "Polygon"; });
    ASSERT_NE(polygon, nullptr);
    ASSERT_GE(polygon->lines.size(), 7u);
    polygon->lines[4] = "Vertex   +00001.500000,+00002.000000,+00003.000000";

    PackageModel model = PackageModel::from_package(package);
    Diagnostics diagnostics;
    T3D::ImportOptions import_options;
    import_options.schema = &schema;
    const T3D::ImportResult result =
        T3D::import_document(model, linker, package, T3D::parse(T3D::serialize(doc)), import_options, &diagnostics);
    for (const Diagnostic& d : diagnostics.entries())
    {
        if (d.severity != Severity::Info) ADD_FAILURE() << d.code << ": " << d.message;
    }
    EXPECT_EQ(diagnostics.count(Severity::Info), 2u); // one t3d.import-modified per modified actor
    EXPECT_EQ(result.modified, 2u);
    EXPECT_EQ(result.brushes_modified, 1u);
    EXPECT_EQ(result.skipped, 0u);

    Package reloaded;
    reloaded.load_from_memory(write_package(model), package.name());
    Linker linker2;
    linker2.add_search_path(fixture_dir());
    ReflectionSchema schema2(linker2);
    for (const Package* script : load_script_packages(linker2)) schema2.add_package(*script);
    const Package& written = linker2.add(std::move(reloaded));
    export_options.schema = &schema2;
    EXPECT_EQ(T3D::serialize(T3D::export_level(linker2, written, export_options)), T3D::serialize(doc));
}
