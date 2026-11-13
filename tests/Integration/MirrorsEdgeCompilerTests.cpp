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
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Compiler/Compiler.hpp>
#include <UnrealLib/Decompiler/Decompiler.hpp>
#include <UnrealLib/Decompiler/NativeTable.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
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
        if (path != wanted.substr(dot + 1) && path.rfind("Default__" + wanted.substr(dot + 1), 0) != 0) continue;
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
        for (const auto& [klass, prop] : c.interfaces) std::cout << "    implements " << n(klass) << " property=" << n(prop) << std::endl;
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
    const char* source = R"(class MirageBouncePad extends Trigger
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
    EXPECT_EQ(p.object_path(f.field.super), "Engine.Trigger");
    EXPECT_EQ(f.class_data->component_defaults.size(), 2u);   // Trigger's (its collision cylinder, its sprite)
    EXPECT_EQ(f.state->function_map.size(), 2u);

    Decompiler::ScriptDecompiler decompiler(check, Decompiler::NativeTable::from_packages(check, check_scripts));
    ReflectionSchema schema(check);
    schema.add_package(p);
    for (const Package* sp : check_scripts) schema.add_package(*sp);
    decompiler.set_schema(&schema);
    const auto text = decompiler.decompile_class(ObjectRef{&p, *cls});
    std::cout << text.source;
    EXPECT_TRUE(text.clean());
    EXPECT_NE(text.source.find("P.Velocity.Z = Strength"), std::string::npos);
    EXPECT_NE(text.source.find("Strength=1200"), std::string::npos);
    // Its own copies of Trigger's components, the inherited references pointing at them
    EXPECT_NE(text.source.find("Begin Object Class=CylinderComponent Name=CollisionCylinder"), std::string::npos);
    EXPECT_NE(text.source.find("Components(1)=CollisionCylinder"), std::string::npos);
    EXPECT_NE(text.source.find("CollisionComponent=CollisionCylinder"), std::string::npos);
}

// UNREALLIB_DUMP_FIELDS=<Package.Class>: every field under a class (states, structs, enums, consts, functions,
// properties; recursively), with its export entry and reflection data
TEST(MirrorsEdgeCompilerTests, DumpFields)
{
    const std::string wanted = env("UNREALLIB_DUMP_FIELDS");
    if (!fixtures_available() || wanted.empty())
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_DUMP_FIELDS";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    const auto dot = wanted.find('.');
    const Package& p = linker.load(*linker.find_package_file(wanted.substr(0, dot)));
    const auto cls = p.find_export(wanted.substr(dot + 1));
    ASSERT_TRUE(cls.has_value());
    const auto n = [&](ObjectIndex x) { return x.is_null() ? std::string("-") : p.object_path(x); };
    const std::function<void(ObjectIndex, int)> dump = [&](ObjectIndex idx, int depth) {
        const std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
        const ExportEntry& e = p.exports()[idx.export_index()];
        const FieldObject f = linker.read_field(ObjectRef{&p, idx});
        std::cout << fmt::format("{}{} {}: super_index={} archetype={} flags=0x{:016X} | super={} next={}", pad,
                                 p.class_name(idx), p.object_name(idx), n(e.super_index), n(e.archetype), e.object_flags,
                                 n(f.field.super), n(f.field.next));
        if (f.structure)
            std::cout << fmt::format(" | children={} line={} pos={} script={}", n(f.structure->children), f.structure->line,
                                     f.structure->text_pos, f.structure->script_size);
        if (f.function) std::cout << fmt::format(" | fflags=0x{:08X} native={}", f.function->flags, f.function->native_token);
        if (f.state)
        {
            std::cout << fmt::format(" | probe=0x{:X} ignore=0x{:X} label=0x{:X} sflags=0x{:X} map=", f.state->probe_mask,
                                     f.state->ignore_mask.value_or(0), f.state->label_table_offset, f.state->flags);
            for (const auto& [name, fn] : f.state->function_map)
                std::cout << p.names().try_to_string(name).value_or("?") << "=" << n(fn) << ",";
        }
        if (f.property)
            std::cout << fmt::format(" | type={} dim={} pflags=0x{:X} ref={} ref2={}", static_cast<int>(f.property->type),
                                     f.property->array_dim, f.property->flags, n(f.property->reference), n(f.property->reference2));
        if (f.kind == FieldKind::Enum) std::cout << " | names=" << f.enum_names.size();
        if (f.const_value) std::cout << " | value=" << f.const_value->text;
        if (f.script_struct)
            std::cout << fmt::format(" | struct_flags=0x{:X} defaults={}", f.script_struct->flags, f.script_struct->defaults.size());
        std::cout << " | bytes:";
        const auto data = p.export_data(idx.export_index());
        for (std::size_t b = 0; b < std::min<std::size_t>(data.size(), 64); ++b) std::cout << fmt::format(" {:02X}", static_cast<unsigned>(data[b]));
        std::cout << '\n';
        if (f.structure && depth < 3)
        {
            int guard = 0;
            for (ObjectIndex c = f.structure->children; !c.is_null() && guard++ < 400;)
            {
                dump(c, depth + 1);
                c = linker.read_field(ObjectRef{&p, c}).field.next;
            }
        }
    };
    dump(*cls, 0);
}

