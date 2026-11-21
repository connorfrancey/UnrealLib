#include <UnrealLib/Assets/Sound.hpp>

#include <cstring>
#include <limits>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib::Assets
{
    namespace
    {
        std::vector<std::int32_t> read_channel_table(BinaryReader& reader)
        {
            const auto count = reader.read_array_count(sizeof(std::int32_t));
            std::vector<std::int32_t> values;
            values.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                values.push_back(reader.read_i32());
            }
            return values;
        }

        BulkData read_lazy_sound_data(BinaryReader& reader)
        {
            const auto version = reader.context().file_version;
            const auto end_offset = reader.read_i32();
            BulkData data;
            if (version >= 254) (void)reader.read_i32(); // StorageSizeAddedToLazyArray
            if (version >= 251) data.flags = reader.read_u32();
            if (version >= 260) (void)read_fname(reader); // Storage package name
            data.element_count = reader.read_index();
            const auto start = reader.absolute_position();
            if (data.element_count < 0 || end_offset < 0 ||
                (end_offset != 0 && static_cast<std::uint64_t>(end_offset) < start) ||
                start > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
            {
                throw SerializationError("invalid SoundNodeWave lazy array", start);
            }
            data.payload_offset = start;
            data.offset_in_file = static_cast<std::int32_t>(start);
            data.size_on_disk = end_offset == 0 ? data.element_count : end_offset - data.offset_in_file;
            reader.skip(static_cast<std::size_t>(data.size_on_disk));
            return data;
        }
    }

    std::string_view file_extension(AudioContainer container) noexcept
    {
        switch (container)
        {
            case AudioContainer::Wave: return "wav";
            case AudioContainer::Ogg: return "ogg";
            case AudioContainer::Xma: return "xma";
            default: return "bin";
        }
    }

    AudioContainer detect_audio_container(std::span<const std::byte> bytes) noexcept
    {
        const auto starts = [&](const char* magic, std::size_t at = 0) {
            const std::size_t n = std::strlen(magic);
            if (bytes.size() < at + n) return false;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (std::to_integer<char>(bytes[at + i]) != magic[i]) return false;
            }
            return true;
        };
        if (starts("OggS")) return AudioContainer::Ogg;
        if (starts("RIFF") && starts("WAVE", 8))
        {
            // fmt chunk format tag: 0x165/0x166 = XMA.
            if (bytes.size() >= 22 && starts("fmt ", 12))
            {
                const auto tag = std::to_integer<std::uint32_t>(bytes[20]) |
                                 std::to_integer<std::uint32_t>(bytes[21]) << 8;
                if (tag == 0x165 || tag == 0x166) return AudioContainer::Xma;
            }
            return AudioContainer::Wave;
        }
        return AudioContainer::Unknown;
    }

    const BulkData* SoundWave::best_payload() const noexcept
    {
        for (const BulkData* b : {&raw_data, &compressed_pc, &compressed_xbox360, &compressed_ps3, &compressed_wiiu,
                               &compressed_iphone, &compressed_flash})
        {
            if (b->has_payload() && b->is_inline()) return b;
        }
        return nullptr;
    }

    SoundWave read_sound_wave(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        const AssetSource source = open_asset(linker, object, schema);
        if (!Text::iequals(source.class_name, "SoundNodeWave"))
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("'{}' is a {}, not a SoundNodeWave",
                                                                linker.qualified_path(object), source.class_name));
        }
        BinaryReader reader = source.native_reader();
        const std::uint16_t version = reader.context().file_version;
        // Upstream USoundNodeWave.Deserialize gates (PackageObjectLegacyVersion).
        if (version < 249)
        {
            throw UnsupportedError(fmt::format("SoundNodeWave layout of file version {} is not supported", version));
        }
        SoundWave sound;
        sound.name = object.package->object_name(object.index);
        const AssetProperties& p = source.properties;
        sound.duration = p.get_float("Duration").value_or(0.0f);
        sound.sample_rate = p.get_int("SampleRate").value_or(0);
        sound.channels = p.get_int("NumChannels").value_or(0);
        sound.spoken_text = p.get_string("SpokenText").value_or("");

        if (version < 380) // AddedPCSoundData
        {
            sound.file_type = read_fname(reader);
        }
        const bool native_channels = version >= 385 && version < 420;
        if (native_channels)
        {
            sound.channel_offsets = read_channel_table(reader);
            sound.channel_sizes = read_channel_table(reader);
        }
        sound.raw_data = version < 266 ? read_lazy_sound_data(reader) : read_bulk_data(reader);
        if (native_channels && version >= 390) sound.channels = reader.read_i32(); // AddedChannelCountSoundInfo
        if (version >= 380) sound.compressed_pc = read_bulk_data(reader);          // AddedPCSoundData
        if (version >= 327) sound.compressed_xbox360 = read_bulk_data(reader);     // AddedXenonSoundData
        if (version >= 376) sound.compressed_ps3 = read_bulk_data(reader);         // AddedPS3SoundData
        if (version >= 845) sound.compressed_wiiu = read_bulk_data(reader);   // AddedWiiUSoundData
        if (version >= 851) sound.compressed_iphone = read_bulk_data(reader); // AddedIPhoneSoundData
        if (version >= 854) sound.compressed_flash = read_bulk_data(reader);  // AddedFlashSoundData
        expect_end(reader, linker.qualified_path(object));
        sound.properties = source.properties;
        return sound;
    }

    std::optional<ExtractedSound> extract_sound(const Package& package, const SoundWave& sound)
    {
        const BulkData* payload = sound.best_payload();
        if (payload == nullptr)
        {
            return std::nullopt;
        }
        ExtractedSound out;
        out.bytes = load_bulk_data(package, *payload);
        out.container = detect_audio_container(out.bytes);
        return out;
    }
}
