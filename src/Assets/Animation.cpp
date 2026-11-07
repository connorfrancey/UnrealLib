#include <UnrealLib/Assets/Animation.hpp>

#include <algorithm>
#include <array>
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

    void AnimTrack::sample(std::int32_t frame, std::int32_t num_frames, Vec3& position, Quat& rotation) const
    {
        const auto at = [&](std::size_t keys, std::size_t& i0, std::size_t& i1, float& t) {
            i0 = i1 = 0;
            t = 0;
            if (keys <= 1 || num_frames <= 1) return;
            const float pos = static_cast<float>(frame) * static_cast<float>(keys - 1) / static_cast<float>(num_frames - 1);
            i0 = std::min(keys - 1, static_cast<std::size_t>(pos));
            i1 = std::min(keys - 1, i0 + 1);
            t = pos - static_cast<float>(i0);
        };
        std::size_t i0 = 0, i1 = 0;
        float t = 0;
        position = {};
        rotation = {};
        if (!positions.empty())
        {
            at(positions.size(), i0, i1, t);
            const Vec3& a = positions[i0];
            const Vec3& b = positions[i1];
            position = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        }
        if (!rotations.empty())
        {
            at(rotations.size(), i0, i1, t);
            rotation = t == 0 ? rotations[i0] : nlerp(rotations[i0], rotations[i1], t);
        }
    }

    float AnimSequence::frame_rate() const noexcept
    {
        return sequence_length > 0 ? static_cast<float>(num_frames) / sequence_length * rate_scale : 30.0f;
    }

    void decode_anim_tracks(AnimSequence& seq, bool ranges_before_rotations)
    {
        if (seq.key_format != AnimKeyFormat::ConstantKeyLerp)
        {
            throw UnsupportedError("only AKF_ConstantKeyLerp animations are supported");
        }
        if (seq.track_offsets.size() % 4 != 0)
        {
            throw SerializationError(fmt::format("CompressedTrackOffsets has {} entries, not a multiple of 4",
                                                 seq.track_offsets.size()));
        }
        BinaryReader r(std::as_bytes(std::span<const std::uint8_t>(seq.stream)));
        seq.tracks.assign(seq.track_offsets.size() / 4, AnimTrack{});
        seq.stream_bytes_used = 0;
        const auto check_offset = [&](std::int32_t offset, const char* what) {
            if (offset < 0 || static_cast<std::size_t>(offset) > seq.stream.size())
            {
                throw SerializationError(fmt::format("{} offset {} outside the {}-byte stream", what, offset,
                                                     seq.stream.size()));
            }
        };
        for (std::size_t t = 0; t < seq.tracks.size(); ++t)
        {
            AnimTrack& track = seq.tracks[t];
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
                check_offset(trans_offset, "translation");
                r.seek(static_cast<std::size_t>(trans_offset));
                const AnimCompression format = trans_keys == 1 ? AnimCompression::None : seq.translation_format;
                for (std::int32_t k = 0; k < trans_keys; ++k)
                {
                    switch (format)
                    {
                        case AnimCompression::None:
                        case AnimCompression::Float96NoW:
                            track.positions.push_back(read_vec3(r));
                            break;
                        case AnimCompression::Identity:
                            track.positions.push_back({});
                            break;
                        default:
                            throw UnsupportedError("unsupported translation compression");
                    }
                }
                seq.stream_bytes_used = std::max(seq.stream_bytes_used, align4(r.position()));
            }
            if (rot_keys > 0)
            {
                check_offset(rot_offset, "rotation");
                r.seek(static_cast<std::size_t>(rot_offset));
                AnimCompression format = seq.rotation_format;
                if (rot_keys == 1)
                {
                    format = AnimCompression::Float96NoW; // a single key is stored uncompressed
                }
                else if (ranges_before_rotations || format == AnimCompression::IntervalFixed32NoW)
                {
                    (void)read_vec3(r); // mins (unused by these codecs)
                    (void)read_vec3(r); // ranges
                }
                for (std::int32_t k = 0; k < rot_keys; ++k)
                {
                    switch (format)
                    {
                        case AnimCompression::None:
                            track.rotations.push_back(read_quat(r));
                            break;
                        case AnimCompression::Float96NoW:
                        {
                            const float x = r.read_f32(), y = r.read_f32(), z = r.read_f32();
                            track.rotations.push_back(restore_w(x, y, z));
                            break;
                        }
                        case AnimCompression::Fixed48NoW:
                        {
                            const auto c = [&] { return (static_cast<float>(r.read_u16()) - 32767.0f) / 32767.0f; };
                            const float x = c(), y = c(), z = c();
                            track.rotations.push_back(restore_w(x, y, z));
                            break;
                        }
                        case AnimCompression::Fixed32NoW:
                        {
                            const std::uint32_t v = r.read_u32(); // Z:10, Y:11, X:11 (low to high)
                            const float z = static_cast<float>(v & 0x3FF) / 511.0f - 1.0f;
                            const float y = static_cast<float>((v >> 10) & 0x7FF) / 1023.0f - 1.0f;
                            const float x = static_cast<float>((v >> 21) & 0x7FF) / 1023.0f - 1.0f;
                            track.rotations.push_back(restore_w(x, y, z));
                            break;
                        }
                        case AnimCompression::Identity:
                            track.rotations.push_back({});
                            break;
                        default:
                            throw UnsupportedError("unsupported rotation compression");
                    }
                }
                seq.stream_bytes_used = std::max(seq.stream_bytes_used, align4(r.position()));
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
        if (version < 372 || version >= 577)
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