// The class package builder with everything a class can declare here: consts, enums, structs with defaults, states
// (auto, ignores, state functions, state code with labels and latent calls, a state extending another)
TEST(MirrorsEdgeCompilerTests, BuildClassPackageStates)
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
    const char* source = R"(class MirageSpinner extends Actor
    placeable;

const SPIN_RATE = 4096;

enum ESpinMode
{
    SPIN_Idle,
    SPIN_Slow,
    SPIN_Fast
};

struct SpinStep
{
    var() float Duration;
    var int Rate;
    var ESpinMode Mode;

    structdefaultproperties
    {
        Duration=0.5
        Rate=1024
    }
};

var() array<SpinStep> Steps;
var ESpinMode Mode;
var int Spins;

function int RateFor(ESpinMode M)
{
    if (M == SPIN_Fast)
    {
        return SPIN_RATE;
    }
    return SPIN_RATE / 4;
}

auto state Idle
{
    ignores Bump;

    event Touch(Actor Other, PrimitiveComponent OtherComp, Vector HitLocation, Vector HitNormal)
    {
        GotoState('Spinning');
    }

Begin:
    Mode = SPIN_Idle;
}

state Spinning
{
    function BeginState(name PreviousStateName)
    {
        Mode = SPIN_Fast;
    }

Begin:
    Spins++;
    Sleep(0.5);
    if (Spins < 3)
    {
        goto 'Begin';
    }
    GotoState('Idle');
}

state SpinningSlowly extends Spinning
{
    function BeginState(name PreviousStateName)
    {
        Mode = SPIN_Slow;
    }
}

defaultproperties
{
    Mode=SPIN_Idle
}
)";
    const Compiler::BuiltPackage built = compiler.build_class_package("MirageTestSpinner", source);
    for (const auto& e : built.errors) std::cout << "  error: " << e << '\n';
    ASSERT_TRUE(built.ok());

    Linker check;
    check.add_search_path(fixture_dir());
    const auto check_scripts = load_script_packages(check);
    Package loaded;
    loaded.load_from_memory(built.bytes, "MirageTestSpinner");
    const Package& p = check.add(std::move(loaded));
    const auto cls = p.find_export("MirageSpinner");
    ASSERT_TRUE(cls.has_value());

    const auto field = [&](std::string_view path) {
        const auto i = p.find_export(path);
        EXPECT_TRUE(i.has_value()) << path;
        return i ? check.read_field(ObjectRef{&p, *i}) : FieldObject{};
    };
    const FieldObject idle = field("MirageSpinner.Idle");
    ASSERT_TRUE(idle.state.has_value());
    EXPECT_NE(idle.state->flags & StateFlag::Auto, 0u);
    EXPECT_NE(idle.state->probe_mask & (1ull << 13), 0u);                     // Touch
    EXPECT_EQ(idle.state->ignore_mask.value_or(0) & (1ull << 15), 0u);       // ignores Bump
    const FieldObject spinning = field("MirageSpinner.Spinning");
    ASSERT_TRUE(spinning.state.has_value());
    EXPECT_NE(spinning.state->probe_mask & (1ull << 16), 0u);                // BeginState
    EXPECT_NE(spinning.state->label_table_offset, 0xFFFF);
    const FieldObject slowly = field("MirageSpinner.SpinningSlowly");
    EXPECT_EQ(p.object_path(slowly.field.super), "MirageSpinner.Spinning");
    const FieldObject mode = field("MirageSpinner.ESpinMode");
    EXPECT_EQ(mode.enum_names.size(), 4u);                                   // SPIN_MAX added
    const FieldObject step = field("MirageSpinner.SpinStep");
    ASSERT_TRUE(step.script_struct.has_value());
    EXPECT_EQ(step.script_struct->defaults.size(), 2u);
    const FieldObject k = field("MirageSpinner.SPIN_RATE");
    ASSERT_TRUE(k.const_value.has_value());
    EXPECT_EQ(k.const_value->text, "4096");

    Decompiler::ScriptDecompiler decompiler(check, Decompiler::NativeTable::from_packages(check, check_scripts));
    const auto text = decompiler.decompile_class(ObjectRef{&p, *cls});
    std::cout << text.source;
    EXPECT_TRUE(text.clean());
    EXPECT_NE(text.source.find("Sleep(0.5)"), std::string::npos);
    EXPECT_NE(text.source.find("goto 'Begin'"), std::string::npos);
    EXPECT_NE(text.source.find("return 4096"), std::string::npos);            // The const, inlined
}

