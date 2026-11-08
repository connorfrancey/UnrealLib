#include <UnrealLib/Assets/Sound.hpp>

#include <cstring>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib::Assets
{
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
        if (version < 380)
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

        if (version >= 385 && version < 420) // AddedChannelsSoundInfo .. DisplacedSoundChannelProperties
        {
            throw UnsupportedError("SoundNodeWave channel tables (versions 385-419) are not supported");
        }
        sound.raw_data = read_bulk_data(reader);
        sound.compressed_pc = read_bulk_data(reader);
        sound.compressed_xbox360 = read_bulk_data(reader);
        sound.compressed_ps3 = read_bulk_data(reader);
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
