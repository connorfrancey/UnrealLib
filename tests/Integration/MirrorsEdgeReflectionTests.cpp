// Reflection / bytecode integration tests against real Mirror's Edge packages.
// Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set; the whole-corpus variant also needs UNREALLIB_FIXTURE_SWEEP=1.
//
// Invariants:
//   * every reflection export (class, state, function, struct, enum, const, property, text buffer) deserializes
//     and consumes its serialized data exactly;
//   * every script's bytecode decodes to tokens that end exactly at ScriptSize, with the Mirror's Edge token map,
//     and encodes back (encode_script) to the same bytes and the same ScriptSize;
//   * assemble_script lays scripts out again correctly: unchanged, byte-identical; edited (a statement in front, a
//     marker parameter added to every native call: every jump and enclosing skip moves), the result decodes, its jumps
//     land on statements, and taking the edits out again gives the original bytes.
//   * with the reflection schema, script-package properties decode without raw values, and the Core math struct
//     layouts derived from reflection equal the built-in ones.

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include <algorithm>

#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
using namespace UnrealLib::TestSupport;
namespace fs = std::filesystem;

namespace
{
    struct ReflectionTally
    {
        std::map<std::string, std::size_t> kinds;
        std::size_t failures = 0;
        std::size_t scripts = 0;
        std::size_t tokens = 0;
        std::size_t script_failures = 0;
        std::size_t encode_failures = 0;
        std::size_t assemble_failures = 0;
        std::map<std::string, std::size_t> token_kinds;
    };

    void check_assemble(const std::string& what, const Package& package, const Script& script, const TokenMap& map,
                        std::span<const std::byte> original, const BytecodeOptions& options, ReflectionTally& tally);

