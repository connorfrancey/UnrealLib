// Linker / object-header integration tests against real Mirror's Edge packages.
// Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set; the whole-corpus variants also need UNREALLIB_FIXTURE_SWEEP=1.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Utilities/Text.hpp>

#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
namespace fs = std::filesystem;

using namespace UnrealLib::TestSupport;

namespace
{
    // UE3 property type names that may appear in a property tag's type field.
    bool is_property_type(std::string_view type)
    {
        static constexpr std::string_view types[] = {
            "ByteProperty",  "IntProperty",   "BoolProperty",      "FloatProperty",  "ObjectProperty",
            "NameProperty",  "StrProperty",   "StringProperty",    "ArrayProperty",  "StructProperty",
            "MapProperty",   "ClassProperty", "DelegateProperty",  "InterfaceProperty", "ComponentProperty",
        };
        for (const auto candidate : types)
        {
            if (candidate == type)
            {
                return true;
            }
        }
        return false;
    }

    // Stock Mirror's Edge (Steam) packages are all LZO-compressed; packages that were re-saved by mod tools are
    // stored uncompressed and may legitimately reference content that is not installed.
    bool is_stock(const Package& package)
    {
        return package.summary().is_compressed();
    }

    struct Tally
    {
        std::map<std::string, std::size_t> statuses;
        std::map<std::string, std::size_t> unresolved_examples_per_status;
        std::size_t headers = 0;
        std::size_t header_failures = 0;
        std::size_t undecidable = 0;
    };

    void resolve_all_imports(Linker& linker, const Package& package, Tally& tally)
    {
        const bool expect_all_ok = is_stock(package);
        for (std::size_t i = 0; i < package.imports().size(); ++i)
        {
            const Resolution& r = linker.resolve(package, ObjectIndex::from_import(i));
            ++tally.statuses[std::string(to_string(r.status))];
            if (!r.ok())
            {
                auto& shown = tally.unresolved_examples_per_status[std::string(to_string(r.status))];
                if (shown++ < 5)
                {
                    std::cout << "  [" << to_string(r.status) << "] " << package.name() << ": "
                              << package.object_path(ObjectIndex::from_import(i)) << " - " << r.detail << '\n';
                }
                if (expect_all_ok || r.status == ResolveStatus::Invalid || r.status == ResolveStatus::ClassMismatch)
                {
                    ADD_FAILURE() << package.name() << ": " << package.object_path(ObjectIndex::from_import(i))
                                  << " -> " << to_string(r.status) << " (" << r.detail << ")";
                }
            }
        }
    }

    // Reads every export's UObject header and checks that a valid property-tag name follows it.
    void check_headers(Linker& linker, const Package& package, Tally& tally)
    {
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ExportEntry& e = package.exports()[i];
            if (e.serial_size <= 0)
            {
                continue;
            }
            const ObjectRef ref{&package, ObjectIndex::from_export(i)};
            const auto input = linker.header_input(ref);
            if (!input)
            {
                ++tally.undecidable;
                continue;
            }
            ++tally.headers;
            try
            {
                const auto data = package.export_data(i);
                BinaryReader reader(data, package.context());
                const ObjectHeader header = read_object_header(reader, *input);
                if (input->is_class)
                {
                    continue; // UClass data (UStruct/UState/UClass fields) follows, not property tags
                }
                // A property tag must follow: "None", or a name followed by a property type name.
                const FName tag_name = read_fname(reader);
                const auto tag_text = package.names().try_to_string(tag_name);
                bool valid = tag_text.has_value() && tag_name.number >= 0;
                std::string type_text;
                if (valid && *tag_text != "None")
                {
                    const FName type = read_fname(reader);
                    type_text = package.names().try_to_string(type).value_or("<invalid>");
                    valid = type.number == 0 && is_property_type(type_text);
                }
                if (!valid && tally.header_failures++ < 10)
                {
                    ADD_FAILURE() << linker.qualified_path(ref) << " (class " << package.class_name(ref.index)
                                  << "): no valid property tag at offset " << header.properties_offset << " (name '"
                                  << tag_text.value_or("<invalid>") << "', type '" << type_text << "')";
                }
            }
            catch (const Error& error)
            {
                if (tally.header_failures++ < 10)
                {
                    ADD_FAILURE() << linker.qualified_path(ref) << ": " << error.what();
                }
            }
        }
    }

    void report(const Tally& tally)
    {
        for (const auto& [status, count] : tally.statuses)
        {
            std::cout << "  imports " << status << ": " << count << '\n';
            ::testing::Test::RecordProperty("imports_" + status, static_cast<int>(count));
        }
        std::cout << "  headers checked: " << tally.headers << ", failures: " << tally.header_failures
                  << ", class undecidable: " << tally.undecidable << '\n';
        ::testing::Test::RecordProperty("headers", static_cast<int>(tally.headers));
        ::testing::Test::RecordProperty("header_failures", static_cast<int>(tally.header_failures));
        ::testing::Test::RecordProperty("undecidable", static_cast<int>(tally.undecidable));
    }
}

TEST(MirrorsEdgeLinkerTests, ScriptPackagesResolveAllImports)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    Tally tally;
    for (const auto& file : packages(false))
    {
        const Package* package = linker.find_package(fs::path(file).stem().string());
        ASSERT_NE(package, nullptr) << file;
        resolve_all_imports(linker, *package, tally);
    }
    report(tally);
    EXPECT_GT(tally.statuses["export"], 0u);
}

TEST(MirrorsEdgeLinkerTests, ScriptPackageObjectHeaders)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    Linker linker;
    linker.add_search_path(fixture_dir());
    Tally tally;
    for (const auto& file : packages(false))
    {
        const Package* package = linker.find_package(fs::path(file).stem().string());
        ASSERT_NE(package, nullptr) << file;
        check_headers(linker, *package, tally);
    }
    report(tally);
    EXPECT_EQ(tally.header_failures, 0u);
    EXPECT_EQ(tally.undecidable, 0u);
    EXPECT_GT(tally.headers, 0u);
}

TEST(MirrorsEdgeLinkerTests, FullCorpusLinking)
{
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    Tally tally;
    std::size_t count = 0;
    for (const auto& file : packages(true))
    {
        // A fresh linker per package keeps memory bounded; script packages are reloaded as needed.
        Linker linker;
        linker.add_search_path(fixture_dir());
        const Package& package = linker.load(file);
        resolve_all_imports(linker, package, tally);
        check_headers(linker, package, tally);
        ++count;
    }
    std::cout << "  packages: " << count << '\n';
    report(tally);
    EXPECT_EQ(tally.header_failures, 0u);
}
