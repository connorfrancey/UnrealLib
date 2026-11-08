#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using namespace UnrealLib::Testing;
namespace fs = std::filesystem;

namespace
{
    // A three-package world:
    //   Core:   classes Object, Component (super Object); Default__Component (CDO).
    //   Engine: imports Core.Object/Core.Component/Core.ArrayProperty (intrinsic) and Game.GameInfo (cycle);
    //           classes Actor (super Core.Object), ActorComponent (super Core.Component);
    //           Default__ActorComponent (CDO, class ActorComponent) with component template data;
    //           broken imports: Missing.Thing, Engine.Nope, Core.Object as class Struct.
    //   Game:   imports Engine.Actor; class GameInfo (super Engine.Actor); an instance of GameInfo.
    class LinkerTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            m_dir = fs::temp_directory_path() / (std::string("unreallib_linker_") + info->name());
            fs::remove_all(m_dir);
            fs::create_directories(m_dir / "Sub");

            write("Core.u", core_spec());
            write("Engine.u", engine_spec());
            write("Sub/Game.u", game_spec()); // found through recursive search
        }

        void TearDown() override
        {
            std::error_code ec;
            fs::remove_all(m_dir, ec);
        }

        static SyntheticPackageSpec base_spec()
        {
            SyntheticPackageSpec spec;
            spec.names = {"None"};
            return spec;
        }

        static SyntheticPackageSpec core_spec()
        {
            auto spec = base_spec();
            const auto object = add_export(spec, {}, {}, {}, "Object", ObjectFlag::Native);
            const auto component = add_export(spec, {}, object, {}, "Component", ObjectFlag::Native);
            // Default__Component: a class default object -> NetIndex + properties only.
            BinaryWriter cdo(SerializationContext{536, 43});
            cdo.write_i32(2);
            write_fname(cdo, intern(spec, "None"));
            add_export(spec, component, {}, {}, "Default__Component", ObjectFlag::ClassDefaultObject, cdo.take());
            return spec;
        }

        static SyntheticPackageSpec engine_spec()
        {
            auto spec = base_spec();
            const auto core = add_import(spec, "Core", "Package", {}, "Core");
            const auto core_object = add_import(spec, "Core", "Class", core, "Object");
            const auto core_component = add_import(spec, "Core", "Class", core, "Component");
            add_import(spec, "Core", "Class", core, "ArrayProperty"); // intrinsic
            const auto game = add_import(spec, "Core", "Package", {}, "Game");
            add_import(spec, "Core", "Class", game, "GameInfo");       // cycle: Game imports Engine too
            const auto missing = add_import(spec, "Core", "Package", {}, "Missing");
            add_import(spec, "Engine", "Thing", missing, "Thing");
            add_import(spec, "Core", "Package", {}, "Core2");
            add_import(spec, "Core", "Struct", core, "Object");        // class mismatch
            add_import(spec, "Engine", "Texture2D", core, "Nope2");    // not found, not a class

            add_export(spec, {}, core_object, {}, "Actor");
            const auto actor_component = add_export(spec, {}, core_component, {}, "ActorComponent");

            // Class default object of a component class: NetIndex + properties only (no template data).
            BinaryWriter cdo(SerializationContext{536, 43});
            cdo.write_i32(2);                         // NetIndex
            write_fname(cdo, intern(spec, "None"));   // first property tag
            add_export(spec, actor_component, {}, {}, "Default__ActorComponent", ObjectFlag::ClassDefaultObject,
                       cdo.take());

            // A component sub-object of the CDO: owner class (null in cooked data), TemplateName (an outer is a CDO),
            // NetIndex.
            BinaryWriter sub(SerializationContext{536, 43});
            sub.write_i32(0);
            write_fname(sub, intern(spec, "Child"));
            sub.write_i32(3);
            write_fname(sub, intern(spec, "None"));
            add_export(spec, actor_component, {}, ObjectIndex::from_export(2), "Child", 0, sub.take());
            return spec;
        }

        static SyntheticPackageSpec game_spec()
        {
            auto spec = base_spec();
            const auto engine = add_import(spec, "Core", "Package", {}, "Engine");
            const auto actor = add_import(spec, "Core", "Class", engine, "Actor");
            const auto game_info = add_export(spec, {}, actor, {}, "GameInfo");
            BinaryWriter instance(SerializationContext{536, 43});
            instance.write_i32(7);
            write_fname(instance, intern(spec, "None"));
            add_export(spec, game_info, {}, {}, "GameInfo", 0, instance.take()); // name clash is fine: GameInfo_0 below
            spec.exports.back().entry.object_name.number = 1;                       // "GameInfo_0"
            return spec;
        }

        void write(const std::string& relative, const SyntheticPackageSpec& spec)
        {
            const auto package = build_synthetic_package(spec);
            std::ofstream out(m_dir / relative, std::ios::binary);
            out.write(reinterpret_cast<const char*>(package.file.data()),
                      static_cast<std::streamsize>(package.file.size()));
        }

        fs::path m_dir;
    };

    ObjectIndex import_named(const Package& package, std::string_view path)
    {
        const auto index = package.find_import(path);
        EXPECT_TRUE(index.has_value()) << path;
        return index.value_or(ObjectIndex{});
    }
}

