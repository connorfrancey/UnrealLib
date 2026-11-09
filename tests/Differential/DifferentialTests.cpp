// Differential tests: UnrealLib vs. upstream UELib (C#).
//
// The CTest fixture "uelib_dump" runs tests/Differential/UELibDump (built from the pinned upstream commit) over the
// Mirror's Edge fixture directory, writing one JSON document per package in the "raw" schema (see Program.cs). These
// tests load the same packages with UnrealLib, produce the same schema, and compare the documents semantically
// (nlohmann::json equality is order-insensitive for object keys).
//
// Upstream cannot decompress fully compressed packages: for those only the summary is compared.
// Known, documented deviations are asserted explicitly so that an upstream fix is noticed (see docs/architecture.md).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <UnrealLib/UnrealLib.hpp>

using namespace UnrealLib;
namespace fs = std::filesystem;
using json = nlohmann::json;

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

    fs::path utf8_path(const std::string& text)
    {
        return fs::path(std::u8string(text.begin(), text.end()));
    }

    fs::path dump_dir()
    {
        const std::string value = env("UNREALLIB_UELIB_DUMP_DIR");
#ifdef UNREALLIB_DEFAULT_UELIB_DUMP_DIR
        return utf8_path(value.empty() ? std::string(UNREALLIB_DEFAULT_UELIB_DUMP_DIR) : value);
#else
        return utf8_path(value);
#endif
    }

    fs::path fixture_dir()
    {
        const std::string value = env("UNREALLIB_MIRRORSEDGE_DIR");
#ifdef UNREALLIB_DEFAULT_MIRRORSEDGE_DIR
        return utf8_path(value.empty() ? std::string(UNREALLIB_DEFAULT_MIRRORSEDGE_DIR) : value);
#else
        return utf8_path(value);
#endif
    }

    std::string hex32(std::uint32_t v)
    {
        return fmt::format("0x{:08X}", v);
    }

    std::string hex64(std::uint64_t v)
    {
        return fmt::format("0x{:016X}", v);
    }

    json name_ref(const FName& n)
    {
        return json::array({n.index, n.number});
    }

    json summary_json(const PackageSummary& s)
    {
        json j;
        j["file_version"] = s.file_version;
        j["licensee_version"] = s.licensee_version;
        j["header_size"] = s.header_size;
        j["folder_name"] = s.folder_name.text;
        j["package_flags"] = hex32(s.package_flags);
        j["name_count"] = s.name_count;
        j["name_offset"] = s.name_offset;
        j["export_count"] = s.export_count;
        j["export_offset"] = s.export_offset;
        j["import_count"] = s.import_count;
        j["import_offset"] = s.import_offset;
        j["depends_offset"] = s.depends_offset;
        j["guid"] = s.guid.to_string();
        j["generations"] = json::array();
        for (const auto& g : s.generations)
        {
            j["generations"].push_back(json::array({g.export_count, g.name_count, g.net_object_count}));
        }
        j["engine_version"] = s.engine_version;
        j["cooker_version"] = s.cooker_version;
        j["compression_flags"] = hex32(s.compression_flags);
        j["compressed_chunks"] = json::array();
        for (const auto& c : s.compressed_chunks)
        {
            j["compressed_chunks"].push_back({{"uncompressed_offset", c.uncompressed_offset},
                                              {"uncompressed_size", c.uncompressed_size},
                                              {"compressed_offset", c.compressed_offset},
                                              {"compressed_size", c.compressed_size}});
        }
        j["package_source"] = hex32(s.package_source);
        j["additional_packages_to_cook"] = json::array();
        for (const auto& p : s.additional_packages_to_cook)
        {
            j["additional_packages_to_cook"].push_back(p.text);
        }
        return j;
    }

    json tables_json(const Package& package)
    {
        json j;
        j["names"] = json::array();
        for (const auto& n : package.names().entries())
        {
            j["names"].push_back({{"text", n.text.text}, {"flags", hex64(n.flags)}});
        }
        j["imports"] = json::array();
        for (const auto& i : package.imports())
        {
            j["imports"].push_back({{"class_package", name_ref(i.class_package)},
                                    {"class", name_ref(i.class_name)},
                                    {"outer", i.outer.value},
                                    {"name", name_ref(i.object_name)}});
        }
        j["exports"] = json::array();
        for (const auto& e : package.exports())
        {
            json components = json::array();
            for (const auto& c : e.component_map)
            {
                components.push_back(json::array({name_ref(c.name), c.object.value}));
            }
            j["exports"].push_back({{"class", e.class_index.value},
                                    {"super", e.super_index.value},
                                    {"outer", e.outer.value},
                                    {"name", name_ref(e.object_name)},
                                    {"archetype", e.archetype.value},
                                    {"object_flags", hex64(e.object_flags)},
                                    {"serial_size", e.serial_size},
                                    {"serial_offset", e.serial_offset},
                                    {"export_flags", hex32(e.export_flags)},
                                    {"component_map", components},
                                    {"net_object_counts", e.generation_net_object_count},
                                    {"package_guid", e.package_guid.to_string()},
                                    {"package_flags", hex32(e.package_flags)}});
        }
        j["depends"] = json::array();
        for (const auto& list : package.depends())
        {
            json values = json::array();
            for (const auto d : list)
            {
                values.push_back(d.value);
            }
            j["depends"].push_back(values);
        }
        return j;
    }

    // Upstream's CompressedChunk.Deserialize reads (UncompressedOffset, CompressedOffset, UncompressedSize,
    // CompressedSize) into (UncompressedOffset, UncompressedSize, CompressedOffset, CompressedSize). Map our verified
    // values to what upstream is expected to expose.
    json expected_upstream_chunks(const PackageSummary& s)
    {
        json chunks = json::array();
        for (const auto& c : s.compressed_chunks)
        {
            chunks.push_back({{"uncompressed_offset", c.uncompressed_offset},
                              {"uncompressed_size", c.compressed_offset},
                              {"compressed_offset", c.uncompressed_size},
                              {"compressed_size", c.compressed_size}});
        }
        return chunks;
    }

    std::string describe_diff(const json& expected, const json& actual, std::size_t max_ops = 8)
    {
        const json patch = json::diff(expected, actual);
        std::ostringstream out;
        std::size_t shown = 0;
        for (const auto& op : patch)
        {
            if (shown++ == max_ops)
            {
                out << "  ... " << (patch.size() - max_ops) << " more\n";
                break;
            }
            const std::string path = op["path"].get<std::string>();
            json upstream_value;
            try
            {
                upstream_value = actual.at(json::json_pointer(path));
            }
            catch (const json::exception&)
            {
                upstream_value = "<absent>";
            }
            json ours;
            try
            {
                ours = expected.at(json::json_pointer(path));
            }
            catch (const json::exception&)
            {
                ours = "<absent>";
            }
            out << "  " << op["op"].get<std::string>() << " " << path << ": unreallib=" << ours.dump()
                << " upstream=" << upstream_value.dump() << "\n";
        }
        return out.str();
    }

    struct DumpEntry
    {
        fs::path package;
        json upstream;
    };

    std::vector<DumpEntry> load_dumps()
    {
        std::vector<DumpEntry> entries;
        for (const auto& item : fs::directory_iterator(dump_dir()))
        {
            if (!item.is_regular_file() || item.path().extension() != ".json")
            {
                continue;
            }
            std::ifstream in(item.path(), std::ios::binary);
            DumpEntry entry;
            entry.upstream = json::parse(in);
            entry.package = fixture_dir() / utf8_path(entry.upstream.at("file").get<std::string>());
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    class DifferentialTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if (dump_dir().empty() || !fs::is_directory(dump_dir()) || fixture_dir().empty())
            {
                GTEST_SKIP() << "no UELib dump directory / fixture directory (configure with "
                                "-DUNREALLIB_BUILD_DIFFERENTIAL=ON -DUNREALLIB_MIRRORSEDGE_DIR=...)";
            }
            m_entries = load_dumps();
            if (m_entries.empty())
            {
                GTEST_SKIP() << "UELib dump directory is empty";
            }
        }

        std::vector<DumpEntry> m_entries;
    };
}

