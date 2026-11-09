// Corpus regression baseline: tests/Reference/mirrorsedge-pc.tsv holds normalized metadata of every package of the
// Mirror's Edge PC release (no game content: sizes, versions, counts and hashes). FullCorpusMatchesReference loads
// every package and compares; any change in how a package's summary or tables are read shows up as a diff line.
//
//   UNREALLIB_FIXTURE_SWEEP=1                 run the comparison
//   UNREALLIB_FIXTURE_SWEEP=1 UNREALLIB_UPDATE_REFERENCE=1   rewrite the baseline (review the diff before committing)
//
// Columns: path (relative to CookedPC), file size, file version, licensee version, profile id, compression flags,
// names, imports, exports, total export bytes, FNV-1a 64 of the re-serialized name/import/export/depends tables.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include <UnrealLib/UnrealLib.hpp>

#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
using namespace UnrealLib::TestSupport;
namespace fs = std::filesystem;

namespace
{
    fs::path reference_file()
    {
        return fs::path(UNREALLIB_REFERENCE_DIR) / "mirrorsedge-pc.tsv";
    }

    std::uint64_t fnv1a(std::span<const std::byte> bytes, std::uint64_t hash = 0xCBF29CE484222325ull)
    {
        for (const std::byte b : bytes)
        {
            hash ^= static_cast<std::uint8_t>(b);
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    std::string describe(const fs::path& file)
    {
        LoadOptions options;
        const Package package = Package::open(file, options);
        const PackageSummary& s = package.summary();
        BinaryWriter tables(package.context());
        for (const auto& n : package.names().entries()) write_name_entry(tables, n);
        for (const auto& i : package.imports()) write_import_entry(tables, i);
        std::uint64_t export_bytes = 0;
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            write_export_entry(tables, package.exports()[i]);
            export_bytes += package.export_data(i).size();
        }
        for (const auto& list : package.depends())
        {
            tables.write_i32(static_cast<std::int32_t>(list.size()));
            for (const ObjectIndex d : list) tables.write_i32(d.value);
        }
        const std::string relative = fs::relative(file, fixture_dir()).generic_string();
        return fmt::format("{}\t{}\t{}\t{}\t{}\t0x{:X}\t{}\t{}\t{}\t{}\t{:016X}", relative, fs::file_size(file),
                           s.file_version, s.licensee_version, package.profile().id, s.compression_flags,
                           package.names().size(), package.imports().size(), package.exports().size(), export_bytes,
                           fnv1a(tables.data()));
    }
}

TEST(MirrorsEdgeReferenceTests, FullCorpusMatchesReference)
{
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    auto files = packages(true);
    std::sort(files.begin(), files.end());
    std::map<std::string, std::string> actual; // path -> line
    for (const auto& file : files)
    {
        std::string line;
        try
        {
            line = describe(file);
        }
        catch (const Error& e)
        {
            line = fs::relative(file, fixture_dir()).generic_string() + "\terror: " + e.what();
        }
        actual.emplace(line.substr(0, line.find('\t')), line);
    }

    if (env("UNREALLIB_UPDATE_REFERENCE") == "1")
    {
        std::ofstream out(reference_file(), std::ios::binary | std::ios::trunc);
        out << "# path\tsize\tfile_version\tlicensee\tprofile\tcompression\tnames\timports\texports\texport_bytes\t"
               "tables_fnv1a\n";
        for (const auto& [path, line] : actual) out << line << '\n';
        GTEST_SKIP() << "wrote " << actual.size() << " lines to " << reference_file().string();
    }

    std::ifstream in(reference_file(), std::ios::binary);
    ASSERT_TRUE(in) << "missing " << reference_file().string() << " (generate with UNREALLIB_UPDATE_REFERENCE=1)";
    std::map<std::string, std::string> expected;
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        expected.emplace(line.substr(0, line.find('\t')), line);
    }
    std::size_t differences = 0;
    for (const auto& [path, line] : expected)
    {
        const auto it = actual.find(path);
        if (it == actual.end())
        {
            if (++differences <= 10) ADD_FAILURE() << "missing package: " << path;
        }
        else if (it->second != line)
        {
            if (++differences <= 10) ADD_FAILURE() << "changed:\n  reference: " << line << "\n  now:       " << it->second;
        }
    }
    for (const auto& [path, line] : actual)
    {
        if (!expected.contains(path) && ++differences <= 10) ADD_FAILURE() << "new package: " << line;
    }
    std::cout << "  " << actual.size() << " packages compared with " << expected.size() << " reference lines, "
              << differences << " differences\n";
    EXPECT_EQ(differences, 0u);
}
