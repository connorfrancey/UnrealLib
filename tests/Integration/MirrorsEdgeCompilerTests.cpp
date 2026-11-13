// Compiler round trip against real Mirror's Edge packages. Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set.
//
// Every function and state of the script packages: its bytecode is decompiled to source, the source compiled back,
// assembled in the package's own format and compared with the original bytes. Reported: how many compile, how many
// come out byte-identical, and for the others the first token that differs, grouped (with an example each).
//
// UNREALLIB_COMPILE_TRACE=<function name> prints the source and both token trees of the scripts of that name.

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Compiler/Compiler.hpp>
#include <UnrealLib/Decompiler/Decompiler.hpp>
#include <UnrealLib/Decompiler/NativeTable.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Reflection/Fields.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
using namespace UnrealLib::TestSupport;

namespace
{
    std::string describe(const Package& p, const Token& t)
    {
        std::string s = std::string(to_string(t.kind));
        if (t.kind == TokenKind::NativeFunction) s += fmt::format(" native={}", t.native_index);
        if (!t.object.is_null()) s += " obj=" + (p.is_valid(t.object) ? p.object_name(t.object) : std::to_string(t.object.value));
        if (!t.object2.is_null()) s += " obj2=" + (p.is_valid(t.object2) ? p.object_name(t.object2) : std::to_string(t.object2.value));
        if (t.name.index != 0 || t.name.number != 0) s += " name=" + p.names().try_to_string(t.name).value_or("?");
        if (t.word) s += fmt::format(" word={}", t.word);
        if (t.word2) s += fmt::format(" word2={}", t.word2);
        if (t.byte_value) s += fmt::format(" byte={}", t.byte_value);
        if (t.byte_value2) s += fmt::format(" byte2={}", t.byte_value2);
        if (t.int_value) s += fmt::format(" int={}", t.int_value);
        s += fmt::format(" children={}", t.children.size());
        return s;
    }

    // The tokens in stream order (pre-order over statements and children)
    std::vector<const Token*> flatten(const Script& s)
    {
        std::vector<const Token*> out;
        const std::function<void(std::uint32_t, int)> walk = [&](std::uint32_t i, int depth) {
            if (i >= s.tokens.size() || depth > 200) return;
            out.push_back(&s.tokens[i]);
            for (const auto c : s.tokens[i].children) walk(c, depth + 1);
        };
        for (const auto st : s.statements) walk(st, 0);
        return out;
    }

    // Fields that the compiler decides (jump targets and relative skips are the assembler's)
    bool same(const Token& a, const Token& b)
    {
        if (a.kind != b.kind || a.children.size() != b.children.size()) return false;
        if (a.kind == TokenKind::NativeFunction && a.native_index != b.native_index) return false;
        if (a.object.value != b.object.value || a.object2.value != b.object2.value) return false;
        if (a.name.index != b.name.index || a.name.number != b.name.number) return false;
        if (a.int_value != b.int_value || a.byte_value != b.byte_value || a.byte_value2 != b.byte_value2) return false;
        if (a.floats != b.floats || a.ints != b.ints || a.string_value != b.string_value) return false;
        const bool relative = a.kind == TokenKind::Jump || a.kind == TokenKind::JumpIfNot || a.kind == TokenKind::Case ||
                              a.kind == TokenKind::Iterator || a.kind == TokenKind::DynamicArrayIterator ||
                              a.kind == TokenKind::Context || a.kind == TokenKind::ClassContext || a.kind == TokenKind::Skip ||
                              a.kind == TokenKind::Conditional || a.kind == TokenKind::FilterEditorOnly ||
                              a.kind == TokenKind::DynamicArrayFind || a.kind == TokenKind::DynamicArrayFindStruct ||
                              a.kind == TokenKind::DefaultParameter || a.kind == TokenKind::Assert ||
                              a.kind == TokenKind::DynamicArrayAddItem || a.kind == TokenKind::DynamicArrayRemoveItem ||
                              a.kind == TokenKind::DynamicArrayInsertItem || a.kind == TokenKind::DynamicArrayInsert ||
                              a.kind == TokenKind::DynamicArrayRemove || a.kind == TokenKind::DynamicArraySort;
        if (!relative && a.word != b.word) return false;
        if (a.kind != TokenKind::Conditional && a.word2 != b.word2) return false;
        return true;
    }
}

