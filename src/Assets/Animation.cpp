#include <UnrealLib/Assets/Animation.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Utilities/Text.hpp>

#include "MeshReading.hpp"

namespace UnrealLib::Assets
{
    using namespace detail;

    AnimKeyFormat anim_key_format_from_name(std::string_view name) noexcept
    {
        if (Text::iequals(name, "AKF_ConstantKeyLerp")) return AnimKeyFormat::ConstantKeyLerp;
        if (Text::iequals(name, "AKF_VariableKeyLerp")) return AnimKeyFormat::VariableKeyLerp;
        if (Text::iequals(name, "AKF_PerTrackCompression")) return AnimKeyFormat::PerTrackCompression;
        return AnimKeyFormat::Unknown;
    }

    AnimCompression anim_compression_from_name(std::string_view name) noexcept
    {
        static constexpr std::array<std::pair<std::string_view, AnimCompression>, 7> names{{
            {"ACF_None", AnimCompression::None},
            {"ACF_Float96NoW", AnimCompression::Float96NoW},
            {"ACF_Fixed48NoW", AnimCompression::Fixed48NoW},
            {"ACF_IntervalFixed32NoW", AnimCompression::IntervalFixed32NoW},
            {"ACF_Fixed32NoW", AnimCompression::Fixed32NoW},
            {"ACF_Float32NoW", AnimCompression::Float32NoW},
            {"ACF_Identity", AnimCompression::Identity},
        }};
        for (const auto& [n, v] : names)
        {
            if (Text::iequals(n, name)) return v;
        }
        return AnimCompression::Unknown;
    }

    namespace
    {
        Quat restore_w(float x, float y, float z) noexcept
        {
            const float w2 = 1.0f - (x * x + y * y + z * z);
            return {x, y, z, w2 > 0.0f ? std::sqrt(w2) : 0.0f};
        }

        std::size_t align4(std::size_t v) noexcept { return (v + 3) & ~std::size_t{3}; }

        /// Elements of an `array<int>` property, decoded or raw.
        std::vector<std::int32_t> int_array(const AssetProperties& p, std::string_view name)
        {
            std::vector<std::int32_t> out;
            const Property* prop = p.find(name);
            const auto* array = prop ? std::get_if<ArrayValue>(&prop->value.value) : nullptr;
            if (array == nullptr) return out;
            if (array->decoded())
            {
                for (const PropertyValue& e : array->elements)
                {
                    const auto* v = std::get_if<std::int32_t>(&e.value);
                    if (v == nullptr) throw SerializationError(fmt::format("{} is not an array of int", name));
                    out.push_back(*v);
                }
                return out;
            }
            if (array->raw->size() != static_cast<std::size_t>(array->count) * 4)
            {
                throw SerializationError(fmt::format("{}: {} raw bytes for {} ints", name, array->raw->size(), array->count));
            }
            BinaryReader r(*array->raw);
            for (std::int32_t i = 0; i < array->count; ++i) out.push_back(r.read_i32());
            return out;
        }

        /// Elements of an `array<name>` property, decoded or raw (8-byte FNames).
        std::vector<std::string> name_array(const Package& package, const AssetProperties& p, std::string_view name)
        {
            std::vector<std::string> out;
            const Property* prop = p.find(name);
            const auto* array = prop ? std::get_if<ArrayValue>(&prop->value.value) : nullptr;
            if (array == nullptr) return out;
            if (array->decoded())
            {
                for (const PropertyValue& e : array->elements)
                {
                    const auto* v = std::get_if<NameValue>(&e.value);
                    if (v == nullptr) throw SerializationError(fmt::format("{} is not an array of names", name));
                    out.push_back(package.names().to_string(v->value));
                }
                return out;
            }
            if (array->raw->size() != static_cast<std::size_t>(array->count) * 8)
            {
                throw SerializationError(fmt::format("{}: {} raw bytes for {} names", name, array->raw->size(), array->count));
            }
            BinaryReader r(*array->raw, package.context());
            for (std::int32_t i = 0; i < array->count; ++i) out.push_back(package.names().to_string(read_fname(r)));
            return out;
        }

        std::vector<ObjectIndex> object_array(const AssetProperties& p, std::string_view name)
        {
            std::vector<ObjectIndex> out;
            const Property* prop = p.find(name);
            const auto* array = prop ? std::get_if<ArrayValue>(&prop->value.value) : nullptr;
            if (array != nullptr && array->decoded())
            {
                for (const PropertyValue& e : array->elements)
                {
                    const auto* v = std::get_if<ObjectValue>(&e.value);
                    if (v == nullptr) throw SerializationError(fmt::format("{} is not an array of objects", name));
                    out.push_back(v->value);
                }
                return out;
            }
            for (const std::int32_t v : int_array(p, name)) out.push_back(ObjectIndex{v});
            return out;
        }

