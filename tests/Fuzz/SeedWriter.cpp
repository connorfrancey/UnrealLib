// UnrealLibFuzzSeeds <dir>: writes the seed inputs of every fuzz target to <dir>/<target>/seed-<n>.bin, the initial
// libFuzzer corpora.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "FuzzTargets.hpp"

int main(int argc, char** argv)
{
    using namespace UnrealLib::Fuzz;
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: UnrealLibFuzzSeeds <corpus-directory>\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const std::pair<const char*, std::vector<std::vector<std::byte>> (*)()> targets[] = {
        {"package", seeds_package},         {"serialization", seeds_serialization},
        {"properties", seeds_properties},   {"bytecode", seeds_bytecode},
        {"compression", seeds_compression}, {"assets", seeds_assets},
        {"text", seeds_text}};
    for (const auto& [name, seeds] : targets)
    {
        const std::filesystem::path dir = root / name;
        std::filesystem::create_directories(dir);
        std::size_t n = 0;
        for (const auto& seed : seeds())
        {
            std::ofstream out(dir / ("seed-" + std::to_string(n++) + ".bin"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
        }
        std::printf("%s: %zu seeds\n", name, n);
    }
    return 0;
}