    void check_package(Linker& linker, const Package& package, ReflectionTally& tally)
    {
        const TokenMap map = TokenMap::ue3(package.summary().file_version, &package.profile());
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ObjectRef ref{&package, ObjectIndex::from_export(i)};
            if (!field_kind_of(package.class_name(ref.index)) || package.exports()[i].serial_size <= 0)
            {
                continue;
            }
            try
            {
                const FieldObject field = linker.read_field(ref);
                ++tally.kinds[std::string(to_string(field.kind))];
                const auto size = static_cast<std::size_t>(package.exports()[i].serial_size);
                if (field.end_offset != size && tally.failures++ < 10)
                {
                    ADD_FAILURE() << linker.qualified_path(ref) << " (" << package.class_name(ref.index)
                                  << "): consumed " << field.end_offset << " of " << size << " bytes";
                }
                if (field.structure && field.structure->script_size > 0)
                {
                    ++tally.scripts;
                    const auto data = package.export_data(i);
                    BinaryReader reader(data, package.context());
                    reader.seek(field.structure->script_offset);
                    BytecodeOptions options;
                    options.token_map = &map;
                    options.name_count = static_cast<std::int32_t>(package.names().size());
                    options.none_index = package.names().find("None").value_or(-1);
                    try
                    {
                        const Script script =
                            decode_script(reader, static_cast<std::size_t>(field.structure->script_size), options);
                        tally.tokens += script.tokens.size();
                        for (const Token& t : script.tokens) ++tally.token_kinds[std::string(to_string(t.kind))];
                        EXPECT_EQ(script.storage_size, field.structure->script_storage_size)
                            << linker.qualified_path(ref);

                        // Round trip: the same bytes, the same in-memory size
                        BinaryWriter writer(package.context());
                        const std::size_t memory = encode_script(writer, script, map);
                        const auto original = data.subspan(field.structure->script_offset, script.storage_size);
                        const auto encoded = writer.data();
                        if (memory != static_cast<std::size_t>(field.structure->script_size) ||
                            !std::equal(encoded.begin(), encoded.end(), original.begin(), original.end()))
                        {
                            if (tally.encode_failures++ < 10)
                            {
                                const auto diff = std::mismatch(encoded.begin(), encoded.end(), original.begin(), original.end());
                                ADD_FAILURE() << linker.qualified_path(ref) << ": encoded " << encoded.size() << " bytes ("
                                              << memory << " in memory) vs " << original.size() << " ("
                                              << field.structure->script_size << "), first difference at byte "
                                              << (diff.first - encoded.begin());
                            }
                        }
                        check_assemble(linker.qualified_path(ref), package, script, map, original, options, tally);
                    }
                    catch (const Error& e)
                    {
                        if (tally.script_failures++ < 10)
                        {
                            ADD_FAILURE() << linker.qualified_path(ref) << ": bytecode: " << e.what();
                        }
                    }
                }
            }
            catch (const Error& e)
            {
                if (tally.failures++ < 10)
                {
                    ADD_FAILURE() << linker.qualified_path(ref) << " (" << package.class_name(ref.index)
                                  << "): " << e.what();
                }
            }
        }
    }

    // See the invariants above. Failures are counted in tally.assemble_failures (the first few reported).
    void check_assemble(const std::string& what, const Package& package, const Script& script, const TokenMap& map,
                        std::span<const std::byte> original, const BytecodeOptions& options, ReflectionTally& tally)
    {
        constexpr std::int32_t marker = 0x7EADBEEF;
        const auto fail = [&](const std::string& why) {
            if (tally.assemble_failures++ < 10) ADD_FAILURE() << what << ": assemble: " << why;
        };
        const auto bytes_of = [](const BinaryWriter& w) { return std::vector<std::byte>(w.data().begin(), w.data().end()); };
        try
        {
            BinaryWriter same(package.context());
            assemble_script(same, script, map);
            if (bytes_of(same) != std::vector<std::byte>(original.begin(), original.end())) return fail("unchanged script differs");

            // Edit: a Nothing statement in front, a marker IntConst as the last parameter of every native call
            Script edited = script;
            Token nothing;
            nothing.kind = TokenKind::Nothing;
            edited.tokens.push_back(nothing);
            edited.statements.insert(edited.statements.begin(), static_cast<std::uint32_t>(edited.tokens.size() - 1));
            for (std::size_t i = 0; i < script.tokens.size(); ++i)
            {
                if (edited.tokens[i].kind != TokenKind::NativeFunction) continue;
                Token m;
                m.kind = TokenKind::IntConst;
                m.int_value = marker;
                edited.tokens.push_back(m);
                auto& kids = edited.tokens[i].children;
                const bool debug = !kids.empty() && edited.tokens[kids.back()].kind == TokenKind::DebugInfo;
                kids.insert(debug ? kids.end() - 1 : kids.end(), static_cast<std::uint32_t>(edited.tokens.size() - 1));
            }
            BinaryWriter w(package.context());
            const std::size_t memory = assemble_script(w, edited, map);
            const auto bytes = bytes_of(w);
            BinaryReader r(std::span<const std::byte>(bytes), package.context());
            const Script again = decode_script(r, memory, options);
            if (again.statements.size() != script.statements.size() + 1) return fail("statement count changed");
            for (const Token& t : again.tokens)
            {
                const bool jump = t.kind == TokenKind::Jump || t.kind == TokenKind::JumpIfNot || t.kind == TokenKind::Iterator ||
                                  t.kind == TokenKind::DynamicArrayIterator || (t.kind == TokenKind::Case && t.word != 0xFFFF);
                if (!jump || t.word == memory) continue;
                if (std::none_of(again.statements.begin(), again.statements.end(),
                                 [&](std::uint32_t st) { return again.tokens[st].memory_offset == t.word; }))
                    return fail(std::string(to_string(t.kind)) + " no longer lands on a statement");
            }

            // Undo: drop the first statement and the markers, lay out again
            Script undone = again;
            undone.statements.erase(undone.statements.begin());
            for (Token& t : undone.tokens)
                std::erase_if(t.children, [&](std::uint32_t c) {
                    return undone.tokens[c].kind == TokenKind::IntConst && undone.tokens[c].int_value == marker;
                });
            BinaryWriter back(package.context());
            assemble_script(back, undone, map);
            if (bytes_of(back) != std::vector<std::byte>(original.begin(), original.end())) fail("edited and undone differs");
        }
        catch (const Error& e)
        {
            fail(e.what());
        }
    }

    void report(const ReflectionTally& tally)
    {
        for (const auto& [kind, count] : tally.kinds) std::cout << "  " << kind << ": " << count << '\n';
        std::cout << "  scripts: " << tally.scripts << ", tokens: " << tally.tokens
                  << ", failures: " << tally.failures << ", script failures: " << tally.script_failures
                  << ", encode failures: " << tally.encode_failures << ", assemble failures: " << tally.assemble_failures << '\n';
        ::testing::Test::RecordProperty("scripts", static_cast<int>(tally.scripts));
        ::testing::Test::RecordProperty("tokens", static_cast<int>(tally.tokens));
    }

}