TEST_F(DifferentialTest, UpstreamLoadsEveryPackage)
{
    for (const auto& e : m_entries)
    {
        EXPECT_FALSE(e.upstream.contains("error"))
            << e.upstream.at("file").get<std::string>() << ": " << e.upstream.value("error", "");
    }
}

TEST_F(DifferentialTest, SummariesMatch)
{
    int compared = 0;
    for (const auto& e : m_entries)
    {
        if (e.upstream.contains("error"))
        {
            continue;
        }
        SCOPED_TRACE(e.package.string());
        const auto bytes = read_file(e.package, 1ull << 31);
        BinaryReader reader(bytes);
        const PackageSummary summary = read_package_summary(reader);

        json ours = summary_json(summary);
        json upstream = e.upstream.at("summary");
        // Compressed chunks are compared in KnownDeviationCompressedChunkFieldOrder.
        ours.erase("compressed_chunks");
        upstream.erase("compressed_chunks");
        EXPECT_EQ(ours, upstream) << describe_diff(ours, upstream);
        ++compared;
    }
    RecordProperty("compared", compared);
    EXPECT_GT(compared, 0);
}

TEST_F(DifferentialTest, KnownDeviationCompressedChunkFieldOrder)
{
    int compressed = 0;
    for (const auto& e : m_entries)
    {
        if (e.upstream.contains("error"))
        {
            continue;
        }
        SCOPED_TRACE(e.package.string());
        const auto bytes = read_file(e.package, 1ull << 31);
        BinaryReader reader(bytes);
        const PackageSummary summary = read_package_summary(reader);
        const json& upstream_chunks = e.upstream.at("summary").at("compressed_chunks");
        EXPECT_EQ(expected_upstream_chunks(summary), upstream_chunks)
            << "upstream's compressed-chunk field order changed; update docs/architecture.md and this test";
        compressed += summary.is_compressed() ? 1 : 0;
    }
    RecordProperty("compressed", compressed);
}