        Quat nlerp(const Quat& a, Quat b, float t) noexcept
        {
            if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
            Quat q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
            const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (len > 0) q = {q.x / len, q.y / len, q.z / len, q.w / len};
            return q;
        }
    }

    namespace
    {
        /// Neighbouring keys @p i0 / @p i1 and blend @p t of frame @p frame among @p keys keys, evenly spaced over
        /// @p num_frames or at the key frames @p frames.
        void locate_key(std::size_t keys, const std::vector<float>& frames, std::int32_t frame, std::int32_t num_frames,
                        std::size_t& i0, std::size_t& i1, float& t)
        {
            i0 = i1 = 0;
            t = 0;
            if (keys <= 1) return;
            const float f = static_cast<float>(frame);
            if (frames.size() == keys)
            {
                const auto next = std::upper_bound(frames.begin(), frames.end(), f);
                if (next == frames.begin()) return;
                if (next == frames.end())
                {
                    i0 = i1 = keys - 1;
                    return;
                }
                i1 = static_cast<std::size_t>(next - frames.begin());
                i0 = i1 - 1;
                const float span = frames[i1] - frames[i0];
                t = span > 0 ? (f - frames[i0]) / span : 0.0f;
                return;
            }
            if (num_frames <= 1) return;
            const float pos = f * static_cast<float>(keys - 1) / static_cast<float>(num_frames - 1);
            i0 = std::min(keys - 1, static_cast<std::size_t>(pos));
            i1 = std::min(keys - 1, i0 + 1);
            t = pos - static_cast<float>(i0);
        }
    }

    void AnimTrack::sample(std::int32_t frame, std::int32_t num_frames, Vec3& position, Quat& rotation) const
    {
        std::size_t i0 = 0, i1 = 0;
        float t = 0;
        position = {};
        rotation = {};
        if (!positions.empty())
        {
            locate_key(positions.size(), position_frames, frame, num_frames, i0, i1, t);
            const Vec3& a = positions[i0];
            const Vec3& b = positions[i1];
            position = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        }
        if (!rotations.empty())
        {
            locate_key(rotations.size(), rotation_frames, frame, num_frames, i0, i1, t);
            rotation = t == 0 ? rotations[i0] : nlerp(rotations[i0], rotations[i1], t);
        }
    }

    float AnimSequence::frame_rate() const noexcept
    {
        return sequence_length > 0 ? static_cast<float>(num_frames) / sequence_length * rate_scale : 30.0f;
    }

    namespace
    {
        // Key codecs (AnimationCompressionFormat). Layouts follow UE Viewer (UnAnim3.cpp / UnMathTools.h).

        /// Mins/ranges of an IntervalFixed32NoW track.
        struct KeyBounds
        {
            Vec3 mins{};
            Vec3 ranges{};
        };

        float fixed11(std::uint32_t v) noexcept { return static_cast<float>(v) / 1023.0f - 1.0f; }
        float fixed10(std::uint32_t v) noexcept { return static_cast<float>(v) / 511.0f - 1.0f; }

        /// Per-track Fixed48 component: 16 bits around 32767, scaled by 2^log2.
        float fixed16(std::uint16_t v, int log2) noexcept
        {
            return (static_cast<float>(v) - 32767.0f) / 32767.0f * static_cast<float>(1 << log2);
        }

        /// One component of FQuatFloat32NoW: sign, 3-bit exponent (bias 123) and @p mantissa_bits mantissa bits.
        float packed_float(std::uint32_t v, int mantissa_bits) noexcept
        {
            const std::uint32_t mantissa = v & ((1u << mantissa_bits) - 1);
            const std::uint32_t exponent = ((v >> mantissa_bits) & 7u) + 123u;
            const std::uint32_t sign = (v >> (mantissa_bits + 3)) & 1u;
            return std::bit_cast<float>(sign << 31 | exponent << 23 | mantissa << (23 - mantissa_bits));
        }

        /// Component mask of a key: bit 0 X, bit 1 Y, bit 2 Z. Zero means all three (UE3 writes uncompressed
        /// tracks with an empty mask).
        unsigned effective_mask(unsigned mask) noexcept { return (mask & 7u) == 0 ? 7u : (mask & 7u); }

