// Deterministic mutation testing of the package loader.
//
// Every mutated input must either load or fail with an UnrealLib::Error — never crash, hang, trip a sanitizer, or
// throw anything else (std::bad_alloc in particular would indicate an uncontrolled allocation). Run under the
// msvc-x64-asan preset for memory-safety coverage. Broader coverage: FuzzReplayTests.cpp (all fuzz targets) and the
// libFuzzer executables (tests/Fuzz/CMakeLists.txt).

#include <gtest/gtest.h>

#include <random>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Package/PackageValidator.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using namespace UnrealLib::Testing;

namespace
{
    struct MutationOutcome
    {
        int loaded = 0;
        int rejected = 0;
    };

    void exercise(const std::vector<std::byte>& input, MutationOutcome& outcome)
    {
        LoadOptions options;
        options.limits.max_allocation_bytes = 16u << 20; // keep allocations small under mutation
        options.limits.max_decompressed_size = 16u << 20;
        try
        {
            Package package;
            package.load_from_memory(input, "Mutated", options);
            // Exercise the read-side API on whatever loaded.
            (void)validate_package(package);
            for (std::size_t i = 0; i < package.exports().size(); ++i)
            {
                const auto index = ObjectIndex::from_export(i);
                (void)package.object_path(index);
                try
                {
                    (void)package.class_name(index);
                    (void)package.export_data(i);
                }
                catch (const Error&)
                {
                }
            }
            ++outcome.loaded;
        }
        catch (const Error&)
        {
            ++outcome.rejected;
        }
    }

    void run_mutations(const std::vector<std::byte>& seed, unsigned rng_seed, int iterations)
    {
        std::mt19937 rng(rng_seed);
        MutationOutcome outcome;
        for (int i = 0; i < iterations; ++i)
        {
            std::vector<std::byte> input = seed;
            const int edits = 1 + static_cast<int>(rng() % 8);
            for (int e = 0; e < edits; ++e)
            {
                const std::size_t position = rng() % input.size();
                switch (rng() % 4)
                {
                case 0: input[position] = static_cast<std::byte>(rng()); break;           // random byte
                case 1: input[position] ^= static_cast<std::byte>(1u << (rng() % 8)); break; // bit flip
                case 2: // interesting 32-bit value
                {
                    static constexpr std::uint32_t values[] = {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x10000};
                    const std::uint32_t v = values[rng() % std::size(values)];
                    for (std::size_t k = 0; k < 4 && position + k < input.size(); ++k)
                    {
                        input[position + k] = static_cast<std::byte>((v >> (8 * k)) & 0xFF);
                    }
                    break;
                }
                default: input.resize(position + 1); break; // truncation
                }
            }
            exercise(input, outcome);
        }
        // Sanity: the unmutated seed must load.
        MutationOutcome seed_outcome;
        exercise(seed, seed_outcome);
        EXPECT_EQ(seed_outcome.loaded, 1);
        ::testing::Test::RecordProperty("loaded", outcome.loaded);
        ::testing::Test::RecordProperty("rejected", outcome.rejected);
    }
}

TEST(MutationTests, UncompressedPackageSurvivesMutations)
{
    run_mutations(build_synthetic_package(make_basic_spec()).file, 1, 4000);
}

TEST(MutationTests, CompressedPackageSurvivesMutations)
{
    SyntheticPackageSpec spec = make_basic_spec();
    spec.compression = CompressionMethod::Zlib;
    spec.block_size = 64;
    run_mutations(build_synthetic_package(spec).file, 2, 4000);
}

TEST(MutationTests, RandomBuffersAreRejected)
{
    std::mt19937 rng(3);
    MutationOutcome outcome;
    for (int i = 0; i < 2000; ++i)
    {
        std::vector<std::byte> input(rng() % 512);
        for (auto& b : input)
        {
            b = static_cast<std::byte>(rng());
        }
        if (input.size() >= 4 && (rng() % 2) == 0) // valid signature to get deeper
        {
            input[0] = std::byte{0xC1};
            input[1] = std::byte{0x83};
            input[2] = std::byte{0x2A};
            input[3] = std::byte{0x9E};
        }
        exercise(input, outcome);
    }
    EXPECT_EQ(outcome.loaded + outcome.rejected, 2000);
}