// UNREALLIB_BUILD_CLASS=<file.uc>: builds the class and prints the errors, or the class decompiled back
TEST(MirrorsEdgeCompilerTests, BuildClassFile)
{
    const std::string file = env("UNREALLIB_BUILD_CLASS");
    if (!fixtures_available() || file.empty())
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_BUILD_CLASS";
    }
    std::ifstream in(file, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Linker linker;
    linker.add_search_path(fixture_dir());
    const auto scripts = load_script_packages(linker);
    Compiler::ScriptCompiler compiler(linker, scripts);
    const Compiler::BuiltPackage built = compiler.build_class_package("MirageTestFile", source);
    for (const auto& e : built.errors) std::cout << "  error: " << e << '\n';
    ASSERT_TRUE(built.ok());
    Linker check;
    check.add_search_path(fixture_dir());
    const auto check_scripts = load_script_packages(check);
    Package loaded;
    loaded.load_from_memory(built.bytes, "MirageTestFile");
    const Package& p = check.add(std::move(loaded));
    Decompiler::ScriptDecompiler decompiler(check, Decompiler::NativeTable::from_packages(check, check_scripts));
    ReflectionSchema schema(check);
    schema.add_package(p);
    for (const Package* sp : check_scripts) schema.add_package(*sp);
    decompiler.set_schema(&schema);
    if (const std::string probe = env("UNREALLIB_SCHEMA_PROBE"); !probe.empty())
    {
        const auto dot = probe.find('.');
        const auto t = schema.find_property(probe.substr(0, dot), probe.substr(dot + 1));
        std::cout << "  schema " << probe << ": " << (t ? static_cast<int>(t->type) : -1) << " inner "
                  << (t && t->inner ? static_cast<int>(t->inner->type) : -1) << std::endl;
    }
    for (std::size_t i = 0; i < p.exports().size(); ++i)
        if (p.class_name(ObjectIndex::from_export(i)) == "Class")
            std::cout << decompiler.decompile_class(ObjectRef{&p, ObjectIndex::from_export(i)}).source;
}