        Vec3 read_vector_key(BinaryReader& r, AnimCompression format, unsigned mask, const KeyBounds& bounds)
        {
            switch (format)
            {
                case AnimCompression::None:
                case AnimCompression::Float96NoW:
                {
                    Vec3 v{};
                    mask = effective_mask(mask);
                    if (mask & 1u) v.x = r.read_f32();
                    if (mask & 2u) v.y = r.read_f32();
                    if (mask & 4u) v.z = r.read_f32();
                    return v;
                }
                case AnimCompression::Fixed48NoW:
                {
                    Vec3 v{};
                    mask = effective_mask(mask);
                    if (mask & 1u) v.x = fixed16(r.read_u16(), 7);
                    if (mask & 2u) v.y = fixed16(r.read_u16(), 7);
                    if (mask & 4u) v.z = fixed16(r.read_u16(), 7);
                    return v;
                }
                case AnimCompression::IntervalFixed32NoW:
                {
                    const std::uint32_t v = r.read_u32(); // X:10, Y:11, Z:11 (low to high)
                    return {fixed10(v & 0x3FF) * bounds.ranges.x + bounds.mins.x,
                            fixed11((v >> 10) & 0x7FF) * bounds.ranges.y + bounds.mins.y,
                            fixed11(v >> 21) * bounds.ranges.z + bounds.mins.z};
                }
                case AnimCompression::Identity:
                    return {};
                default:
                    throw UnsupportedError(fmt::format("translation codec {} is not supported",
                                                       static_cast<int>(format)));
            }
        }

        Quat read_quat_key(BinaryReader& r, AnimCompression format, unsigned mask, const KeyBounds& bounds)
        {
            switch (format)
            {
                case AnimCompression::None:
                    return read_quat(r);
                case AnimCompression::Float96NoW:
                {
                    float c[3]{};
                    mask = effective_mask(mask);
                    for (int i = 0; i < 3; ++i)
                    {
                        if (mask & (1u << i)) c[i] = r.read_f32();
                    }
                    return restore_w(c[0], c[1], c[2]);
                }
                case AnimCompression::Fixed48NoW:
                {
                    float c[3]{};
                    mask = effective_mask(mask);
                    for (int i = 0; i < 3; ++i)
                    {
                        if (mask & (1u << i)) c[i] = fixed16(r.read_u16(), 0);
                    }
                    return restore_w(c[0], c[1], c[2]);
                }
                case AnimCompression::IntervalFixed32NoW:
                {
                    const std::uint32_t v = r.read_u32(); // Z:10, Y:11, X:11 (low to high)
                    return restore_w(fixed11(v >> 21) * bounds.ranges.x + bounds.mins.x,
                                     fixed11((v >> 10) & 0x7FF) * bounds.ranges.y + bounds.mins.y,
                                     fixed10(v & 0x3FF) * bounds.ranges.z + bounds.mins.z);
                }
                case AnimCompression::Fixed32NoW:
                {
                    const std::uint32_t v = r.read_u32(); // Z:10, Y:11, X:11 (low to high)
                    return restore_w(fixed11(v >> 21), fixed11((v >> 10) & 0x7FF), fixed10(v & 0x3FF));
                }
                case AnimCompression::Float32NoW:
                {
                    const std::uint32_t v = r.read_u32(); // Z:10, Y:11, X:11 (low to high)
                    return restore_w(packed_float(v >> 21, 7), packed_float((v >> 10) & 0x7FF, 7),
                                     packed_float(v & 0x3FF, 6));
                }
                case AnimCompression::Identity:
                    return {};
                default:
                    throw UnsupportedError(fmt::format("rotation codec {} is not supported", static_cast<int>(format)));
            }
        }

        void skip_padding(BinaryReader& r)
        {
            r.seek(std::min(align4(r.position()), r.size()));
        }

        /// Frame table of a variable-key track: one byte per key below 256 frames, else uint16; padded to 4 bytes.
        void read_key_frames(BinaryReader& r, std::size_t keys, std::int32_t num_frames, std::vector<float>& out)
        {
            if (keys <= 1) return;
            out.reserve(std::min<std::size_t>(keys, r.remaining()));
            for (std::size_t k = 0; k < keys; ++k)
            {
                out.push_back(num_frames < 256 ? static_cast<float>(r.read_u8()) : static_cast<float>(r.read_u16()));
            }
            skip_padding(r);
        }

