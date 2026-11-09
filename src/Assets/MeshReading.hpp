#pragma once

// Internal helpers for mesh/animation readers.

#include <cstdint>
#include <string_view>

#include <fmt/format.h>

#include <UnrealLib/Assets/MeshTypes.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>

namespace UnrealLib::Assets::detail
{
    /// Reads a UE3 bulk array header (version >= 453): int32 element size, int32 count. Checks the element size
    /// against @p expected (0 = accept any) and that count * size bytes remain. Returns {count, element size}.
    struct BulkArrayHeader
    {
        std::uint32_t count = 0;
        std::uint32_t element_size = 0;
    };

    inline BulkArrayHeader read_bulk_array_header(BinaryReader& reader, std::uint32_t expected, std::string_view what)
    {
        const std::uint64_t at = reader.absolute_position();
        const std::int32_t size = reader.read_i32();
        const std::int32_t count = reader.read_i32();
        if (size < 0 || count < 0 || (expected != 0 && count > 0 && static_cast<std::uint32_t>(size) != expected))
        {
            throw SerializationError(
                fmt::format("{}: bulk array of {} elements of {} bytes (expected {} bytes)", what, count, size, expected),
                at);
        }
        if (static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(size) > reader.remaining())
        {
            throw SerializationError(fmt::format("{}: bulk array of {} x {} bytes exceeds the data", what, count, size),
                                     at);
        }
        return {static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(size)};
    }

    inline Vec3 read_vec3(BinaryReader& r)
    {
        Vec3 v;
        v.x = r.read_f32();
        v.y = r.read_f32();
        v.z = r.read_f32();
        return v;
    }

    inline Quat read_quat(BinaryReader& r)
    {
        Quat q;
        q.x = r.read_f32();
        q.y = r.read_f32();
        q.z = r.read_f32();
        q.w = r.read_f32();
        return q;
    }

    inline Color read_color(BinaryReader& r)
    {
        Color c;
        c.b = r.read_u8();
        c.g = r.read_u8();
        c.r = r.read_u8();
        c.a = r.read_u8();
        return c;
    }

    inline BoxSphereBounds read_bounds(BinaryReader& r)
    {
        BoxSphereBounds b;
        b.origin = read_vec3(r);
        b.box_extent = read_vec3(r);
        b.sphere_radius = r.read_f32();
        return b;
    }

    /// IEEE half to float.
    float half_to_float(std::uint16_t h) noexcept;
}