TEST(MirrorsEdgeCompilerTests, DecompileCompileRoundTrip)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    const auto scripts = load_script_packages(linker);
    ASSERT_FALSE(scripts.empty());
    Decompiler::NativeTable natives = Decompiler::NativeTable::from_packages(linker, scripts);
    Decompiler::ScriptDecompiler decompiler(linker, std::move(natives));
    Compiler::ScriptCompiler compiler(linker, scripts);
    const std::string trace = env("UNREALLIB_COMPILE_TRACE");

    std::size_t total = 0, compiled = 0, identical = 0, unclean = 0;
    std::map<std::string, std::pair<std::size_t, std::size_t>> per_package;    // identical, total
    std::map<std::string, std::pair<std::size_t, std::string>> errors, differences;
    const auto tally = [](std::map<std::string, std::pair<std::size_t, std::string>>& m, const std::string& k, const std::string& example) {
        auto& e = m[k];
        if (e.first++ == 0) e.second = example;
    };

    for (const Package* package : scripts)
    {
        const TokenMap map = TokenMap::ue3(package->summary().file_version, &package->profile());
        BytecodeOptions options;
        options.token_map = &map;
        options.name_count = static_cast<std::int32_t>(package->names().size());
        options.none_index = package->names().find("None").value_or(-1);
        for (std::size_t i = 0; i < package->exports().size(); ++i)
        {
            const ObjectRef ref{package, ObjectIndex::from_export(i)};
            const std::string class_name = package->class_name(ref.index);
            if ((class_name != "Function" && class_name != "State") || package->exports()[i].serial_size <= 0) continue;
            FieldObject field;
            try
            {
                field = linker.read_field(ref);
            }
            catch (const std::exception&)
            {
                continue;
            }
            if (!field.structure || field.structure->script_size <= 0) continue;
            const auto data = package->export_data(ref.index.export_index());
            const std::size_t offset = field.structure->script_offset;
            const std::size_t storage = field.structure->script_storage_size;
            BinaryReader reader(data, package->context());
            reader.seek(offset);
            Script original;
            try
            {
                original = decode_script(reader, static_cast<std::size_t>(field.structure->script_size), options);
            }
            catch (const std::exception&)
            {
                continue;
            }
            ++total;
            ++per_package[package->name()].second;
            const std::string path = linker.qualified_path(ref);
            const std::string name = package->object_name(ref.index);
            const auto source = decompiler.decompile_body(original, ref);
            if (!source.clean()) ++unclean;

            Compiler::CompiledScript result = compiler.compile_body(source.source, ref, source.default_values);
            const bool traced = !trace.empty() && ("," + trace + ",").find("," + name + ",") != std::string::npos;
            if (traced) std::cout << "  === " << path << " ===\n" << source.source;
            if (!result.ok())
            {
                std::string k = result.errors.front();
                // Group by the message without its specifics
                if (const auto q = k.find('\''); q != std::string::npos) k = k.substr(0, q);
                if (const auto colon = k.find(": "); colon != std::string::npos) k = k.substr(colon + 2);
                tally(errors, k, path + ": " + result.errors.front());
                if (traced) std::cout << "  error: " << result.errors.front() << '\n';
                continue;
            }
            ++compiled;
            std::vector<std::byte> bytes;
            try
            {
                BinaryWriter writer(package->context());
                assemble_script(writer, result.script, map);
                bytes.assign(writer.data().begin(), writer.data().end());
            }
            catch (const std::exception& e)
            {
                tally(errors, std::string("assemble: ") + e.what(), path);
                continue;
            }
            const auto want = data.subspan(offset, storage);
            if (bytes.size() == want.size() && std::equal(bytes.begin(), bytes.end(), want.begin()))
            {
                ++identical;
                ++per_package[package->name()].first;
                continue;
            }
            // The first token that differs
            const auto a = flatten(original);
            const auto b = flatten(result.script);
            std::string k = "same tokens, other bytes";
            for (std::size_t t = 0; t < std::max(a.size(), b.size()); ++t)
            {
                if (t >= a.size() || t >= b.size())
                {
                    k = t >= a.size() ? "extra tokens" : "missing tokens";
                    break;
                }
                if (!same(*a[t], *b[t]))
                {
                    k = describe(*package, *a[t]) + "  <-  " + describe(*package, *b[t]);
                    // Group by kinds and the differing field kinds, not the values
                    std::string group = std::string(to_string(a[t]->kind)) + " <- " + std::string(to_string(b[t]->kind));
                    if (a[t]->kind == b[t]->kind)
                    {
                        if (a[t]->children.size() != b[t]->children.size()) group += " (children)";
                        else if (a[t]->object.value != b[t]->object.value) group += " (object)";
                        else if (a[t]->object2.value != b[t]->object2.value) group += " (object2)";
                        else if (a[t]->word2 != b[t]->word2) group += " (word2)";
                        else if (a[t]->word != b[t]->word) group += " (word)";
                        else if (a[t]->byte_value != b[t]->byte_value || a[t]->byte_value2 != b[t]->byte_value2) group += " (byte)";
                        else if (a[t]->native_index != b[t]->native_index) group += " (native)";
                        else group += " (value)";
                    }
                    tally(differences, group, path + ": " + k);
                    k.clear();
                    break;
                }
            }
            if (k == "same tokens, other bytes")
            {
                // Which relative size or jump differs: decode what was compiled and compare the words
                try
                {
                    BinaryReader back(std::span<const std::byte>(bytes.data(), bytes.size()), package->context());
                    const Script again = decode_script(back, original.memory_size, options);
                    const auto c = flatten(again);
                    for (std::size_t t = 0; t < std::min(a.size(), c.size()); ++t)
                        if (a[t]->word != c[t]->word || a[t]->word2 != c[t]->word2)
                        {
                            k = fmt::format("relative word of {}", to_string(a[t]->kind));
                            std::string detail;
                            if ((a[t]->kind == TokenKind::Context || a[t]->kind == TokenKind::ClassContext) && a[t]->children.size() == 2)
                            {
                                const Token& m = original.tokens[a[t]->children[1]];
                                std::string kids;
                                for (const auto ch : m.children) kids += std::string(to_string(original.tokens[ch].kind)) + " ";
                                const Token& o = original.tokens[a[t]->children[0]];
                                const std::string object = describe(*package, o);
                                detail = fmt::format(" object {} member {} size {} [{}]", object, to_string(m.kind), m.size, kids);
                                k += fmt::format(" (stored {} member size {}, object {})", a[t]->word, a[t]->word == m.size ? "=" : (a[t]->word < m.size ? ">" : "<"),
                                                 object.substr(0, object.find(" children")));
                            }
                            tally(differences, k, fmt::format("{}: original {}/{} compiled {}/{}{}", path, a[t]->word, a[t]->word2,
                                                              c[t]->word, c[t]->word2, detail));
                            k.clear();
                            break;
                        }
                }
                catch (const std::exception& e)
                {
                    k = std::string("compiled bytes don't decode: ") + e.what();
                }
            }
            if (!k.empty()) tally(differences, k, path);
            if (traced)
            {
                std::cout << "  original / compiled tokens:\n";
                for (std::size_t t = 0; t < std::max(a.size(), b.size()); ++t)
                    std::cout << "    " << (t < a.size() ? describe(*package, *a[t]) : "-") << "  |  "
                              << (t < b.size() ? describe(*package, *b[t]) : "-") << '\n';
            }
        }
    }

    const auto print = [](const char* title, const std::map<std::string, std::pair<std::size_t, std::string>>& m) {
        std::vector<std::pair<std::size_t, std::string>> sorted;
        for (const auto& [k, v] : m) sorted.emplace_back(v.first, k);
        std::sort(sorted.rbegin(), sorted.rend());
        std::cout << "  " << title << ":\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 40); ++i)
            std::cout << fmt::format("    {:6} {}\n           e.g. {}\n", sorted[i].first, sorted[i].second, m.at(sorted[i].second).second);
    };
    std::cout << fmt::format("  scripts {}, compiled {}, byte-identical {} ({:.1f}%), decompiled with placeholders {}\n", total,
                             compiled, identical, total ? 100.0 * identical / total : 0.0, unclean);
    for (const auto& [name, counts] : per_package)
        std::cout << fmt::format("    {:28} {:6} of {:6} byte-identical ({:.1f}%)\n", name, counts.first, counts.second,
                                 counts.second ? 100.0 * counts.first / counts.second : 0.0);
    print("compile errors", errors);
    print("first differences (original <- compiled)", differences);
    EXPECT_GT(total, 10000u);
    // Regression floors (measured 2026-11-12: 99.9% compile, 97.1% byte-identical)
    EXPECT_GE(compiled * 1000, total * 995);
    EXPECT_GE(identical * 1000, total * 965);
}