        /// Keys of a ConstantKeyLerp/VariableKeyLerp track (key count from CompressedTrackOffsets).
        template <typename Key, typename Read>
        void read_keys(BinaryReader& r, std::size_t keys, std::vector<Key>& out, Read read)
        {
            out.reserve(std::min<std::size_t>(keys, r.remaining()));
            for (std::size_t k = 0; k < keys; ++k) out.push_back(read());
        }

        bool is_codec(AnimCompression format) noexcept
        {
            return format <= AnimCompression::Identity;
        }
    }

    void decode_anim_tracks(AnimSequence& seq, bool ranges_before_rotations)
    {
        const AnimKeyFormat key_format = seq.key_format;
        if (key_format != AnimKeyFormat::ConstantKeyLerp && key_format != AnimKeyFormat::VariableKeyLerp &&
            key_format != AnimKeyFormat::PerTrackCompression)
        {
            throw UnsupportedError("unknown AnimationKeyFormat");
        }
        const bool per_track = key_format == AnimKeyFormat::PerTrackCompression;
        const std::size_t stride = per_track ? 2 : 4;
        if (seq.track_offsets.size() % stride != 0)
        {
            throw SerializationError(fmt::format("CompressedTrackOffsets has {} entries, not a multiple of {}",
                                                 seq.track_offsets.size(), stride));
        }
        BinaryReader r(std::as_bytes(std::span<const std::uint8_t>(seq.stream)));
        seq.tracks.assign(seq.track_offsets.size() / stride, AnimTrack{});
        seq.stream_bytes_used = 0;
        const auto seek_to = [&](std::int32_t offset, const char* what) {
            if (offset < 0 || static_cast<std::size_t>(offset) > seq.stream.size())
            {
                throw SerializationError(fmt::format("{} offset {} outside the {}-byte stream", what, offset,
                                                     seq.stream.size()));
            }
            r.seek(static_cast<std::size_t>(offset));
        };
        const auto track_done = [&] { seq.stream_bytes_used = std::max(seq.stream_bytes_used, align4(r.position())); };

        for (std::size_t t = 0; t < seq.tracks.size(); ++t)
        {
            AnimTrack& track = seq.tracks[t];
            if (per_track)
            {
                for (int channel = 0; channel < 2; ++channel)
                {
                    const bool translation = channel == 0;
                    const std::int32_t offset = seq.track_offsets[t * 2 + static_cast<std::size_t>(channel)];
                    if (offset == -1) // identity track
                    {
                        if (translation) track.positions.push_back({});
                        else track.rotations.push_back({});
                        continue;
                    }
                    seek_to(offset, translation ? "translation" : "rotation");
                    const std::uint32_t header = r.read_u32();
                    const auto format = static_cast<AnimCompression>(header >> 28);
                    const unsigned flags = (header >> 24) & 0xFu;
                    const std::size_t keys = header & 0xFFFFFFu;
                    if (!is_codec(format))
                    {
                        throw UnsupportedError(fmt::format("per-track codec {} is not supported", header >> 28));
                    }
                    KeyBounds bounds;
                    if (format == AnimCompression::IntervalFixed32NoW)
                    {
                        float* mins[3] = {&bounds.mins.x, &bounds.mins.y, &bounds.mins.z};
                        float* ranges[3] = {&bounds.ranges.x, &bounds.ranges.y, &bounds.ranges.z};
                        for (int i = 0; i < 3; ++i)
                        {
                            if (flags & (1u << i))
                            {
                                *mins[i] = r.read_f32();
                                *ranges[i] = r.read_f32();
                            }
                        }
                    }
                    if (translation)
                    {
                        read_keys(r, keys, track.positions, [&] { return read_vector_key(r, format, flags, bounds); });
                    }
                    else
                    {
                        read_keys(r, keys, track.rotations, [&] { return read_quat_key(r, format, flags, bounds); });
                    }
                    skip_padding(r);
                    if (flags & 8u)
                    {
                        read_key_frames(r, keys, seq.num_frames,
                                        translation ? track.position_frames : track.rotation_frames);
                    }
                    track_done();
                }
                continue;
            }

            const bool variable = key_format == AnimKeyFormat::VariableKeyLerp;
            const std::int32_t trans_offset = seq.track_offsets[t * 4];
            const std::int32_t trans_keys = seq.track_offsets[t * 4 + 1];
            const std::int32_t rot_offset = seq.track_offsets[t * 4 + 2];
            const std::int32_t rot_keys = seq.track_offsets[t * 4 + 3];
            if (trans_keys < 0 || rot_keys < 0)
            {
                throw SerializationError("negative key count");
            }
            if (trans_keys > 0)
            {
                seek_to(trans_offset, "translation");
                const AnimCompression format = trans_keys == 1 ? AnimCompression::None : seq.translation_format;
                KeyBounds bounds;
                if (format == AnimCompression::IntervalFixed32NoW)
                {
                    bounds.mins = read_vec3(r);
                    bounds.ranges = read_vec3(r);
                }
                read_keys(r, static_cast<std::size_t>(trans_keys), track.positions,
                          [&] { return read_vector_key(r, format, 7, bounds); });
                skip_padding(r);
                if (variable) read_key_frames(r, track.positions.size(), seq.num_frames, track.position_frames);
                track_done();
            }
            if (rot_keys > 0)
            {
                seek_to(rot_offset, "rotation");
                // A single key is stored uncompressed.
                const AnimCompression format = rot_keys == 1 ? AnimCompression::Float96NoW : seq.rotation_format;
                KeyBounds bounds;
                if (rot_keys > 1 && (ranges_before_rotations || format == AnimCompression::IntervalFixed32NoW))
                {
                    bounds.mins = read_vec3(r);
                    bounds.ranges = read_vec3(r);
                }
                read_keys(r, static_cast<std::size_t>(rot_keys), track.rotations,
                          [&] { return read_quat_key(r, format, 7, bounds); });
                skip_padding(r);
                if (variable) read_key_frames(r, track.rotations.size(), seq.num_frames, track.rotation_frames);
                track_done();
            }
        }
    }