TEST_F(LinkerTest, LoadsPackagesLazilyFromSearchPaths)
{
    Linker linker;
    linker.add_search_path(m_dir);
    EXPECT_EQ(linker.indexed_count(), 3u);
    EXPECT_EQ(linker.loaded_count(), 0u);
    const Package* engine = linker.find_package("ENGINE");
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(linker.loaded_count(), 1u);
    EXPECT_EQ(linker.find_package("engine"), engine);
    EXPECT_EQ(linker.find_package("DoesNotExist"), nullptr);
}

TEST_F(LinkerTest, ResolvesImportsAcrossPackages)
{
    Linker linker;
    linker.add_search_path(m_dir);
    const Package& engine = *linker.find_package("Engine");

    const Resolution& object = linker.resolve(engine, import_named(engine, "Core.Object"));
    ASSERT_EQ(object.status, ResolveStatus::Export) << object.detail;
    EXPECT_EQ(object.target.package->name(), "Core");
    EXPECT_EQ(linker.qualified_path(object.target), "Core.Object");

    const Resolution& package = linker.resolve(engine, import_named(engine, "Core"));
    EXPECT_EQ(package.status, ResolveStatus::Package);
    EXPECT_EQ(package.target.package->name(), "Core");

    EXPECT_EQ(linker.resolve(engine, ObjectIndex{}).status, ResolveStatus::Null);
    EXPECT_EQ(linker.resolve(engine, ObjectIndex{999}).status, ResolveStatus::Invalid);
    EXPECT_EQ(linker.resolve(engine, ObjectIndex::from_export(0)).status, ResolveStatus::Export);
}

TEST_F(LinkerTest, ReportsUnresolvableImports)
{
    Linker linker;
    linker.add_search_path(m_dir);
    const Package& engine = *linker.find_package("Engine");

    EXPECT_EQ(linker.resolve(engine, import_named(engine, "Core.ArrayProperty")).status, ResolveStatus::Intrinsic);
    EXPECT_EQ(linker.resolve(engine, import_named(engine, "Missing.Thing")).status, ResolveStatus::PackageNotFound);
    EXPECT_EQ(linker.resolve(engine, import_named(engine, "Core2")).status, ResolveStatus::PackageNotFound);
    EXPECT_EQ(linker.resolve(engine, import_named(engine, "Core.Nope2")).status, ResolveStatus::ObjectNotFound);

    // Core.Object imported as a Struct: the export exists but is a Class.
    std::optional<ObjectIndex> mismatch;
    for (std::size_t i = 0; i < engine.imports().size(); ++i)
    {
        if (engine.names().to_string(engine.imports()[i].class_name) == "Struct")
        {
            mismatch = ObjectIndex::from_import(i);
        }
    }
    ASSERT_TRUE(mismatch.has_value());
    const Resolution& r = linker.resolve(engine, *mismatch);
    EXPECT_EQ(r.status, ResolveStatus::ClassMismatch);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.target.valid());
}

TEST_F(LinkerTest, HandlesCyclicPackageDependencies)
{
    Linker linker;
    linker.add_search_path(m_dir);
    const Package& game = *linker.find_package("Game");
    const Package& engine = *linker.find_package("Engine");

    // Game -> Engine.Actor and Engine -> Game.GameInfo.
    const Resolution& actor = linker.resolve(game, import_named(game, "Engine.Actor"));
    ASSERT_EQ(actor.status, ResolveStatus::Export);
    const Resolution& game_info = linker.resolve(engine, import_named(engine, "Game.GameInfo"));
    ASSERT_EQ(game_info.status, ResolveStatus::Export) << game_info.detail;
    EXPECT_EQ(game_info.target.package, &game);
    EXPECT_EQ(linker.loaded_count(), 2u); // resolving never loaded Core
}