// write_field() is read_field()'s inverse: every reflection export of the script packages (classes, functions,
// properties, states, structs, enums, consts) read and written again gives its own bytes back.
TEST(MirrorsEdgeCompilerTests, FieldWriterRoundTrip)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    const auto scripts = load_script_packages(linker);
    std::size_t fields = 0, identical = 0;
    std::map<std::string, std::pair<std::size_t, std::string>> different;
    for (const Package* package : scripts)
    {
        FieldWriteOptions options;
        options.none = FName{package->names().find("None").value_or(0), 0};
        for (std::size_t i = 0; i < package->exports().size(); ++i)
        {
            const ObjectRef ref{package, ObjectIndex::from_export(i)};
            PropertyType type = PropertyType::Unknown;
            if (!field_kind_of(package->class_name(ref.index), &type) || package->exports()[i].serial_size <= 0) continue;
            FieldObject field;
            try
            {
                field = linker.read_field(ref);
            }
            catch (const std::exception&)
            {
                continue;
            }
            const auto input = linker.header_input(ref);
            if (!input) continue;
            ++fields;
            const auto data = package->export_data(ref.index.export_index());
            std::span<const std::byte> script;
            if (field.structure) script = data.subspan(field.structure->script_offset, field.structure->script_storage_size);
            BinaryWriter writer(package->context());
            std::string problem;
            try
            {
                write_field(writer, field, script, *input, options);
            }
            catch (const std::exception& e)
            {
                problem = e.what();
            }
            const auto want = data.subspan(0, field.end_offset);
            if (problem.empty())
            {
                if (writer.data().size() == want.size() && std::equal(writer.data().begin(), writer.data().end(), want.begin()))
                {
                    ++identical;
                    continue;
                }
                std::size_t at = 0;
                while (at < std::min(writer.data().size(), want.size()) && writer.data()[at] == want[at]) ++at;
                problem = fmt::format("differs at byte {} of {} (written {})", at, want.size(), writer.data().size());
            }
            auto& d = different[std::string(to_string(field.kind)) + ": " + problem.substr(0, problem.find(" at byte"))];
            if (d.first++ == 0) d.second = linker.qualified_path(ref) + ": " + problem;
        }
    }
    std::cout << fmt::format("  reflection exports {}, written byte-identical {}\n", fields, identical);
    for (const auto& [k, v] : different) std::cout << fmt::format("    {:6} {}\n           e.g. {}\n", v.first, k, v.second);
    EXPECT_GT(fields, 50000u);
    EXPECT_EQ(identical, fields);
}