TEST_F(DifferentialTest, TablesMatch)
{
    int compared = 0;
    for (const auto& e : m_entries)
    {
        if (e.upstream.contains("error") || !e.upstream.at("tables_loaded").get<bool>())
        {
            continue; // upstream stops after the summary for compressed packages
        }
        SCOPED_TRACE(e.package.string());
        const Package package = Package::open(e.package);
        const json ours = tables_json(package);
        for (const char* table : {"names", "imports", "exports", "depends"})
        {
            const json& upstream = e.upstream.at(table);
            EXPECT_EQ(ours.at(table), upstream) << table << " differ:\n" << describe_diff(ours.at(table), upstream);
        }
        ++compared;
    }
    RecordProperty("compared", compared);
    EXPECT_GT(compared, 0);
}

TEST_F(DifferentialTest, UpstreamBuildDetectionAgreesWithProfile)
{
    for (const auto& e : m_entries)
    {
        if (e.upstream.contains("error"))
        {
            continue;
        }
        SCOPED_TRACE(e.package.string());
        const auto bytes = read_file(e.package, 1ull << 31);
        BinaryReader reader(bytes);
        const PackageSummary summary = read_package_summary(reader);
        const auto detection = GameProfileRegistry::builtin().detect(summary);
        ASSERT_NE(detection.profile, nullptr);
        const bool upstream_me = e.upstream.at("upstream_build").get<std::string>() == "MirrorsEdge";
        EXPECT_EQ(detection.profile->id == "mirrorsedge-pc", upstream_me);
    }
}

namespace
{
    /// UnrealLib's property tags of @p ref in the dump schema (see UELibDump WriteObjects). Upstream's
    /// UScriptStruct reads its struct defaults into the same Properties collection (replacing the leading, empty,
    /// object property list), so for a ScriptStruct the defaults are compared.
    json property_tags_json(Linker& linker, ObjectRef ref)
    {
        const Package& package = *ref.package;
        json out = json::array();
        const std::vector<Property> list = package.class_name(ref.index) == "ScriptStruct"
                                               ? linker.read_field(ref).script_struct.value().defaults
                                               : linker.read_properties(ref).properties;
        for (const Property& p : list)
        {
            if (!p.tag) continue;
            // Known upstream deviation: its PropertyType enum aliases DelegateProperty to StringProperty (both 7), and
            // the dump's Enum.GetName reports "StringProperty". Map the stored type name to what upstream exposes.
            std::string type = package.names().to_string(p.tag->type_name);
            if (type == "DelegateProperty") type = "StringProperty";
            json tag = {{"name", name_ref(p.tag->name)},
                        {"type", type},
                        {"size", p.tag->size},
                        {"index", p.tag->array_index}};
            if (p.tag->type == PropertyType::Struct && p.tag->struct_name) tag["struct"] = name_ref(*p.tag->struct_name);
            if (p.tag->bool_value) tag["bool"] = *p.tag->bool_value;
            out.push_back(std::move(tag));
        }
        return out;
    }

