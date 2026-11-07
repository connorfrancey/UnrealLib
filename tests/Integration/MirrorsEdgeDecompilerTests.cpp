// Decompiler integration tests against real Mirror's Edge packages.
// Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set.
//
// Every function and state of the script packages (*.u) is decompiled. Checked:
//   * nothing throws;
//   * every native call is found in the NativeTable built from the game's own functions;
//   * no placeholders (unknown casts, unexpected tokens) are printed;
//   * reported: unstructured jumps (gotos), structure problems and unresolved game-specific tokens.
//
// UNREALLIB_DECOMPILE_DUMP=<directory> also writes every decompiled script to
// <directory>/<Package>/<Outer>.<Name>.uc for manual review (game-derived output: never commit it).

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Decompiler/Decompiler.hpp>
#include <UnrealLib/Decompiler/NativeTable.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>

#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
using namespace UnrealLib::Decompiler;
using namespace UnrealLib::TestSupport;
namespace fs = std::filesystem;

namespace
{
    struct DecompileTally
    {
        std::size_t functions = 0;
        std::size_t states = 0;
        std::size_t clean = 0;
        std::size_t failures = 0;
        std::size_t unknown_natives = 0;
        std::size_t placeholders = 0;
        std::size_t gotos = 0;
        std::size_t problems = 0;
        std::size_t unresolved = 0;
        std::size_t scripts_with_gotos = 0;
        std::map<std::string, std::size_t> messages;
    };

    /// UNREALLIB_DECOMPILE_TRACE=<function or state name>: prints its top-level statements (memory offset, token
    /// kind, jump/case target, size) to debug the structuring.
    void print_statements(Linker& linker, ObjectRef ref)
    {
        const Package& package = *ref.package;
        const FieldObject field = linker.read_field(ref);
        if (!field.structure || field.structure->script_size <= 0)
        {
            return;
        }
        const TokenMap map = TokenMap::ue3(package.summary().file_version, &package.profile());
        BytecodeOptions options;
        options.token_map = &map;
        options.name_count = static_cast<std::int32_t>(package.names().size());
        options.none_index = package.names().find("None").value_or(-1);
        BinaryReader reader(package.export_data(ref.index.export_index()), package.context());
        reader.seek(field.structure->script_offset);
        const Script script = decode_script(reader, static_cast<std::size_t>(field.structure->script_size), options);
        std::cout << "  trace " << linker.qualified_path(ref) << " (memory size 0x" << std::hex << script.memory_size
                  << ")\n";
        const std::function<void(std::uint32_t, int)> print = [&](std::uint32_t index, int depth) {
            const Token& t = script.tokens[index];
            std::cout << std::string(4 + depth * 2, ' ') << "0x" << std::hex << t.memory_offset << " "
                      << to_string(t.kind) << " op=0x" << static_cast<int>(t.opcode) << " word=0x" << t.word
                      << " size=0x" << t.size << std::dec << '\n';
            for (const std::uint32_t child : t.children) print(child, depth + 1);
        };
        for (const std::uint32_t s : script.statements) print(s, 0);
        std::cout << "  all tokens (including terminators that are not children):\n";
        for (const Token& t : script.tokens)
        {
            std::cout << "    0x" << std::hex << t.memory_offset << " " << to_string(t.kind) << " op=0x"
                      << static_cast<int>(t.opcode) << " size=0x" << t.size << std::dec << '\n';
        }
    }

    void dump(const fs::path& root, const Package& package, ObjectRef ref, const std::string& source)
    {
        const ObjectIndex outer = package.outer(ref.index);
        const std::string name =
            (outer.is_null() ? std::string() : package.object_name(outer) + ".") + package.object_name(ref.index);
        const fs::path directory = root / package.name();
        fs::create_directories(directory);
        std::ofstream(directory / (name + ".uc"), std::ios::binary) << source;
    }
}

