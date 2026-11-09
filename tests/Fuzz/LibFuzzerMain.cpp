// libFuzzer entry point for one target, selected with -DUNREALLIB_LIBFUZZER_TARGET=<name> (see FuzzTargets.hpp).
// UnrealLib::Error is the documented way to reject input and is swallowed; everything else reaches libFuzzer.
// UNREALLIB_FUZZ_TRACE=<file> writes each input to <file> before running it (for failures libFuzzer cannot catch).

#include <cstdio>
#include <cstdlib>

#include "FuzzTargets.hpp"

#ifndef UNREALLIB_LIBFUZZER_TARGET
#error "UNREALLIB_LIBFUZZER_TARGET must name a fuzz target (package, serialization, ...)"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static const char* trace = std::getenv("UNREALLIB_FUZZ_TRACE");
    if (trace != nullptr)
    {
        if (std::FILE* f = std::fopen(trace, "wb"))
        {
            std::fwrite(data, 1, size, f);
            std::fclose(f);
        }
    }
    try
    {
        UnrealLib::Fuzz::UNREALLIB_FUZZ_CONCAT(fuzz_, UNREALLIB_LIBFUZZER_TARGET)(data, size);
    }
    catch (const UnrealLib::Error&)
    {
    }
    return 0;
}
