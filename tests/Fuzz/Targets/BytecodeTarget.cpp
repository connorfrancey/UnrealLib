// Fuzz target "bytecode": UnrealScript token decoding for several file versions.

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>

#include "../FuzzTargets.hpp"

UNREALLIB_FUZZ_TARGET(bytecode)
{
    using namespace UnrealLib;
    if (size < 3) return;
    // Byte 0: file version; bytes 1-2: in-memory script size; the rest: bytecode.
    // TokenMap::ue3() supports file versions >= 178.
    static constexpr std::uint16_t versions[] = {178, 343, 512, 536, 587, 868};
    const std::size_t which = data[0] % std::size(versions);
    const std::uint16_t version = versions[which];
    const std::size_t memory_size = static_cast<std::size_t>(data[1]) | (static_cast<std::size_t>(data[2]) << 8);
    static const TokenMap maps[] = {TokenMap::ue3(178), TokenMap::ue3(343), TokenMap::ue3(512),
                                    TokenMap::ue3(536), TokenMap::ue3(587), TokenMap::ue3(868)};
    SerializationContext context;
    context.file_version = version;
    context.limits.max_allocation_bytes = 1u << 20;
    BinaryReader reader(Fuzz::as_bytes(data + 3, size - 3), context);
    BytecodeOptions options;
    options.token_map = &maps[which];
    options.name_count = 64;
    options.none_index = 0;
    options.max_tokens = 1u << 16;
    const Script script = decode_script(reader, memory_size, options);
    Fuzz::fuzz_check(script.memory_size == memory_size, "a decoded script ends exactly at ScriptSize");
    for (const std::uint32_t s : script.statements)
    {
        Fuzz::fuzz_check(s < script.tokens.size(), "statement index in range");
    }
    for (const Token& t : script.tokens)
    {
        for (const std::uint32_t c : t.children) Fuzz::fuzz_check(c < script.tokens.size(), "child index in range");
    }
}