// UNREALLIB_CLASS_ROUNDTRIP=1 (slow; =<Package> for one package): every class of the script packages decompiled,
// built again with the class package builder, decompiled again, and the two sources compared. Reported: how many
// build, how many come back identical, the build errors and the first differing line (grouped, an example each).
TEST(MirrorsEdgeCompilerTests, ClassRoundTrip)
{
    const std::string wanted = env("UNREALLIB_CLASS_ROUNDTRIP");
    if (!fixtures_available() || wanted.empty())
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_CLASS_ROUNDTRIP";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    const auto scripts = load_script_packages(linker);
    const Decompiler::NativeTable natives = Decompiler::NativeTable::from_packages(linker, scripts);
    Decompiler::ScriptDecompiler decompiler(linker, natives);
    ReflectionSchema schema(linker);
    for (const Package* p : scripts) schema.add_package(*p);
    decompiler.set_schema(&schema);
    Compiler::ScriptCompiler compiler(linker, scripts);

    std::size_t total = 0, built = 0, identical = 0, n = 0;
    std::map<std::string, std::pair<std::size_t, std::string>> errors, differences;
    const auto tally = [](auto& m, const std::string& k, const std::string& example) {
        auto& e = m[k];
        if (e.first++ == 0) e.second = example;
    };
    // The class header's modifiers as a set, without the ones a built class can't have (native C++ classes)
    const auto normalized = [](const std::string& source) {
        const auto end = source.find(";\n");
        if (end == std::string::npos) return source;
        std::istringstream header(source.substr(0, end));
        std::vector<std::string> words;
        for (std::string w; header >> w;)
            if (w != "native" && w.rfind("native(", 0) != 0 && w != "nativereplication" && w != "noexport") words.push_back(w);
        std::sort(words.begin() + std::min<std::size_t>(words.size(), 4), words.end());
        std::string out;
        for (const std::string& w : words) out += w + " ";
        std::string rest = source.substr(end);
        // Native structs: C++ ones (a built struct is a script struct)
        for (std::size_t at = rest.find("struct native "); at != std::string::npos; at = rest.find("struct native ", at))
            rest.erase(at + 6, 7);
        return out + rest;
    };
    const auto lines = [](const std::string& s) {
        std::vector<std::string> out;
        std::size_t at = 0;
        while (at <= s.size())
        {
            const auto end = s.find('\n', at);
            out.push_back(s.substr(at, end == std::string::npos ? std::string::npos : end - at));
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return out;
    };
    for (const Package* package : scripts)
    {
        if (wanted != "1" && package->name() != wanted) continue;
        for (std::size_t i = 0; i < package->exports().size(); ++i)
        {
            const ObjectIndex idx = ObjectIndex::from_export(i);
            if (package->class_name(idx) != "Class") continue;
            const std::string path = package->name() + "." + package->object_name(idx);
            ++total;
            std::string original;
            try
            {
                original = decompiler.decompile_class(ObjectRef{package, idx}).source;
            }
            catch (const std::exception& e)
            {
                tally(errors, "decompile: " + std::string(e.what()).substr(0, 60), path);
                continue;
            }
            const auto result = compiler.build_class_package(fmt::format("MirageRT{}", n++), original);
            if (!result.ok())
            {
                std::string k = result.errors.empty() ? "?" : result.errors.front();
                // Group by the message without its line number and names in quotes
                if (const auto c = k.find(": "); k.rfind("line ", 0) == 0 && c != std::string::npos) k = k.substr(c + 2);
                if (const auto q = k.find('\''); q != std::string::npos) k = k.substr(0, q);
                tally(errors, k.substr(0, 90), path + ": " + (result.errors.empty() ? "" : result.errors.front()));
                continue;
            }
            ++built;
            std::string again;
            try
            {
                // The rebuilt package, read through the first linker (it knows every script package); its own schema
                // entries first, so its classes' defaults decode against them
                Package copy;
                copy.load_from_memory(result.bytes, fmt::format("MirageRTCopy{}", n));
                const Package& cp = linker.add(std::move(copy));
                ReflectionSchema schema2(linker);
                schema2.add_package(cp);
                for (const Package* sp : scripts) schema2.add_package(*sp);
                Decompiler::ScriptDecompiler d2(linker, natives);
                d2.set_schema(&schema2);
                again = d2.decompile_class(ObjectRef{&cp, *cp.find_export(package->object_name(idx))}).source;
            }
            catch (const std::exception& e)
            {
                tally(errors, "decompile again: " + std::string(e.what()).substr(0, 60), path);
                continue;
            }
            if (normalized(again) == normalized(original))
            {
                ++identical;
                continue;
            }
            const auto a = lines(normalized(original)), b = lines(normalized(again));
            std::size_t l = 0;
            while (l < a.size() && l < b.size() && a[l] == b[l]) ++l;
            const std::string was = l < a.size() ? a[l] : "<end>", now = l < b.size() ? b[l] : "<end>";
            std::string key = was;
            key.erase(0, key.find_first_not_of(' '));
            tally(differences, key.substr(0, key.find_first_of("(=;") == std::string::npos ? 40 : std::min<std::size_t>(40, key.find_first_of("(=;"))),
                  path + ": [" + was + "] -> [" + now + "]");
        }
    }
    std::cout << fmt::format("  classes {}, built {}, identical {} ({:.1f}%)\n", total, built, identical,
                             total ? 100.0 * static_cast<double>(identical) / static_cast<double>(total) : 0.0);
    const auto print = [](const char* title, const auto& m) {
        std::vector<std::pair<std::size_t, std::string>> sorted;
        for (const auto& [k, v] : m) sorted.emplace_back(v.first, k + "\n        e.g. " + v.second);
        std::sort(sorted.rbegin(), sorted.rend());
        std::cout << "  " << title << ":\n";
        for (std::size_t i = 0; i < sorted.size() && i < 25; ++i) std::cout << fmt::format("    {:5} {}\n", sorted[i].first, sorted[i].second);
    };
    print("build errors", errors);
    print("differences", differences);
}