TEST_F(LinkerTest, ClassHierarchyQueries)
{
    Linker linker;
    linker.add_search_path(m_dir);
    const Package& game = *linker.find_package("Game");
    const Package& engine = *linker.find_package("Engine");

    const ObjectRef instance{&game, ObjectIndex::from_export(1)}; // GameInfo_0 : GameInfo : Engine.Actor : Core.Object
    EXPECT_EQ(linker.qualified_path(instance), "Game.GameInfo_0");
    EXPECT_EQ(linker.is_a(instance, "Game.GameInfo"), std::optional<bool>(true));
    EXPECT_EQ(linker.is_a(instance, "Engine.Actor"), std::optional<bool>(true));
    EXPECT_EQ(linker.is_a(instance, "core.object"), std::optional<bool>(true));
    EXPECT_EQ(linker.is_a(instance, "Core.Component"), std::optional<bool>(false));

    const ObjectRef cdo{&engine, ObjectIndex::from_export(2)};
    EXPECT_EQ(linker.is_a(cdo, "Core.Component"), std::optional<bool>(true));

    const ObjectRef actor_class{&engine, ObjectIndex::from_export(0)};
    EXPECT_EQ(linker.is_a(actor_class, "Core.Class"), std::optional<bool>(true));
}

TEST_F(LinkerTest, ReadsObjectHeadersUsingTheClassHierarchy)
{
    Linker linker;
    linker.add_search_path(m_dir);
    const Package& engine = *linker.find_package("Engine");
    const Package& game = *linker.find_package("Game");

    // CDO of a component class: NetIndex only.
    const ObjectHeader cdo = linker.read_header({&engine, ObjectIndex::from_export(2)});
    EXPECT_FALSE(cdo.component.has_value());
    EXPECT_EQ(cdo.net_index, std::optional<std::int32_t>(2));
    EXPECT_EQ(cdo.properties_offset, 4u);

    // Component sub-object of a CDO: owner class, template name (an outer is a CDO), net index.
    const ObjectHeader child = linker.read_header({&engine, ObjectIndex::from_export(3)});
    ASSERT_TRUE(child.component && child.component->template_name);
    EXPECT_TRUE(child.component->owner_class.is_null());
    EXPECT_EQ(engine.names().to_string(*child.component->template_name), "Child");
    EXPECT_EQ(child.net_index, std::optional<std::int32_t>(3));
    EXPECT_EQ(child.properties_offset, 16u);

    // Non-component: only the net index.
    const ObjectHeader plain = linker.read_header({&game, ObjectIndex::from_export(1)});
    EXPECT_FALSE(plain.component.has_value());
    EXPECT_EQ(plain.net_index, std::optional<std::int32_t>(7));
    EXPECT_EQ(plain.properties_offset, 4u);
}

TEST_F(LinkerTest, HeaderNeedsResolvableClass)
{
    // Without search paths Engine cannot see Core, so the component check is undecidable.
    Linker linker;
    const Package& engine = linker.load(m_dir / "Engine.u");
    EXPECT_FALSE(linker.header_input({&engine, ObjectIndex::from_export(2)}).has_value());
    EXPECT_THROW((void)linker.read_header({&engine, ObjectIndex::from_export(2)}), Error);
}

TEST_F(LinkerTest, ResolvesImportsNestedInForcedExports)
{
    // Textures.upk: group "Group" (a Package) containing "Tex" (an Engine.Texture2D).
    auto textures = base_spec();
    const auto engine = add_import(textures, "Core", "Package", {}, "Engine");
    const auto texture_class = add_import(textures, "Core", "Class", engine, "Texture2D");
    const auto core = add_import(textures, "Core", "Package", {}, "Core");
    const auto package_class = add_import(textures, "Core", "Class", core, "Package");
    const auto group = add_export(textures, package_class, {}, {}, "Group");
    add_export(textures, texture_class, {}, group, "Tex");
    write("Textures.upk", textures);

    // Map: seek-free cooked; forced exports stand in for Textures and Textures.Group, the texture stays an import.
    auto map = base_spec();
    const auto map_core = add_import(map, "Core", "Package", {}, "Core");
    const auto map_package_class = add_import(map, "Core", "Class", map_core, "Package");
    const auto forced_package = add_export(map, map_package_class, {}, {}, "Textures");
    const auto forced_group = add_export(map, map_package_class, {}, forced_package, "Group");
    map.exports[0].entry.export_flags = ExportFlag::ForcedExport;
    map.exports[1].entry.export_flags = ExportFlag::ForcedExport;
    add_import(map, "Engine", "Texture2D", forced_group, "Tex");
    add_export(map, map_package_class, {}, forced_package, "Local"); // an object cooked into the forced package
    map.exports.back().entry.export_flags = 0;
    // Not allowed: an import nested in a regular (non-forced) export.
    const auto regular = add_export(map, map_package_class, {}, {}, "Regular");
    add_import(map, "Engine", "Texture2D", regular, "Tex");
    write("Map.me1", map);

    Linker linker;
    linker.add_search_path(m_dir);
    const Package& map_package = *linker.find_package("Map");

    const Resolution& tex = linker.resolve(map_package, import_named(map_package, "Textures.Group.Tex"));
    ASSERT_EQ(tex.status, ResolveStatus::Export) << tex.detail;
    EXPECT_EQ(tex.target.package->name(), "Textures");
    EXPECT_EQ(linker.qualified_path(tex.target), "Textures.Group.Tex");

    // Exports inside a forced package report the original package's path, not the map's.
    EXPECT_EQ(linker.qualified_path({&map_package, ObjectIndex::from_export(2)}), "Textures.Local");
    EXPECT_EQ(linker.qualified_path({&map_package, ObjectIndex::from_export(3)}), "Map.Regular");

    const Resolution& invalid = linker.resolve(map_package, import_named(map_package, "Regular.Tex"));
    EXPECT_EQ(invalid.status, ResolveStatus::Invalid);
}