TEST(MirrorsEdgeReflectionTests, ScriptPackageReflectionAndBytecode)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    ReflectionTally tally;
    for (const Package* package : load_script_packages(linker))
    {
        check_package(linker, *package, tally);
    }
    report(tally);
    EXPECT_EQ(tally.failures, 0u);
    EXPECT_EQ(tally.script_failures, 0u);
    EXPECT_EQ(tally.encode_failures, 0u);
    EXPECT_EQ(tally.assemble_failures, 0u);
    EXPECT_GT(tally.scripts, 1000u);
}

TEST(MirrorsEdgeReflectionTests, ReflectionLayoutsMatchBuiltinStructs)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    ReflectionSchema schema(linker);
    schema.add_package(*linker.find_package("Core"));
    for (const char* name : {"Vector", "Vector2D", "Vector4", "Plane", "Quat", "Rotator", "Color", "LinearColor",
                             "Guid", "IntPoint", "Box", "Matrix", "TwoVectors"})
    {
        SCOPED_TRACE(name);
        const StructLayout* builtin = BuiltinPropertySchema::instance().find_struct(name);
        const StructLayout* reflected = schema.find_struct(name);
        ASSERT_NE(builtin, nullptr);
        ASSERT_NE(reflected, nullptr);
        EXPECT_TRUE(reflected->binary);
        ASSERT_EQ(reflected->fields.size(), builtin->fields.size());
        for (std::size_t i = 0; i < builtin->fields.size(); ++i)
        {
            EXPECT_EQ(reflected->fields[i].name, builtin->fields[i].name);
            EXPECT_EQ(reflected->fields[i].type.type, builtin->fields[i].type.type);
            EXPECT_EQ(reflected->fields[i].type.struct_name, builtin->fields[i].type.struct_name);
        }
    }
}

TEST(MirrorsEdgeReflectionTests, ScriptPackagePropertiesWithReflectionSchema)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    ReflectionSchema schema(linker);
    const auto scripts = load_script_packages(linker);
    for (const Package* p : scripts) schema.add_package(*p);

    std::size_t raw_values = 0, raw_arrays = 0, decoded_arrays = 0, failures = 0;
    std::map<std::string, std::size_t> raw_examples;
    std::function<void(const PropertyValue&, const std::string&)> walk = [&](const PropertyValue& v,
                                                                             const std::string& where) {
        if (const auto* a = std::get_if<ArrayValue>(&v.value))
        {
            if (a->decoded()) ++decoded_arrays;
            else if (raw_arrays++ , raw_examples[where]++ == 0 && raw_examples.size() <= 15)
                std::cout << "    raw array: " << where << '\n';
            for (const auto& e : a->elements) walk(e, where);
        }
        else if (const auto* s = std::get_if<StructValue>(&v.value))
        {
            for (const auto& f : s->fields) walk(f.value, where + "." + f.name);
        }
    };
    for (const Package* package : scripts)
    {
        for (std::size_t i = 0; i < package->exports().size(); ++i)
        {
            const ExportEntry& e = package->exports()[i];
            if (e.serial_size <= 0 || e.class_index.is_null()) continue;
            const ObjectRef ref{package, ObjectIndex::from_export(i)};
            try
            {
                const ObjectProperties result = linker.read_properties(ref, &schema);
                raw_values += result.diagnostics.entries().size();
                for (const auto& d : result.diagnostics.entries())
                {
                    if (raw_values <= 10) std::cout << "    " << d.message << '\n';
                }
                for (const auto& p : result.properties)
                    walk(p.value, package->class_name(ref.index) + "." + p.name);
            }
            catch (const Error& err)
            {
                if (failures++ < 10) ADD_FAILURE() << linker.qualified_path(ref) << ": " << err.what();
            }
        }
    }
    std::cout << "  arrays decoded/raw: " << decoded_arrays << "/" << raw_arrays << ", raw values: " << raw_values
              << ", schema problems: " << schema.diagnostics().entries().size() << '\n';
    for (std::size_t i = 0; i < schema.diagnostics().entries().size() && i < 10; ++i)
    {
        std::cout << "    schema: " << schema.diagnostics().entries()[i].message << '\n';
    }
    EXPECT_EQ(failures, 0u);
    EXPECT_EQ(raw_values, 0u);
    EXPECT_EQ(raw_arrays, 0u);
}

TEST(MirrorsEdgeReflectionTests, FullCorpusReflection)
{
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    ReflectionTally tally;
    for (const auto& file : packages(true))
    {
        Linker linker;
        linker.add_search_path(fixture_dir());
        check_package(linker, linker.load(file), tally);
    }
    report(tally);
    EXPECT_EQ(tally.failures, 0u);
    EXPECT_EQ(tally.script_failures, 0u);
}
