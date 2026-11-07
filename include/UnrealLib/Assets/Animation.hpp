#pragma once

/// @file
/// @brief UE3 skeletal animation: AnimSet (bone tracks) and AnimSequence (compressed keys), decoding and ActorX PSA
///        export.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <UnrealLib/Assets/AssetObject.hpp>
#include <UnrealLib/Assets/MeshTypes.hpp>

namespace UnrealLib::Assets
{
    /// @brief AnimationKeyFormat (AKF_*).
    enum class AnimKeyFormat : std::uint8_t
    {
        ConstantKeyLerp,  ///< Keys evenly spaced over the sequence.
        VariableKeyLerp,  ///< Keys with frame times (not supported: unused by Mirror's Edge).
        PerTrackCompression, ///< Per-track formats (not supported: unused by Mirror's Edge).
        Unknown,
    };

    /// @brief AnimationCompressionFormat (ACF_*).
    enum class AnimCompression : std::uint8_t
    {
        None,             ///< Full precision (FVector / FQuat).
        Float96NoW,       ///< 3 floats, W restored.
        Fixed48NoW,       ///< 3 x uint16, W restored.
        IntervalFixed32NoW, ///< 11/11/10 bits in a range.
        Fixed32NoW,       ///< 11/11/10 bits.
        Float32NoW,       ///< Packed floats.
        Identity,         ///< No data (identity).
        Unknown,
    };

    /// @brief Parses an `AKF_*` enumerator name. Unknown names give AnimKeyFormat::Unknown.
    [[nodiscard]] AnimKeyFormat anim_key_format_from_name(std::string_view name) noexcept;

    /// @brief Parses an `ACF_*` enumerator name. Unknown names give AnimCompression::Unknown.
    [[nodiscard]] AnimCompression anim_compression_from_name(std::string_view name) noexcept;

    /// @brief Decoded keys of one bone.
    struct AnimTrack
    {
        std::vector<Vec3> positions; ///< Translation keys (empty: use the reference pose).
        std::vector<Quat> rotations; ///< Rotation keys (empty: use the reference pose).

        /// @brief Samples frame @p frame of @p num_frames: keys are evenly spaced (ConstantKeyLerp); positions are
        ///        lerped and rotations nlerped between neighbours. Empty tracks give the identity.
        void sample(std::int32_t frame, std::int32_t num_frames, Vec3& position, Quat& rotation) const;
    };

    /// @brief An AnimSequence export.
    ///
    /// Native data (file versions < 577): the CompressedByteStream (TArray<byte>). The track layout comes from the
    /// properties: CompressedTrackOffsets holds (translation offset, translation keys, rotation offset, rotation keys)
    /// per track. Below version 761 multi-key rotation tracks start with 24 bytes of mins/ranges. Tracks are aligned
    /// to 4 bytes. Format reference: UE Viewer.
    struct AnimSequence
    {
        std::string name;                   ///< Object name.
        std::string sequence_name;          ///< SequenceName property.
        std::int32_t num_frames = 0;        ///< NumFrames.
        float sequence_length = 0;          ///< SequenceLength (seconds).
        float rate_scale = 1;               ///< RateScale.
        bool additive = false;              ///< bIsAdditive.
        AnimKeyFormat key_format = AnimKeyFormat::ConstantKeyLerp; ///< KeyEncodingFormat.
        AnimCompression translation_format = AnimCompression::None; ///< TranslationCompressionFormat.
        AnimCompression rotation_format = AnimCompression::None;    ///< RotationCompressionFormat.
        std::vector<std::int32_t> track_offsets; ///< CompressedTrackOffsets.
        std::vector<std::uint8_t> stream;   ///< CompressedByteStream.
        std::vector<AnimTrack> tracks;      ///< Decoded tracks (one per AnimSet bone).
        std::size_t stream_bytes_used = 0;  ///< Bytes covered by the decoded tracks (with alignment).
        AssetProperties properties;         ///< Tagged properties.

        /// @brief Frames per second (NumFrames / SequenceLength * RateScale; 30 if the length is 0).
        [[nodiscard]] float frame_rate() const noexcept;
    };

    /// @brief Reads and decodes an AnimSequence export. The native data must be consumed exactly and every track must
    ///        decode inside the byte stream.
    /// @param schema Needed for CompressedTrackOffsets only if its array is not decodable without one; the reader
    ///        also accepts the raw int32 elements.
    [[nodiscard]] AnimSequence read_anim_sequence(Linker& linker, ObjectRef object,
                                                  const PropertySchema* schema = nullptr);

    /// @brief Decodes @p sequence.stream into @p sequence.tracks (called by read_anim_sequence()).
    /// @throws UnsupportedError for key formats/codecs other than ConstantKeyLerp with None, Float96NoW, Fixed48NoW,
    ///         Fixed32NoW or Identity; SerializationError if a track leaves the stream.
    void decode_anim_tracks(AnimSequence& sequence, bool ranges_before_rotations);

    /// @brief An AnimSet export: the bones its sequences animate.
    struct AnimSet
    {
        std::string name;                         ///< Object name.
        std::vector<std::string> track_bone_names; ///< One per track, in track order.
        std::vector<ObjectIndex> sequences;       ///< Sequences property.
        bool rotation_only = false;               ///< bAnimRotationOnly.
        AssetProperties properties;               ///< Tagged properties.
    };

    /// @brief Reads an AnimSet export, or one of a class derived from it (Mirror's Edge: TdAnimSet). Properties only:
    ///        there is no native data at version 536.
    [[nodiscard]] AnimSet read_anim_set(Linker& linker, ObjectRef object, const PropertySchema* schema = nullptr);

    /// @brief Writes an ActorX PSA file with @p sequences, sampled once per frame. Like UE Viewer, positions and
    ///        rotations are mirrored on Y (rotation Y and W negated) to match to_psk().
    [[nodiscard]] std::vector<std::byte> to_psa(const AnimSet& set, std::span<const AnimSequence> sequences);
}
