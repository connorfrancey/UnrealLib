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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
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

// ---------------------------------------------------------------------------------------------------------------------
// Parity report: one accuracy percentage for the port.
//
// Every item that both libraries produce for the dumped packages is scored as matching or not, per area: summary
// fields, game detection, name/import/export/depends rows, property tags, bytecode tokens and script storage sizes.
// Documented upstream deviations (compressed-chunk field order, DelegateProperty reported as StringProperty, token
// storage positions) are normalized exactly as in the tests above, so they do not count against the port. Objects and
// packages that upstream itself fails to read have no reference value and are reported separately ("beyond upstream").
//
// Prints a table and records the percentages as test properties. Optional environment variables:
//   UNREALLIB_PARITY_MIN_PERCENT  fail when the overall (area-weighted) accuracy is below this value
//   UNREALLIB_PARITY_REPORT       also write the report as JSON to this path
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
    struct Tally
    {
        std::size_t matched = 0;
        std::size_t total = 0;

        void add(bool match)
        {
            matched += match ? 1 : 0;
            ++total;
        }

        [[nodiscard]] double percent() const
        {
            return total == 0 ? 100.0 : 100.0 * static_cast<double>(matched) / static_cast<double>(total);
        }
    };

    /// Element-wise comparison of two arrays: every index present in either counts once. A non-array @p ours (an
    /// error message) scores every upstream element, and at least one item, as a mismatch.
    void tally_rows(Tally& tally, const json& ours, const json& upstream)
    {
        if (!ours.is_array())
        {
            tally.total += std::max<std::size_t>(upstream.size(), 1);
            return;
        }
        const std::size_t rows = std::max(ours.size(), upstream.size());
        for (std::size_t i = 0; i < rows; ++i)
        {
            tally.add(i < ours.size() && i < upstream.size() && ours[i] == upstream[i]);
        }
    }

    /// Token streams compared as sets of [position, code]: matched = intersection, total = union, so missing and
    /// extra tokens both count.
    void tally_tokens(Tally& tally, const json& ours, const json& upstream)
    {
        if (!ours.is_array())
        {
            tally.total += std::max<std::size_t>(upstream.size(), 1);
            return;
        }
        std::vector<json> a(ours.begin(), ours.end());
        std::vector<json> b(upstream.begin(), upstream.end());
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        std::vector<json> common;
        std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(common));
        tally.matched += common.size();
        tally.total += a.size() + b.size() - common.size();
    }

    /// Rounded down to two decimals, so that 100.00% is only shown when every item matches.
    double floor2(double percent)
    {
        return std::floor(percent * 100.0) / 100.0;
    }

    const std::vector<std::string>& parity_areas()
    {
        static const std::vector<std::string> areas = {
            "Package summary fields", "Game detection",     "Name table rows", "Import table rows",
            "Export table rows",      "Depends table rows", "Property tags",   "Bytecode tokens",
            "Script storage sizes"};
        return areas;
    }
}