TEST(MirrorsEdgeDecompilerTests, ScriptPackageFunctionsAndStates)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    const auto scripts = load_script_packages(linker);
    ASSERT_FALSE(scripts.empty());

    Diagnostics table_diagnostics;
    NativeTable natives = NativeTable::from_packages(linker, scripts, &table_diagnostics);
    std::cout << "  native table: " << natives.size() << " functions, " << table_diagnostics.entries().size()
              << " unreadable\n";
    EXPECT_GT(natives.size(), 100u);

    const std::string dump_dir = env("UNREALLIB_DECOMPILE_DUMP");
    const std::string trace = env("UNREALLIB_DECOMPILE_TRACE");
    ScriptDecompiler decompiler(linker, std::move(natives));
    DecompileTally tally;
    for (const Package* package : scripts)
    {
        for (std::size_t i = 0; i < package->exports().size(); ++i)
        {
            const ObjectRef ref{package, ObjectIndex::from_export(i)};
            const std::string class_name = package->class_name(ref.index);
            if ((class_name != "Function" && class_name != "State") || package->exports()[i].serial_size <= 0)
            {
                continue;
            }
            ++(class_name == "Function" ? tally.functions : tally.states);
            try
            {
                if (!trace.empty() && package->object_name(ref.index) == trace)
                {
                    print_statements(linker, ref);
                }
                const DecompiledScript result = decompiler.decompile(ref);
                tally.unknown_natives += result.unknown_natives;
                tally.placeholders += result.placeholders;
                tally.gotos += result.structure.gotos;
                tally.problems += result.structure.problems;
                tally.unresolved += result.structure.unresolved;
                tally.scripts_with_gotos += result.structure.gotos != 0 ? 1 : 0;
                tally.clean += result.clean() ? 1 : 0;
                for (const auto& d : result.diagnostics.entries())
                {
                    if (d.severity != Severity::Info)
                    {
                        ++tally.messages[d.code + ": " + d.message.substr(d.message.find(": ") + 2)];
                    }
                }
                if (!dump_dir.empty())
                {
                    dump(dump_dir, *package, ref, result.source);
                }
            }
            catch (const Error& error)
            {
                if (tally.failures++ < 10)
                {
                    ADD_FAILURE() << linker.qualified_path(ref) << ": " << error.what();
                }
            }
        }
    }

    std::cout << "  functions: " << tally.functions << ", states: " << tally.states << ", clean: " << tally.clean
              << ", failures: " << tally.failures << '\n'
              << "  unknown natives: " << tally.unknown_natives << ", placeholders: " << tally.placeholders
              << ", unresolved tokens: " << tally.unresolved << '\n'
              << "  gotos: " << tally.gotos << " (in " << tally.scripts_with_gotos
              << " scripts), structure problems: " << tally.problems << '\n';
    std::vector<std::pair<std::size_t, std::string>> top;
    for (const auto& [message, count] : tally.messages) top.emplace_back(count, message);
    std::sort(top.rbegin(), top.rend());
    for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 20); ++i)
    {
        std::cout << "    " << top[i].first << "x " << top[i].second << '\n';
    }
    ::testing::Test::RecordProperty("scripts", static_cast<int>(tally.functions + tally.states));
    ::testing::Test::RecordProperty("clean", static_cast<int>(tally.clean));
    ::testing::Test::RecordProperty("gotos", static_cast<int>(tally.gotos));

    EXPECT_EQ(tally.failures, 0u);
    EXPECT_EQ(tally.unknown_natives, 0u);
    EXPECT_EQ(tally.placeholders, 0u);
}

TEST(MirrorsEdgeDecompilerTests, ScriptPackageClasses)
{
    // Every class of the script packages decompiled to a whole .uc file (declaration, members, functions, states,
    // defaultproperties). Checked: nothing throws, no unknown natives, no placeholders (raw default values
    // included), and the output names the class.
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    const auto scripts = load_script_packages(linker);
    ASSERT_FALSE(scripts.empty());
    ReflectionSchema schema(linker);
    for (const Package* package : scripts) schema.add_package(*package);

    ScriptDecompiler decompiler(linker, NativeTable::from_packages(linker, scripts));
    decompiler.set_schema(&schema);
    const std::string dump_dir = env("UNREALLIB_DECOMPILE_DUMP");
    std::size_t classes = 0, failures = 0, unknown_natives = 0, placeholders = 0, lines = 0;
    std::map<std::string, std::size_t> messages;
    for (const Package* package : scripts)
    {
        for (std::size_t i = 0; i < package->exports().size(); ++i)
        {
            const ObjectRef ref{package, ObjectIndex::from_export(i)};
            if (package->class_name(ref.index) != "Class" || package->exports()[i].serial_size <= 0)
            {
                continue;
            }
            ++classes;
            try
            {
                const DecompiledScript result = decompiler.decompile_class(ref);
                unknown_natives += result.unknown_natives;
                placeholders += result.placeholders;
                lines += static_cast<std::size_t>(std::count(result.source.begin(), result.source.end(), '\n'));
                EXPECT_NE(result.source.find(" " + package->object_name(ref.index)), std::string::npos);
                for (const auto& d : result.diagnostics.entries())
                {
                    if (d.severity != Severity::Info && messages[d.code]++ < 5)
                    {
                        std::cout << "    " << d.code << ": " << d.message << '\n';
                    }
                }
                if (!dump_dir.empty())
                {
                    const fs::path directory = fs::path(dump_dir) / "Classes" / package->name();
                    fs::create_directories(directory);
                    std::ofstream(directory / (package->object_name(ref.index) + ".uc"), std::ios::binary)
                        << result.source;
                }
            }
            catch (const Error& error)
            {
                if (failures++ < 10)
                {
                    ADD_FAILURE() << linker.qualified_path(ref) << ": " << error.what();
                }
            }
        }
    }
    std::cout << "  classes: " << classes << ", lines: " << lines << ", failures: " << failures
              << ", unknown natives: " << unknown_natives << ", placeholders: " << placeholders << '\n';
    for (const auto& [code, count] : messages) std::cout << "    " << count << "x " << code << '\n';
    ::testing::Test::RecordProperty("classes", static_cast<int>(classes));
    EXPECT_GT(classes, 1000u);
    EXPECT_EQ(failures, 0u);
    EXPECT_EQ(unknown_natives, 0u);
    EXPECT_EQ(placeholders, 0u);
}
