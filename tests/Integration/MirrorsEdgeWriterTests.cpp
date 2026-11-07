// Package writer round trips against real Mirror's Edge packages.
// Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set; the full corpus needs UNREALLIB_FIXTURE_SWEEP=1.
//
//   * uncompressed packages are rewritten byte-identically;
//   * compressed packages are rewritten uncompressed and reload with the same names, imports, exports (paths,
//     classes, flags), depends lists and export bytes;
//   * every 25th package is also rewritten LZO-compressed and checked the same way.

#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <string>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Writer/PackageWriter.hpp>

#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
using namespace UnrealLib::TestSupport;

namespace
{
    struct RoundTripTally
    {
        std::size_t packages = 0;
        std::size_t identical = 0;
        std::size_t equivalent = 0;
        std::size_t compressed_checked = 0;
        std::size_t failures = 0;
        std::uint64_t original_bytes = 0;
        std::uint64_t lzo_bytes = 0;
        std::uint64_t lzo_source_bytes = 0;
    };

    /// Empty string if @p a and @p b have the same contents (not byte layout).
    std::string compare(const Package& a, const Package& b)
    {
        if (a.names().size() != b.names().size()) return "name count";
        for (std::size_t i = 0; i < a.names().size(); ++i)
        {
            if (!(a.names().entries()[i] == b.names().entries()[i])) return "name " + std::to_string(i);
        }
        if (!std::equal(a.imports().begin(), a.imports().end(), b.imports().begin(), b.imports().end())) return "imports";
        if (a.exports().size() != b.exports().size()) return "export count";
        for (std::size_t i = 0; i < a.exports().size(); ++i)
        {
            ExportEntry x = a.exports()[i], y = b.exports()[i];
            x.serial_offset = y.serial_offset = 0;
            if (!(x == y)) return "export entry " + std::to_string(i);
            const auto da = a.export_data(i), db = b.export_data(i);
            if (!std::equal(da.begin(), da.end(), db.begin(), db.end())) return "export data " + std::to_string(i);
        }
        if (a.depends() != b.depends()) return "depends";
        return {};
    }

    void round_trip(const std::filesystem::path& file, std::size_t ordinal, RoundTripTally& tally)
    {
        ++tally.packages;
        const Package original = Package::open(file);
        const std::string name = file.stem().string();
        try
        {
            const PackageModel model = PackageModel::from_package(original);
            const std::vector<std::byte> written = write_package(model);
            tally.original_bytes += original.file_size();
            if (!original.summary().is_compressed())
            {
                const auto data = original.data();
                if (std::equal(data.begin(), data.end(), written.begin(), written.end()))
                {
                    ++tally.identical;
                }
                else if (tally.failures++ < 10)
                {
                    ADD_FAILURE() << name << ": uncompressed rewrite is not byte-identical";
                }
            }
            else
            {
                Package reloaded;
                reloaded.load_from_memory(written, name);
                const std::string difference = compare(original, reloaded);
                if (difference.empty()) ++tally.equivalent;
                else if (tally.failures++ < 10) ADD_FAILURE() << name << ": " << difference;
            }
            if (ordinal % 25 == 0)
            {
                WriteOptions options;
                options.compression = CompressionMethod::Lzo;
                const std::vector<std::byte> lzo = write_package(model, options);
                Package reloaded;
                reloaded.load_from_memory(lzo, name);
                const std::string difference = compare(original, reloaded);
                ++tally.compressed_checked;
                tally.lzo_bytes += lzo.size();
                tally.lzo_source_bytes += written.size();
                if (!difference.empty() && tally.failures++ < 10) ADD_FAILURE() << name << " (LZO): " << difference;
            }
        }
        catch (const Error& e)
        {
            if (tally.failures++ < 10) ADD_FAILURE() << name << ": " << e.what();
        }
    }

    void report(const RoundTripTally& t)
    {
        std::cout << "  packages: " << t.packages << ", byte-identical: " << t.identical
                  << ", equivalent (compressed originals): " << t.equivalent << ", LZO rewrites checked: "
                  << t.compressed_checked << ", failures: " << t.failures << '\n';
        if (t.lzo_source_bytes != 0)
        {
            std::cout << "  LZO output: " << t.lzo_bytes << " of " << t.lzo_source_bytes << " bytes ("
                      << (100 * t.lzo_bytes / t.lzo_source_bytes) << "%)\n";
        }
        ::testing::Test::RecordProperty("identical", static_cast<int>(t.identical));
    }
}

TEST(MirrorsEdgeWriterTests, ScriptPackagesRoundTrip)
{
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    RoundTripTally tally;
    std::size_t ordinal = 0;
    for (const auto& file : packages(false)) round_trip(file, ordinal++, tally);
    report(tally);
    EXPECT_GT(tally.packages, 0u);
    EXPECT_EQ(tally.failures, 0u);
}

TEST(MirrorsEdgeWriterTests, FullCorpusRoundTrip)
{
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    RoundTripTally tally;
    std::size_t ordinal = 0;
    for (const auto& file : packages(true)) round_trip(file, ordinal++, tally);
    report(tally);
    EXPECT_EQ(tally.failures, 0u);
}