    AnimSequence read_anim_sequence(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        const AssetSource source = open_asset(linker, object, schema);
        if (!Text::iequals(source.class_name, "AnimSequence"))
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("'{}' is a {}, not an AnimSequence",
                                                                linker.qualified_path(object), source.class_name));
        }
        BinaryReader r = source.native_reader();
        const std::uint16_t version = r.context().file_version;
        if (version < 372)
        {
            throw UnsupportedError(fmt::format("AnimSequence layout of file version {} is not supported", version));
        }
        const AssetProperties& p = source.properties;
        AnimSequence seq;
        seq.name = object.package->object_name(object.index);
        seq.sequence_name = p.get_name("SequenceName").value_or(seq.name);
        seq.num_frames = p.get_int("NumFrames").value_or(0);
        seq.sequence_length = p.get_float("SequenceLength").value_or(0.0f);
        seq.rate_scale = p.get_float("RateScale").value_or(1.0f);
        seq.additive = p.get_bool("bIsAdditive").value_or(false);
        seq.key_format = anim_key_format_from_name(p.get_name("KeyEncodingFormat").value_or("AKF_ConstantKeyLerp"));
        seq.translation_format =
            anim_compression_from_name(p.get_name("TranslationCompressionFormat").value_or("ACF_None"));
        seq.rotation_format = anim_compression_from_name(p.get_name("RotationCompressionFormat").value_or("ACF_None"));
        seq.track_offsets = int_array(p, "CompressedTrackOffsets");

        if (version >= 577) // RawAnimData: TArray<FRawAnimSequenceTrack>, key arrays bulk-serialized
        {
            const std::uint32_t tracks = r.read_array_count(16);
            seq.raw_tracks.resize(tracks);
            const auto bulk_count = [&](std::int32_t element_size, const char* what) {
                const std::int32_t stored = r.read_i32();
                if (stored != element_size)
                {
                    throw SerializationError(fmt::format("RawAnimData {} keys of {} bytes, expected {}", what, stored,
                                                         element_size));
                }
                return r.read_array_count(static_cast<std::size_t>(element_size));
            };
            for (AnimTrack& track : seq.raw_tracks)
            {
                const std::uint32_t positions = bulk_count(12, "position");
                track.positions.reserve(positions);
                for (std::uint32_t k = 0; k < positions; ++k) track.positions.push_back(read_vec3(r));
                const std::uint32_t rotations = bulk_count(16, "rotation");
                track.rotations.reserve(rotations);
                for (std::uint32_t k = 0; k < rotations; ++k) track.rotations.push_back(read_quat(r));
            }
        }

        const std::uint32_t size = r.read_array_count(1);
        const auto bytes = r.read_view(size);
        seq.stream.resize(size);
        for (std::size_t i = 0; i < size; ++i) seq.stream[i] = std::to_integer<std::uint8_t>(bytes[i]);
        expect_end(r, linker.qualified_path(object));

        decode_anim_tracks(seq, version < 761);
        seq.properties = source.properties;
        return seq;
    }

    AnimSet read_anim_set(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        const AssetSource source = open_asset(linker, object, schema);
        if (!Text::iequals(source.class_name, "AnimSet") && !linker.is_a(object, "Engine.AnimSet").value_or(false))
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("'{}' is a {}, not an AnimSet",
                                                                linker.qualified_path(object), source.class_name));
        }
        BinaryReader r = source.native_reader();
        expect_end(r, linker.qualified_path(object));
        AnimSet set;
        set.name = object.package->object_name(object.index);
        set.track_bone_names = name_array(*object.package, source.properties, "TrackBoneNames");
        set.sequences = object_array(source.properties, "Sequences");
        set.rotation_only = source.properties.get_bool("bAnimRotationOnly").value_or(false);
        set.properties = source.properties;
        return set;
    }

    namespace
    {
        void psa_chunk(BinaryWriter& w, std::string_view id, std::int32_t size, std::int32_t count)
        {
            for (std::size_t i = 0; i < 20; ++i) w.write_u8(i < id.size() ? static_cast<std::uint8_t>(id[i]) : 0);
            w.write_u32(20100422);
            w.write_i32(size);
            w.write_i32(count);
        }

        void psa_string(BinaryWriter& w, std::string_view text)
        {
            for (std::size_t i = 0; i < 64; ++i)
            {
                w.write_u8(i < 63 && i < text.size() ? static_cast<std::uint8_t>(text[i]) : std::uint8_t{0});
            }
        }
    }

    std::vector<std::byte> to_psa(const AnimSet& set, std::span<const AnimSequence> sequences)
    {
        const std::size_t bones = set.track_bone_names.size();
        for (const AnimSequence& s : sequences)
        {
            if (s.tracks.size() != bones)
            {
                throw Error(ErrorCode::InvalidArgument, fmt::format("sequence '{}' has {} tracks, the set {} bones",
                                                                    s.sequence_name, s.tracks.size(), bones));
            }
        }
        BinaryWriter w;
        psa_chunk(w, "ANIMHEAD", 0, 0);
        psa_chunk(w, "BONENAMES", 120, static_cast<std::int32_t>(bones));
        for (std::size_t b = 0; b < bones; ++b)
        {
            psa_string(w, set.track_bone_names[b]);
            w.write_i32(0);                         // flags
            w.write_i32(0);                         // children (unknown)
            w.write_i32(b == 0 ? -1 : 0);           // parent (unknown for an AnimSet)
            for (int i = 0; i < 3; ++i) w.write_f32(0.0f);
            w.write_f32(1.0f);                      // orientation (identity)
            for (int i = 0; i < 3; ++i) w.write_f32(0.0f);
            w.write_f32(1.0f);                      // length
            for (int i = 0; i < 3; ++i) w.write_f32(0.0f);
        }
        psa_chunk(w, "ANIMINFO", 168, static_cast<std::int32_t>(sequences.size()));
        std::int32_t first_frame = 0;
        for (const AnimSequence& s : sequences)
        {
            psa_string(w, s.sequence_name);
            psa_string(w, "None");
            w.write_i32(static_cast<std::int32_t>(bones));
            w.write_i32(0);
            w.write_i32(0);
            w.write_i32(s.num_frames * static_cast<std::int32_t>(bones));
            w.write_f32(0.0f);
            w.write_f32(static_cast<float>(s.num_frames));
            w.write_f32(s.frame_rate());
            w.write_i32(0);
            w.write_i32(first_frame);
            w.write_i32(s.num_frames);
            first_frame += s.num_frames;
        }
        psa_chunk(w, "ANIMKEYS", 32, first_frame * static_cast<std::int32_t>(bones));
        for (const AnimSequence& s : sequences)
        {
            for (std::int32_t f = 0; f < s.num_frames; ++f)
            {
                for (std::size_t b = 0; b < bones; ++b)
                {
                    Vec3 p;
                    Quat q;
                    s.tracks[b].sample(f, s.num_frames, p, q);
                    w.write_f32(p.x);
                    w.write_f32(-p.y);
                    w.write_f32(p.z);
                    w.write_f32(q.x);
                    w.write_f32(-q.y);
                    w.write_f32(q.z);
                    w.write_f32(-q.w);
                    w.write_f32(1.0f); // time
                }
            }
        }
        return w.take();
    }
}