// UNREALLIB_DUMP_CLASS=<Package.Class>: a class's export entry, reflection fields and default object, for building
// class packages like the ones the game loads
TEST(MirrorsEdgeCompilerTests, DumpClass)
{
    const std::string wanted = env("UNREALLIB_DUMP_CLASS");
    if (!fixtures_available() || wanted.empty())
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_DUMP_CLASS";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    const auto dot = wanted.find('.');
    const Package& p = linker.load(*linker.find_package_file(wanted.substr(0, dot)));
    const auto entry_text = [&](const ExportEntry& e) {
        const auto name = [&](ObjectIndex i) { return i.is_null() ? std::string("-") : p.object_path(i); };
        return fmt::format("class={} super={} outer={} archetype={} object_flags=0x{:016X} export_flags=0x{:X} package_flags=0x{:X} size={}",
                           name(e.class_index), name(e.super_index), name(e.outer), name(e.archetype), e.object_flags,
                           e.export_flags, e.package_flags, e.serial_size);
    };
    for (std::size_t i = 0; i < p.exports().size(); ++i)
    {
        const ObjectIndex idx = ObjectIndex::from_export(i);
        const std::string path = p.object_path(idx);
        if (path != wanted.substr(dot + 1) && path != "Default__" + wanted.substr(dot + 1)) continue;
        std::cout << "  " << path << ": " << entry_text(p.exports()[i]) << '\n';
        const auto data = p.export_data(i);
        std::string hex;
        for (std::size_t b = 0; b < std::min<std::size_t>(data.size(), 160); ++b) hex += fmt::format("{:02X} ", static_cast<unsigned>(data[b]));
        std::cout << "    bytes: " << hex << '\n';
        if (p.class_name(idx) != "Class") continue;
        const FieldObject f = linker.read_field(ObjectRef{&p, idx});
        const auto n = [&](ObjectIndex x) { return x.is_null() ? std::string("-") : p.object_path(x); };
        std::cout << fmt::format("    super={} next={} children={} script_text={} cpp_text={} line={} pos={} script_size={}\n",
                                 n(f.field.super), n(f.field.next), n(f.structure->children), n(f.structure->script_text),
                                 n(f.structure->cpp_text), f.structure->line, f.structure->text_pos, f.structure->script_size);
        std::cout << fmt::format("    state: probe=0x{:X} label_table=0x{:X} flags=0x{:X} funcs={}\n", f.state->probe_mask,
                                 f.state->label_table_offset, f.state->flags, f.state->function_map.size());
        const ClassData& c = *f.class_data;
        std::cout << fmt::format("    class: flags=0x{:08X} within={} config={} hide={} components={} interfaces={} autoexpand={} cdo={}\n",
                                 c.flags, n(c.within), p.names().try_to_string(c.config_name).value_or("?"), c.hide_categories.size(),
                                 c.component_defaults.size(), c.interfaces.size(), c.auto_expand_categories.size(), n(c.default_object));
    }
    std::cout << fmt::format("  package: flags=0x{:X} engine={} cooker={} licensee={}\n", p.summary().package_flags,
                             p.summary().engine_version, p.summary().cooker_version, p.summary().licensee_version);
}

