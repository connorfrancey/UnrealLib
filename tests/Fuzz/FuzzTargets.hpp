#pragma once

// Fuzz targets shared by the libFuzzer executables (UNREALLIB_BUILD_FUZZERS, clang-cl) and the GoogleTest replay
// (FuzzReplayTests, every compiler).
//
// Contract of every target: for ANY input it returns normally or throws UnrealLib::Error. Anything else — another
// exception type (std::bad_alloc would mean an uncontrolled allocation), a crash, a hang, a sanitizer report, or a
// failed round-trip invariant (reported with fuzz_check(), which aborts) — is a bug.
//
// A target file defines `void UnrealLib::Fuzz::fuzz_<name>(const std::uint8_t*, std::size_t)` with
// UNREALLIB_FUZZ_TARGET(name). LibFuzzerMain.cpp, compiled once per libFuzzer executable with
// UNREALLIB_LIBFUZZER_TARGET=<name>, turns one of them into LLVMFuzzerTestOneInput (swallowing UnrealLib::Error).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include <UnrealLib/Core/Error.hpp>

namespace UnrealLib::Fuzz
{
    /// Called by fuzz_check() on a violated invariant; must not return (the replay test throws). nullptr: abort,
    /// which libFuzzer reports as a crash together with the input.
    inline void (*invariant_handler)(const char* message) = nullptr;

    /// Reports @p message if @p condition is false (see invariant_handler).
    inline void fuzz_check(bool condition, const char* message)
    {
        if (!condition)
        {
            if (invariant_handler != nullptr) invariant_handler(message);
            std::fprintf(stderr, "fuzz invariant violated: %s\n", message);
            std::abort();
        }
    }

    inline std::span<const std::byte> as_bytes(const std::uint8_t* data, std::size_t size)
    {
        return {reinterpret_cast<const std::byte*>(data), size};
    }

    // Entry points (non-libFuzzer builds). Each may throw UnrealLib::Error.
    void fuzz_package(const std::uint8_t* data, std::size_t size);
    void fuzz_serialization(const std::uint8_t* data, std::size_t size);
    void fuzz_properties(const std::uint8_t* data, std::size_t size);
    void fuzz_bytecode(const std::uint8_t* data, std::size_t size);
    void fuzz_compression(const std::uint8_t* data, std::size_t size);
    void fuzz_assets(const std::uint8_t* data, std::size_t size);
    void fuzz_text(const std::uint8_t* data, std::size_t size);

    // Seed inputs per target (valid or nearly valid inputs that reach deep code). Used by the replay test and written
    // to disk by UnrealLibFuzzSeeds for libFuzzer corpora.
    std::vector<std::vector<std::byte>> seeds_package();
    std::vector<std::vector<std::byte>> seeds_serialization();
    std::vector<std::vector<std::byte>> seeds_properties();
    std::vector<std::vector<std::byte>> seeds_bytecode();
    std::vector<std::vector<std::byte>> seeds_compression();
    std::vector<std::vector<std::byte>> seeds_assets();
    std::vector<std::vector<std::byte>> seeds_text();
}

#define UNREALLIB_FUZZ_CONCAT_(a, b) a##b
#define UNREALLIB_FUZZ_CONCAT(a, b) UNREALLIB_FUZZ_CONCAT_(a, b)

// Defines the entry point of a target (in namespace UnrealLib::Fuzz).
#define UNREALLIB_FUZZ_TARGET(name) void UnrealLib::Fuzz::UNREALLIB_FUZZ_CONCAT(fuzz_, name)(const std::uint8_t* data, std::size_t size)