    /// [memory position, code] per token, sorted by position (code: opcode, or 0x10000 + native index).
    json script_tokens_json(Linker& linker, ObjectRef ref, std::size_t& storage_size)
    {
        const Package& package = *ref.package;
        const FieldObject field = linker.read_field(ref);
        json out = json::array();
        storage_size = 0;
        if (!field.structure || field.structure->script_size <= 0) return out;
        storage_size = field.structure->script_storage_size;
        const TokenMap map = TokenMap::ue3(package.summary().file_version, &package.profile());
        BytecodeOptions options;
        options.token_map = &map;
        options.name_count = static_cast<std::int32_t>(package.names().size());
        options.none_index = package.names().find("None").value_or(-1);
        BinaryReader reader(package.export_data(ref.index.export_index()), package.context());
        reader.seek(field.structure->script_offset);
        const Script script = decode_script(reader, static_cast<std::size_t>(field.structure->script_size), options);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> tokens;
        for (const Token& t : script.tokens)
        {
            tokens.emplace_back(t.memory_offset,
                                t.kind == TokenKind::NativeFunction ? 0x10000u + t.native_index : t.opcode);
        }
        std::sort(tokens.begin(), tokens.end());
        for (const auto& [position, code] : tokens) out.push_back(json::array({position, code}));
        return out;
    }
}

TEST_F(DifferentialTest, ObjectsMatch)
{
    // Script packages that upstream deserialized completely (uncompressed .u): property tags of every export and the
    // bytecode token stream of every struct with script, for the objects upstream loaded without error.
    std::size_t objects = 0, tag_lists = 0, scripts = 0, tokens = 0, packages = 0;
    std::size_t upstream_failed = 0, read_where_upstream_failed = 0;
    std::map<std::string, std::size_t> mismatches;
    const auto mismatch = [&](const std::string& kind, const std::string& where, const std::string& detail) {
        if (mismatches[kind]++ < 5) ADD_FAILURE() << kind << " differ for " << where << "\n" << detail;
    };
    for (const auto& e : m_entries)
    {
        if (!e.upstream.contains("objects")) continue;
        ++packages;
        Linker linker;
        linker.add_search_path(fixture_dir());
        const Package& package = linker.load(e.package);
        for (const json& o : e.upstream.at("objects"))
        {
            const auto index = o.at("export").get<std::size_t>();
            if (index >= package.exports().size()) continue;
            const ObjectRef ref{&package, ObjectIndex::from_export(index)};
            if (!o.at("ok").get<bool>())
            {
                // Upstream could not deserialize the object; record whether UnrealLib reads its properties.
                ++upstream_failed;
                try
                {
                    (void)property_tags_json(linker, ref);
                    ++read_where_upstream_failed;
                }
                catch (const Error&)
                {
                }
                continue;
            }
            const std::string where = linker.qualified_path(ref) + " (" + package.class_name(ref.index) + ")";
            ++objects;

            json ours;
            try
            {
                ours = property_tags_json(linker, ref);
            }
            catch (const Error& error)
            {
                ours = std::string("error: ") + error.what();
            }
            if (ours != o.at("properties"))
            {
                mismatch("property tags", where, describe_diff(ours, o.at("properties")));
                ++mismatches["property tags of a " + package.class_name(ref.index)];
            }
            ++tag_lists;

            if (!o.contains("script")) continue;
            json upstream_tokens = json::array();
            for (const json& t : o.at("script").at("tokens")) upstream_tokens.push_back(json::array({t[0], t[2]}));
            std::size_t storage = 0;
            json our_tokens;
            try
            {
                our_tokens = script_tokens_json(linker, ref, storage);
            }
            catch (const Error& error)
            {
                our_tokens = std::string("error: ") + error.what();
            }
            if (our_tokens != upstream_tokens) mismatch("bytecode tokens", where, describe_diff(our_tokens, upstream_tokens));
            if (storage != o.at("script").at("storage_size").get<std::size_t>())
            {
                mismatch("script storage size", where,
                         fmt::format("upstream {}, UnrealLib {}", o.at("script").at("storage_size").get<std::size_t>(),
                                     storage));
            }
            ++scripts;
            tokens += upstream_tokens.size();
        }
    }
    std::cout << "  " << packages << " script packages, " << objects << " objects, " << tag_lists
              << " property tag lists, " << scripts << " scripts (" << tokens << " tokens) compared; upstream failed on "
              << upstream_failed << " objects, UnrealLib reads the properties of " << read_where_upstream_failed
              << " of them\n";
    for (const auto& [kind, n] : mismatches) std::cout << "  " << n << " mismatching " << kind << '\n';
    if (packages == 0) GTEST_SKIP() << "no dump has objects (regenerate the UELib dumps)";
}