TEST_F(DifferentialTest, ParityReport)
{
    std::map<std::string, Tally> areas;
    std::size_t packages = 0, upstream_failed_packages = 0, read_where_upstream_failed_packages = 0;
    std::size_t upstream_failed_objects = 0, read_where_upstream_failed_objects = 0;

    for (const auto& e : m_entries)
    {
        if (e.upstream.contains("error"))
        {
            ++upstream_failed_packages;
            try
            {
                (void)Package::open(e.package);
                ++read_where_upstream_failed_packages;
            }
            catch (const Error&)
            {
            }
            continue;
        }
        ++packages;

        // Summary, field by field (compressed chunks in upstream's documented field order).
        const json& upstream_summary = e.upstream.at("summary");
        try
        {
            const auto bytes = read_file(e.package, 1ull << 31);
            BinaryReader reader(bytes);
            const PackageSummary summary = read_package_summary(reader);
            json ours = summary_json(summary);
            ours["compressed_chunks"] = expected_upstream_chunks(summary);
            for (const auto& [key, value] : upstream_summary.items())
            {
                areas["Package summary fields"].add(ours.contains(key) && ours.at(key) == value);
            }
            const auto detection = GameProfileRegistry::builtin().detect(summary);
            const bool upstream_me = e.upstream.at("upstream_build").get<std::string>() == "MirrorsEdge";
            areas["Game detection"].add(detection.profile != nullptr &&
                                        (detection.profile->id == "mirrorsedge-pc") == upstream_me);
        }
        catch (const Error&)
        {
            areas["Package summary fields"].total += upstream_summary.size();
            areas["Game detection"].total += 1;
        }

        // Tables (upstream reads them for uncompressed packages only).
        if (e.upstream.at("tables_loaded").get<bool>())
        {
            json ours;
            try
            {
                ours = tables_json(Package::open(e.package));
            }
            catch (const Error& error)
            {
                ours = {{"names", error.what()}, {"imports", error.what()}, {"exports", error.what()},
                        {"depends", error.what()}};
            }
            tally_rows(areas["Name table rows"], ours.at("names"), e.upstream.at("names"));
            tally_rows(areas["Import table rows"], ours.at("imports"), e.upstream.at("imports"));
            tally_rows(areas["Export table rows"], ours.at("exports"), e.upstream.at("exports"));
            tally_rows(areas["Depends table rows"], ours.at("depends"), e.upstream.at("depends"));
        }

        // Objects of script packages: property tags and bytecode.
        if (!e.upstream.contains("objects")) continue;
        Linker linker;
        linker.add_search_path(fixture_dir());
        const Package* package = nullptr;
        try
        {
            package = &linker.load(e.package);
        }
        catch (const Error&)
        {
        }
        for (const json& o : e.upstream.at("objects"))
        {
            const auto index = o.at("export").get<std::size_t>();
            const bool exists = package != nullptr && index < package->exports().size();
            const ObjectRef ref{package, ObjectIndex::from_export(index)};
            if (!o.at("ok").get<bool>())
            {
                ++upstream_failed_objects;
                try
                {
                    if (exists)
                    {
                        (void)property_tags_json(linker, ref);
                        ++read_where_upstream_failed_objects;
                    }
                }
                catch (const Error&)
                {
                }
                continue;
            }

            json ours_tags = "error: export missing";
            if (exists)
            {
                try
                {
                    ours_tags = property_tags_json(linker, ref);
                }
                catch (const Error& error)
                {
                    ours_tags = std::string("error: ") + error.what();
                }
            }
            tally_rows(areas["Property tags"], ours_tags, o.at("properties"));

            if (!o.contains("script")) continue;
            json upstream_tokens = json::array();
            for (const json& t : o.at("script").at("tokens")) upstream_tokens.push_back(json::array({t[0], t[2]}));
            std::size_t storage = 0;
            json ours_tokens = "error: export missing";
            if (exists)
            {
                try
                {
                    ours_tokens = script_tokens_json(linker, ref, storage);
                }
                catch (const Error& error)
                {
                    ours_tokens = std::string("error: ") + error.what();
                }
            }
            tally_tokens(areas["Bytecode tokens"], ours_tokens, upstream_tokens);
            areas["Script storage sizes"].add(ours_tokens.is_array() &&
                                              storage == o.at("script").at("storage_size").get<std::size_t>());
        }
    }

    // Overall: area-weighted (each feature area counts equally) is the headline; item-weighted is dominated by the
    // largest areas (names, tokens) and is shown for reference.
    Tally all;
    double area_sum = 0.0;
    std::size_t area_count = 0;
    json report = {{"packages", packages}, {"areas", json::array()}};
    std::ostringstream table;
    table << fmt::format("\n  UELib parity report ({} packages compared with upstream UELib)\n", packages);
    table << fmt::format("  {:<26} {:>12} {:>12} {:>10}\n", "Area", "Matched", "Compared", "Accuracy");
    for (const std::string& name : parity_areas())
    {
        const Tally& t = areas[name];
        if (t.total == 0)
        {
            table << fmt::format("  {:<26} {:>12} {:>12} {:>10}\n", name, "-", "-", "n/a");
            continue;
        }
        table << fmt::format("  {:<26} {:>12} {:>12} {:>9.2f}%\n", name, t.matched, t.total,
                             floor2(t.percent()));
        report["areas"].push_back(
            {{"area", name}, {"matched", t.matched}, {"compared", t.total}, {"percent", t.percent()}});
        RecordProperty("parity." + name, fmt::format("{:.4f}", t.percent()));
        all.matched += t.matched;
        all.total += t.total;
        area_sum += t.percent();
        ++area_count;
    }
    if (area_count == 0) GTEST_SKIP() << "no comparable data in the UELib dumps";

    const double overall = area_sum / static_cast<double>(area_count);
    table << fmt::format("  {:<52} {:>9.2f}%\n", "Overall accuracy (areas weighted equally)", floor2(overall));
    table << fmt::format("  {:<52} {:>9.2f}%\n", "Overall accuracy (every compared item)", floor2(all.percent()));
    table << fmt::format("  Beyond upstream: UnrealLib reads {} of {} packages and the properties of {} of {} "
                         "objects that upstream fails on\n",
                         read_where_upstream_failed_packages, upstream_failed_packages,
                         read_where_upstream_failed_objects, upstream_failed_objects);
    std::cout << table.str();

    report["overall_percent"] = overall;
    report["overall_item_percent"] = all.percent();
    report["beyond_upstream"] = {{"packages_upstream_failed", upstream_failed_packages},
                                 {"packages_read", read_where_upstream_failed_packages},
                                 {"objects_upstream_failed", upstream_failed_objects},
                                 {"objects_read", read_where_upstream_failed_objects}};
    RecordProperty("parity.overall", fmt::format("{:.4f}", overall));
    RecordProperty("parity.overall_items", fmt::format("{:.4f}", all.percent()));

    if (const std::string path = env("UNREALLIB_PARITY_REPORT"); !path.empty())
    {
        std::ofstream out(utf8_path(path), std::ios::binary);
        out << report.dump(2) << '\n';
        EXPECT_TRUE(out.good()) << "could not write " << path;
    }
    if (const std::string minimum = env("UNREALLIB_PARITY_MIN_PERCENT"); !minimum.empty())
    {
        EXPECT_GE(overall, std::stod(minimum)) << "UELib parity below UNREALLIB_PARITY_MIN_PERCENT";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Speed comparison: UnrealLib (C++) vs. upstream UELib (C#).
//
// The CTest fixture "Differential.UELibBench.Run" runs `UELibDump --bench` (see Program.cs), which times upstream on
// two workloads and writes per-package times. This test runs the same workloads on the same packages with UnrealLib,
// using the same method (one warm-up pass over all packages, then the minimum of N runs per package), and prints the
// speedup (upstream time / UnrealLib time):
//
//   load     open the package: summary and name/import/export/depends tables (upstream: UnrealLoader.LoadPackage;
//            UnrealLib: Package::open, which also reads all export data into memory)
//   objects  open a script package and read every export: property tags, reflection data and bytecode tokens
//            (upstream: InitializePackage(All) + Load; UnrealLib: Linker::read_properties / read_field +
//            decode_script). Upstream never loads other packages, so the packages UnrealLib's Linker needs to resolve
//            imported classes are loaded before the clock starts.
//
// Release builds only (in Debug builds the numbers are meaningless and the test skips). Optional environment
// variables: UNREALLIB_UELIB_BENCH_FILE (upstream results), UNREALLIB_BENCH_ITERATIONS (default: upstream's count),
// UNREALLIB_BENCH_DETAILS (print per-package times), UNREALLIB_SPEED_REPORT (also write the results as JSON to this
// path), UNREALLIB_MIN_SPEEDUP (fail when the overall speedup is below this factor).
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
    fs::path bench_file()
    {
        const std::string value = env("UNREALLIB_UELIB_BENCH_FILE");
#ifdef UNREALLIB_DEFAULT_UELIB_BENCH_FILE
        return utf8_path(value.empty() ? std::string(UNREALLIB_DEFAULT_UELIB_BENCH_FILE) : value);
#else
        return utf8_path(value);
#endif
    }

    using bench_clock = std::chrono::steady_clock;

    double elapsed_ms(bench_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(bench_clock::now() - start).count();
    }

    /// Opens the package (summary and tables). Returns the timed milliseconds; @p sink receives a value that depends
    /// on the work, so it cannot be optimized away.
    double bench_load(const fs::path& file, std::size_t& sink)
    {
        const auto start = bench_clock::now();
        const Package package = Package::open(file);
        sink += package.names().size() + package.imports().size() + package.exports().size();
        return elapsed_ms(start);
    }

    /// Names of the packages @p package imports from (top-level imports of class Package).
    std::vector<std::string> imported_packages(const Package& package)
    {
        std::vector<std::string> out;
        for (const ImportEntry& import : package.imports())
        {
            if (import.outer.is_null() && package.names().to_string(import.class_name) == "Package")
            {
                out.push_back(package.names().to_string(import.object_name));
            }
        }
        return out;
    }

    /// The packages @p file depends on, transitively (cached: computing it opens the packages).
    const std::vector<std::string>& dependencies_of(const fs::path& file)
    {
        static std::map<fs::path, std::vector<std::string>> cache;
        if (const auto it = cache.find(file); it != cache.end()) return it->second;
        Linker linker;
        linker.add_search_path(file.parent_path(), false);
        std::vector<std::string> result;
        std::vector<std::string> pending = imported_packages(Package::open(file));
        const std::string self = file.stem().string();
        while (!pending.empty())
        {
            const std::string name = pending.back();
            pending.pop_back();
            const auto same = [&](const std::string& other) {
                return Text::iequals(other, name);
            };
            if (Text::iequals(name, self) || std::any_of(result.begin(), result.end(), same)) continue;
            const Package* dependency = linker.find_package(name);
            if (dependency == nullptr) continue;
            result.push_back(name);
            for (std::string next : imported_packages(*dependency)) pending.push_back(std::move(next));
        }
        return cache.emplace(file, std::move(result)).first->second;
    }

    /// Reads every export of a script package: reflection data and bytecode for fields, property tags otherwise.
    ///
    /// Upstream never loads other packages. UnrealLib's Linker loads the packages that define imported classes (to
    /// resolve class hierarchies) and indexes each one's exports on first use; a Linker that reads many packages pays
    /// for that once. So before the clock starts, the dependencies are loaded and indexed by resolving the imports of
    /// a copy of the package registered under another name. The timed part loads the package itself, with empty
    /// caches of its own, and reads it.
    double bench_objects(const fs::path& file, std::size_t& sink)
    {
        Linker linker;
        linker.add_search_path(file.parent_path(), false);
        for (const std::string& dependency : dependencies_of(file)) (void)linker.find_package(dependency);
        {
            Package copy;
            copy.load_from_memory(read_file(file, 1ull << 31), "__bench_warmup_" + file.stem().string());
            const Package& warm = linker.add(std::move(copy));
            for (std::size_t i = 0; i < warm.imports().size(); ++i)
            {
                (void)linker.resolve(warm, ObjectIndex::from_import(i));
            }
        }

        const auto start = bench_clock::now();
        const Package& package = linker.load(file);
        const TokenMap map = TokenMap::ue3(package.summary().file_version, &package.profile());
        BytecodeOptions options;
        options.token_map = &map;
        options.name_count = static_cast<std::int32_t>(package.names().size());
        options.none_index = package.names().find("None").value_or(-1);

        std::size_t items = 0;
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ObjectRef ref{&package, ObjectIndex::from_export(i)};
            try
            {
                if (field_kind_of(package.class_name(ref.index)))
                {
                    const FieldObject field = linker.read_field(ref);
                    items += field.properties.size() + 1;
                    if (field.structure && field.structure->script_size > 0)
                    {
                        BinaryReader reader(package.export_data(i), package.context());
                        reader.seek(field.structure->script_offset);
                        items += decode_script(reader, static_cast<std::size_t>(field.structure->script_size), options)
                                     .tokens.size();
                    }
                }
                else
                {
                    items += linker.read_properties(ref).properties.size();
                }
            }
            catch (const Error&)
            {
                // Same as upstream: failures are part of the work, not of the result.
            }
        }
        const double ms = elapsed_ms(start);
        sink += items;
        return ms;
    }

    using BenchWork = double (*)(const fs::path&, std::size_t&);

    struct WorkloadResult
    {
        std::size_t packages = 0;
        double upstream_ms = 0.0;
        double ours_ms = 0.0;
        double log_speedup_sum = 0.0; ///< Sum of ln(per-package speedup), for the geometric mean.
        json details = json::array(); ///< [{package, upstream_ms, unreallib_ms, speedup}]

        [[nodiscard]] double speedup() const { return upstream_ms / ours_ms; }
        [[nodiscard]] double geomean_speedup() const
        {
            return std::exp(log_speedup_sum / static_cast<double>(packages));
        }
        [[nodiscard]] json to_json() const
        {
            return {{"packages", packages},     {"upstream_ms", upstream_ms},      {"unreallib_ms", ours_ms},
                    {"speedup", speedup()},     {"geomean_speedup", geomean_speedup()}};
        }
    };

    /// One warm-up pass over all packages, then per package the minimum of @p iterations runs (as UELibDump does).
    WorkloadResult run_workload(const json& upstream_times, int iterations, BenchWork work, std::size_t& sink)
    {
        WorkloadResult result;
        std::vector<std::pair<fs::path, double>> packages;
        for (const auto& [relative, ms] : upstream_times.items())
        {
            packages.emplace_back(fixture_dir() / utf8_path(relative), ms.get<double>());
        }
        for (const auto& [file, _] : packages)
        {
            (void)work(file, sink);
        }
        for (const auto& [file, upstream_ms] : packages)
        {
            double best = std::numeric_limits<double>::max();
            for (int i = 0; i < iterations; ++i)
            {
                best = std::min(best, work(file, sink));
            }
            ++result.packages;
            result.upstream_ms += upstream_ms;
            result.ours_ms += best;
            result.log_speedup_sum += std::log(upstream_ms / best);
            result.details.push_back({{"package", file.filename().string()},
                                      {"upstream_ms", upstream_ms},
                                      {"unreallib_ms", best},
                                      {"speedup", upstream_ms / best}});
        }
        return result;
    }
}

TEST(DifferentialSpeed, SpeedComparison)
{
#ifndef NDEBUG
    GTEST_SKIP() << "speed comparison needs a Release build of UnrealLib";
#endif
    const fs::path file = bench_file();
    if (file.empty() || !fs::is_regular_file(file) || fixture_dir().empty())
    {
        GTEST_SKIP() << "no UELib benchmark results (configure with -DUNREALLIB_BUILD_DIFFERENTIAL=ON "
                        "-DUNREALLIB_MIRRORSEDGE_DIR=... and run through CTest)";
    }
    std::ifstream in(file, std::ios::binary);
    const json upstream = json::parse(in);
    int iterations = upstream.at("iterations").get<int>();
    if (const std::string value = env("UNREALLIB_BENCH_ITERATIONS"); !value.empty()) iterations = std::stoi(value);

    std::size_t sink = 0;
    const std::vector<std::tuple<std::string, std::string, BenchWork>> workloads = {
        {"load", "Open package (summary + tables)", &bench_load},
        {"objects", "Read all objects (props + bytecode)", &bench_objects}};

    // Speedup = total upstream time / total UnrealLib time (dominated by the largest packages); the geometric mean
    // of the per-package speedups weights every package equally.
    std::ostringstream table;
    table << fmt::format("\n  Speed: UnrealLib (C++) vs upstream UELib (C#, {}), minimum of {} runs per package\n",
                         upstream.value("runtime", "?"), iterations);
    table << fmt::format("  {:<36} {:>8} {:>13} {:>13} {:>9} {:>9}\n", "Workload", "Packages", "UELib (C#)",
                         "UnrealLib", "Speedup", "Geomean");
    const bool show_details = !env("UNREALLIB_BENCH_DETAILS").empty();
    json report = {{"runtime", upstream.value("runtime", "?")}, {"iterations", iterations},
                   {"workloads", json::array()}};
    WorkloadResult total;
    for (const auto& [key, label, work] : workloads)
    {
        if (!upstream.contains(key) || upstream.at(key).empty()) continue;
        const WorkloadResult r = run_workload(upstream.at(key), iterations, work, sink);
        if (show_details)
        {
            table << "  " << label << ":\n";
            for (const json& d : r.details)
            {
                table << fmt::format("    {:<42} {:>10.2f} ms {:>10.2f} ms {:>8.2f}x\n",
                                     d.at("package").get<std::string>(), d.at("upstream_ms").get<double>(),
                                     d.at("unreallib_ms").get<double>(), d.at("speedup").get<double>());
            }
        }
        table << fmt::format("  {:<36} {:>8} {:>10.1f} ms {:>10.1f} ms {:>8.1f}x {:>8.1f}x\n", label, r.packages,
                             r.upstream_ms, r.ours_ms, r.speedup(), r.geomean_speedup());
        json entry = r.to_json();
        entry["key"] = key;
        entry["label"] = label;
        entry["details"] = r.details;
        report["workloads"].push_back(std::move(entry));
        RecordProperty("speedup." + key, fmt::format("{:.2f}", r.speedup()));
        RecordProperty("speedup_geomean." + key, fmt::format("{:.2f}", r.geomean_speedup()));
        total.packages += r.packages;
        total.upstream_ms += r.upstream_ms;
        total.ours_ms += r.ours_ms;
        total.log_speedup_sum += r.log_speedup_sum;
    }
    if (total.packages == 0) GTEST_SKIP() << "the UELib benchmark results contain no packages";

    const double overall = total.speedup();
    table << fmt::format("  {:<45} {:>10.1f} ms {:>10.1f} ms {:>8.1f}x {:>8.1f}x\n", "Total", total.upstream_ms,
                         total.ours_ms, overall, total.geomean_speedup());
    std::cout << table.str();
    RecordProperty("speedup.overall", fmt::format("{:.2f}", overall));
    EXPECT_GT(sink, 0u);

    report["total"] = total.to_json();
    if (const std::string path = env("UNREALLIB_SPEED_REPORT"); !path.empty())
    {
        std::ofstream out(utf8_path(path), std::ios::binary);
        out << report.dump(2) << '\n';
        EXPECT_TRUE(out.good()) << "could not write " << path;
    }

    if (const std::string minimum = env("UNREALLIB_MIN_SPEEDUP"); !minimum.empty())
    {
        EXPECT_GE(overall, std::stod(minimum)) << "UnrealLib is less than UNREALLIB_MIN_SPEEDUP times faster";
    }
}