// The class package builder: a placeable actor with a property, an event and a function, built against the game's
// script packages, then loaded by another linker and decompiled back to source
TEST(MirrorsEdgeCompilerTests, BuildClassPackage)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    const auto scripts = load_script_packages(linker);
    ASSERT_FALSE(scripts.empty());
    Compiler::ScriptCompiler compiler(linker, scripts);
    const char* source = R"(class MirageBouncePad extends Actor
    placeable;

var() float Strength;
var int Bounces;

event Touch(Actor Other, PrimitiveComponent OtherComp, Vector HitLocation, Vector HitNormal)
{
    local Pawn P;

    P = Pawn(Other);
    if (P == None)
    {
        return;
    }
    Bounces++;
    P.Velocity.Z = Strength;
    P.SetPhysics(PHYS_Falling);
}

function int TimesBounced(optional bool bReset)
{
    local int N;

    N = Bounces;
    if (bReset)
    {
        Bounces = 0;
    }
    return N;
}

defaultproperties
{
    Strength=1200.0
    bCollideActors=true
}
)";
    const Compiler::BuiltPackage built = compiler.build_class_package("MirageTestPad", source);
    for (const auto& e : built.errors) std::cout << "  error: " << e << '\n';
    ASSERT_TRUE(built.ok());

    Linker check;
    check.add_search_path(fixture_dir());
    const auto check_scripts = load_script_packages(check);
    Package loaded;
    loaded.load_from_memory(built.bytes, "MirageTestPad");
    const Package& p = check.add(std::move(loaded));
    const auto cls = p.find_export("MirageBouncePad");
    ASSERT_TRUE(cls.has_value());
    const FieldObject f = check.read_field(ObjectRef{&p, *cls});
    ASSERT_TRUE(f.class_data.has_value());
    EXPECT_NE(f.class_data->flags & 0x200u, 0u);    // Placeable
    EXPECT_EQ(p.object_path(f.field.super), "Engine.Actor");
    EXPECT_EQ(f.state->function_map.size(), 2u);

    Decompiler::ScriptDecompiler decompiler(check, Decompiler::NativeTable::from_packages(check, check_scripts));
    const auto text = decompiler.decompile_class(ObjectRef{&p, *cls});
    std::cout << text.source;
    EXPECT_TRUE(text.clean());
    EXPECT_NE(text.source.find("P.Velocity.Z = Strength"), std::string::npos);
    EXPECT_NE(text.source.find("Strength=1200"), std::string::npos);
}
