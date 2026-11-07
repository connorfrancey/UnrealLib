// Fuzz target "serialization": the primitive readers (CompactIndex, FString, FName, array counts, GUIDs, ...) in
// every version/byte-order context, driven by an opcode stream, plus write/read round trips of what was read.

#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "../FuzzTargets.hpp"

UNREALLIB_FUZZ_TARGET(serialization)
{
    using namespace UnrealLib;
    if (size < 2) return;
    // Byte 0: context (file version and byte order); the rest: data. Opcodes are taken from the data itself.
    static constexpr std::uint16_t versions[] = {61, 68, 178, 249, 343, 536, 587, 868};
    SerializationContext context;
    context.file_version = versions[data[0] % std::size(versions)];
    context.endian = (data[0] & 0x80) != 0 ? Endian::Big : Endian::Little;
    context.limits.max_allocation_bytes = 1u << 20;
    BinaryReader reader(Fuzz::as_bytes(data + 1, size - 1), context);
    while (!reader.at_end())
    {
        switch (reader.read_u8() % 9)
        {
            case 0:
            {
                const std::int32_t value = reader.read_compact_index();
                BinaryWriter writer(context);
                writer.write_compact_index(value);
                BinaryReader back(writer.data(), context);
                Fuzz::fuzz_check(back.read_compact_index() == value && back.at_end(), "CompactIndex round trip");
                break;
            }
            case 1:
            {
                const FString text = reader.read_fstring();
                BinaryWriter writer(context);
                writer.write_fstring(text);
                BinaryReader back(writer.data(), context);
                Fuzz::fuzz_check(back.read_fstring() == text && back.at_end(), "FString round trip");
                break;
            }
            case 2: (void)read_fname(reader); break;
            case 3: (void)reader.read_index(); break;
            case 4: (void)reader.read_array_count(1 + reader.read_u8() % 16); break;
            case 5: (void)reader.read_guid(); break;
            case 6: (void)reader.read_string(); break;
            case 7: (void)reader.read_ansi_null_string(); break;
            default: (void)reader.read_f32(); break;
        }
    }
}
