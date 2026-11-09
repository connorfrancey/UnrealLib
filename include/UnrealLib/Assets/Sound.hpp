#pragma once

/// @file
/// @brief UE3 SoundNodeWave: sound metadata and payload extraction.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <UnrealLib/Assets/AssetObject.hpp>
#include <UnrealLib/Assets/BulkData.hpp>

namespace UnrealLib::Assets
{
    /// @brief Container format of a sound payload, detected from its first bytes.
    enum class AudioContainer : std::uint8_t
    {
        Unknown, ///< Unrecognized bytes.
        Wave,    ///< RIFF/WAVE (uncooked raw data).
        Ogg,     ///< Ogg (Vorbis): the PC cooked format.
        Xma,     ///< Xbox 360 XMA (RIFF with XMA format tag).
    };

    /// @brief File extension for @p container ("wav", "ogg", "xma", "bin").
    [[nodiscard]] std::string_view file_extension(AudioContainer container) noexcept;

    /// @brief Detects the container of @p bytes.
    [[nodiscard]] AudioContainer detect_audio_container(std::span<const std::byte> bytes) noexcept;

    /// @brief A SoundNodeWave export.
    ///
    /// Native layout (file versions 380..844, verified on every Mirror's Edge sound): bulk data RawData,
    /// CompressedPCData, CompressedXbox360Data, CompressedPS3Data.
    struct SoundWave
    {
        std::string name;                  ///< Object name.
        float duration = 0.0f;             ///< Duration property (seconds).
        std::int32_t sample_rate = 0;      ///< SampleRate property (0 if absent).
        std::int32_t channels = 0;         ///< NumChannels property (0 if absent).
        std::string spoken_text;           ///< SpokenText property (subtitles source).
        BulkData raw_data;                 ///< Uncooked source (RIFF WAVE).
        BulkData compressed_pc;            ///< PC cooked data (Ogg Vorbis).
        BulkData compressed_xbox360;       ///< Xbox 360 cooked data.
        BulkData compressed_ps3;           ///< PS3 cooked data.
        BulkData compressed_wiiu;          ///< Wii U cooked data (file version >= 845).
        BulkData compressed_iphone;        ///< iPhone cooked data (file version >= 851).
        BulkData compressed_flash;         ///< Flash cooked data (file version >= 854).
        AssetProperties properties;        ///< All tagged properties.

        /// @brief The record extract_sound() uses: RawData if present, else the PC, Xbox 360 or PS3 data.
        [[nodiscard]] const BulkData* best_payload() const noexcept;
    };

    /// @brief Reads a SoundNodeWave export. The native data must be consumed exactly.
    [[nodiscard]] SoundWave read_sound_wave(Linker& linker, ObjectRef object, const PropertySchema* schema = nullptr);

    /// @brief An extracted sound file.
    struct ExtractedSound
    {
        AudioContainer container = AudioContainer::Unknown; ///< Detected format.
        std::vector<std::byte> bytes;                       ///< Complete file contents.
    };

    /// @brief Loads the best payload of @p sound (see SoundWave::best_payload()) as a complete file.
    /// @return std::nullopt if the sound has no stored payload.
    [[nodiscard]] std::optional<ExtractedSound> extract_sound(const Package& package, const SoundWave& sound);
}