TEST_F(LinkerTest, ResolvesImportsThroughForcedExportsInOtherPackages)
{
    // Seek-free cooking without the original file: "Sounds" exists only as a forced export inside LevelA, and LevelB
    // imports Sounds.Cue. With the forced-export index the import resolves to LevelA's copy.
    auto level_a = base_spec();
    const auto core_a = add_import(level_a, "Core", "Package", {}, "Core");
    const auto package_class_a = add_import(level_a, "Core", "Class", core_a, "Package");
    const auto engine_a = add_import(level_a, "Core", "Package", {}, "Engine");
    const auto cue_class_a = add_import(level_a, "Core", "Class", engine_a, "SoundCue");
    const auto forced = add_export(level_a, package_class_a, {}, {}, "Sounds");
    level_a.exports.back().entry.export_flags = ExportFlag::ForcedExport;
    add_export(level_a, cue_class_a, {}, forced, "Cue");
    write("LevelA.upk", level_a);

    auto level_b = base_spec();
    const auto core_b = add_import(level_b, "Core", "Package", {}, "Core");
    add_import(level_b, "Core", "Class", core_b, "Package");
    const auto sounds = add_import(level_b, "Core", "Package", {}, "Sounds");
    add_import(level_b, "Engine", "SoundCue", sounds, "Cue");
    add_import(level_b, "Engine", "SoundCue", sounds, "Missing");
    write("LevelB.upk", level_b);

    Linker linker;
    linker.add_search_path(m_dir);
    const Package& b = *linker.find_package("LevelB");
    EXPECT_EQ(linker.resolve(b, import_named(b, "Sounds.Cue")).status, ResolveStatus::PackageNotFound);

    EXPECT_EQ(linker.index_forced_exports(), 1u);
    ASSERT_EQ(linker.forced_export_index().count("Sounds"), 1u);
    const Resolution& cue = linker.resolve(b, import_named(b, "Sounds.Cue"));
    ASSERT_EQ(cue.status, ResolveStatus::Export) << cue.detail;
    EXPECT_EQ(cue.target.package->name(), "LevelA");
    EXPECT_EQ(linker.qualified_path(cue.target), "Sounds.Cue");
    EXPECT_EQ(linker.resolve(b, import_named(b, "Sounds.Missing")).status, ResolveStatus::ObjectNotFound);

    // The index can be shared with another linker over the same files.
    Linker other;
    other.add_search_path(m_dir);
    other.set_forced_export_index(linker.forced_export_index());
    const Package& b2 = *other.find_package("LevelB");
    EXPECT_EQ(other.resolve(b2, import_named(b2, "Sounds.Cue")).status, ResolveStatus::Export);
}

TEST_F(LinkerTest, DuplicatePackagesAndLoadFailures)
{
    // A corrupt package with the same stem in a later search path is ignored (first wins) ...
    fs::create_directories(m_dir / "Later");
    {
        std::ofstream out(m_dir / "Later" / "Core.u", std::ios::binary);
        out << "garbage";
    }
    Linker linker;
    linker.add_search_path(m_dir, false);
    linker.add_search_path(m_dir / "Later");
    EXPECT_NE(linker.find_package("Core"), nullptr);
    EXPECT_EQ(linker.diagnostics().count(Severity::Warning), 1u);

    // ... but a corrupt package that is the only candidate fails to load and is reported once.
    Linker broken;
    broken.add_search_path(m_dir / "Later");
    EXPECT_EQ(broken.find_package("Core"), nullptr);
    EXPECT_EQ(broken.find_package("Core"), nullptr);
    EXPECT_EQ(broken.diagnostics().count(Severity::Error), 1u);
}
