// Runs every fuzz target (tests/Fuzz/Targets) on its seeds and on deterministic mutations of them, with any compiler.
// The libFuzzer executables (UNREALLIB_BUILD_FUZZERS) explore far more; this keeps the targets and the library's
// robustness checked on every test run. UNREALLIB_FUZZ_ITERATIONS overrides the mutation count per target.
//
// Inputs that once broke a target (libFuzzer crash files) are kept in tests/Fuzz/Regressions/<target>/ and replayed
// first. UNREALLIB_FUZZ_TRACE=<file> writes each input to <file> before running it (to catch hangs).
//
// Every input must be accepted or rejected with UnrealLib::Error. Any other exception, or a violated invariant, fails
// the test and prints the input (hex).

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include "Fuzz/FuzzTargets.hpp"
#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
using namespace UnrealLib::Fuzz;

namespace
{
    struct InvariantViolation : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    using Target = void (*)(const std::uint8_t*, std::size_t);
    using Seeds = std::vector<std::vector<std::byte>> (*)();

    std::string hex(const std::vector<std::byte>& input)
    {
        std::string out;
        for (std::size_t i = 0; i < input.size() && i < 512; ++i)
        {
            out += fmt::format("{:02x}", static_cast<unsigned>(input[i]));
        }
        return input.size() > 512 ? out + "..." : out;
    }

    /// Runs @p target on @p input; returns whether it was accepted (true) or rejected with UnrealLib::Error (false).
    /// Anything else is a test failure.
    bool run(Target target, const std::vector<std::byte>& input, const char* what, std::string* rejection = nullptr)
    {
        // Debugging hangs: UNREALLIB_FUZZ_TRACE=<file> holds the input being run.
        static const std::string trace = TestSupport::env("UNREALLIB_FUZZ_TRACE");
        if (!trace.empty())
        {
            std::ofstream(trace, std::ios::binary | std::ios::trunc)
                .write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(input.size()));
        }
        try
        {
            target(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
            return true;
        }
        catch (const Error& e)
        {
            if (rejection != nullptr) *rejection = e.what();
            return false;
        }
        catch (const InvariantViolation& e)
        {
            ADD_FAILURE() << what << ": invariant violated: " << e.what() << "\ninput: " << hex(input);
        }
        catch (const std::exception& e)
        {
            ADD_FAILURE() << what << ": non-UnrealLib exception: " << e.what() << "\ninput: " << hex(input);
        }
        return false;
    }

    std::vector<std::byte> mutate(const std::vector<std::vector<std::byte>>& seeds, std::mt19937& rng)
    {
        std::vector<std::byte> input = seeds[rng() % seeds.size()];
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits; ++e)
        {
            if (input.empty())
            {
                input.push_back(static_cast<std::byte>(rng()));
                continue;
            }
            const std::size_t at = rng() % input.size();
            switch (rng() % 7)
            {
                case 0: input[at] = static_cast<std::byte>(rng()); break;
                case 1: input[at] ^= static_cast<std::byte>(1u << (rng() % 8)); break;
                case 2:
                {
                    static constexpr std::uint32_t interesting[] = {0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0xFFFF,
                                                                    0x7FFFFFFF, 0x80000000, 0xFFFFFFFF};
                    const std::uint32_t v = interesting[rng() % std::size(interesting)];
                    for (std::size_t b = 0; b < 4 && at + b < input.size(); ++b)
                    {
                        input[at + b] = static_cast<std::byte>(v >> (8 * b));
                    }
                    break;
                }
                case 3: input.resize(at); break;
                case 4: input.insert(input.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::byte>(rng())); break;
                case 5: // duplicate a chunk
                {
                    const std::size_t len = std::min<std::size_t>(1 + rng() % 16, input.size() - at);
                    const std::vector<std::byte> chunk(input.begin() + static_cast<std::ptrdiff_t>(at),
                                                       input.begin() + static_cast<std::ptrdiff_t>(at + len));
                    input.insert(input.begin() + static_cast<std::ptrdiff_t>(rng() % input.size()), chunk.begin(),
                                 chunk.end());
                    break;
                }
                default: // splice in bytes from another seed
                {
                    const auto& other = seeds[rng() % seeds.size()];
                    if (other.empty()) break;
                    const std::size_t from = rng() % other.size();
                    const std::size_t len = std::min<std::size_t>({1 + rng() % 32, other.size() - from, input.size() - at});
                    std::copy_n(other.begin() + static_cast<std::ptrdiff_t>(from), len,
                                input.begin() + static_cast<std::ptrdiff_t>(at));
                    break;
                }
            }
        }
        return input;
    }

    struct ReplayCase
    {
        const char* name;
        Target target;
        Seeds seeds;
        bool seeds_must_pass; // every seed is a valid input
    };

    class FuzzReplayTest : public ::testing::TestWithParam<ReplayCase>
    {
    protected:
        void SetUp() override
        {
            invariant_handler = [](const char* message) { throw InvariantViolation(message); };
        }
        void TearDown() override { invariant_handler = nullptr; }
    };

    std::size_t iterations()
    {
        const std::string text = TestSupport::env("UNREALLIB_FUZZ_ITERATIONS");
        return text.empty() ? 2000 : std::stoul(text);
    }
}

TEST_P(FuzzReplayTest, SeedsAndMutations)
{
    const ReplayCase& c = GetParam();
    const std::filesystem::path regressions =
        std::filesystem::path(UNREALLIB_FUZZ_REGRESSION_DIR) / c.name;
    if (std::filesystem::is_directory(regressions))
    {
        for (const auto& entry : std::filesystem::directory_iterator(regressions))
        {
            std::ifstream in(entry.path(), std::ios::binary);
            const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            std::vector<std::byte> input(chars.size());
            std::memcpy(input.data(), chars.data(), chars.size());
            const std::string label = "regression " + entry.path().filename().string();
            (void)run(c.target, input, label.c_str());
        }
    }
    const auto seeds = c.seeds();
    ASSERT_FALSE(seeds.empty());
    for (const auto& seed : seeds)
    {
        std::string rejection;
        const bool accepted = run(c.target, seed, "seed", &rejection);
        if (c.seeds_must_pass) EXPECT_TRUE(accepted) << c.name << " rejected a seed (" << rejection << "): " << hex(seed);
    }
    std::mt19937 rng(0x5EED0000u ^ static_cast<unsigned>(std::hash<std::string>{}(c.name)));
    std::size_t accepted = 0;
    const std::size_t n = iterations();
    for (std::size_t i = 0; i < n && !HasFailure(); ++i)
    {
        accepted += run(c.target, mutate(seeds, rng), "mutation") ? 1 : 0;
    }
    std::cout << "  " << c.name << ": " << accepted << " of " << n << " mutations accepted\n";
}

INSTANTIATE_TEST_SUITE_P(
    Targets, FuzzReplayTest,
    ::testing::Values(ReplayCase{"package", fuzz_package, seeds_package, true},
                      ReplayCase{"serialization", fuzz_serialization, seeds_serialization, true},
                      ReplayCase{"properties", fuzz_properties, seeds_properties, true},
                      ReplayCase{"bytecode", fuzz_bytecode, seeds_bytecode, true},
                      ReplayCase{"compression", fuzz_compression, seeds_compression, true},
                      ReplayCase{"assets", fuzz_assets, seeds_assets, true},
                      ReplayCase{"text", fuzz_text, seeds_text, true}),
    [](const ::testing::TestParamInfo<ReplayCase>& info) { return std::string(info.param.name); });
