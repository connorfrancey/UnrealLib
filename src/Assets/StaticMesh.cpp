#include <UnrealLib/Assets/StaticMesh.hpp>

#include <cmath>
#include <cstring>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Utilities/Text.hpp>

#include "MeshReading.hpp"

namespace UnrealLib::Assets
{
    Vec3 unpack_normal(std::uint32_t packed) noexcept
    {
        const auto component = [&](int shift) { return static_cast<float>((packed >> shift) & 0xFF) / 127.5f - 1.0f; };
        return {component(0), component(8), component(16)};
    }

    namespace detail
    {
        float half_to_float(std::uint16_t h) noexcept
        {
            const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
            float value = 0;
            if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
            else if (exponent == 31) value = mantissa != 0 ? std::nanf("") : INFINITY;
            else value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
            return sign != 0 ? -value : value;
        }
    }

    using namespace detail;

    namespace
    {
        StaticMeshSection read_section(BinaryReader& r, std::uint16_t version)
        {
            StaticMeshSection s;
            s.material = ObjectIndex{r.read_i32()};
            s.enable_collision = r.read_bool32();
            s.old_enable_collision = r.read_bool32();
            if (version >= 473) s.enable_shadow_casting = r.read_bool32();
            s.first_index = r.read_i32();
            s.num_triangles = r.read_i32();
            s.min_vertex = r.read_i32();
            s.max_vertex = r.read_i32();
            if (version >= 492) s.material_index = r.read_i32();
            if (version >= 514)
            {
                const std::uint32_t n = r.read_array_count(8);
                s.fragments.resize(n);
                for (auto& f : s.fragments)
                {
                    f[0] = r.read_i32();
                    f[1] = r.read_i32();
                }
            }
            if (version >= 618)
            {
                throw UnsupportedError("StaticMesh sections of file version >= 618 are not supported");
            }
            return s;
        }

        std::vector<std::uint16_t> read_index_buffer(BinaryReader& r, std::string_view what)
        {
            const BulkArrayHeader h = read_bulk_array_header(r, 2, what);
            std::vector<std::uint16_t> out(h.count);
            for (auto& i : out) i = r.read_u16();
            return out;
        }

        StaticMeshLod read_lod(BinaryReader& r, std::uint16_t version)
        {
            StaticMeshLod lod;
            lod.raw_triangles = read_bulk_data(r);
            const std::uint32_t sections = r.read_array_count(28);
            lod.sections.reserve(sections);
            for (std::uint32_t i = 0; i < sections; ++i) lod.sections.push_back(read_section(r, version));

            // Position stream.
            const std::int32_t vertex_size = r.read_i32();
            const std::int32_t vertex_count = r.read_i32();
            const BulkArrayHeader positions = read_bulk_array_header(r, 12, "StaticMesh positions");
            if (vertex_size != 12 || vertex_count != static_cast<std::int32_t>(positions.count))
            {
                throw SerializationError(fmt::format("StaticMesh position stream: vertex size {}, {} vertices, {} stored",
                                                     vertex_size, vertex_count, positions.count));
            }
            lod.positions.reserve(positions.count);
            for (std::uint32_t i = 0; i < positions.count; ++i) lod.positions.push_back(read_vec3(r));

            // UV stream: packed tangent X and Z, color (434..614), then the UV sets.
            lod.num_uv_sets = r.read_i32();
            const std::int32_t item_size = r.read_i32();
            const std::int32_t uv_vertices = r.read_i32();
            lod.full_precision_uvs = version >= 474 ? r.read_bool32() : true;
            if (lod.num_uv_sets < 0 || lod.num_uv_sets > 8)
            {
                throw SerializationError(fmt::format("StaticMesh has {} UV sets", lod.num_uv_sets));
            }
            const bool has_color = version >= 434 && version < 615;
            const std::uint32_t expected_item = 8u + (has_color ? 4u : 0u) +
                                                static_cast<std::uint32_t>(lod.num_uv_sets) *
                                                    (lod.full_precision_uvs ? 8u : 4u);
            const BulkArrayHeader uv = read_bulk_array_header(r, expected_item, "StaticMesh UV stream");
            if (item_size != static_cast<std::int32_t>(expected_item) || uv_vertices != static_cast<std::int32_t>(uv.count) ||
                uv.count != positions.count)
            {
                throw SerializationError(fmt::format("StaticMesh UV stream: item size {} (expected {}), {} vertices, {} "
                                                     "stored, {} positions",
                                                     item_size, expected_item, uv_vertices, uv.count, positions.count));
            }
            lod.tangents.resize(uv.count);
            lod.normals.resize(uv.count);
            if (has_color) lod.colors.resize(uv.count);
            lod.uvs.assign(static_cast<std::size_t>(lod.num_uv_sets), std::vector<Vec2>(uv.count));
            for (std::uint32_t i = 0; i < uv.count; ++i)
            {
                lod.tangents[i] = unpack_normal(r.read_u32());
                lod.normals[i] = unpack_normal(r.read_u32());
                if (has_color) lod.colors[i] = read_color(r);
                for (auto& set : lod.uvs)
                {
                    if (lod.full_precision_uvs)
                    {
                        set[i].u = r.read_f32();
                        set[i].v = r.read_f32();
                    }
                    else
                    {
                        set[i].u = half_to_float(r.read_u16());
                        set[i].v = half_to_float(r.read_u16());
                    }
                }
            }
            if (version >= 615)
            {
                throw UnsupportedError("StaticMesh color streams (file version >= 615) are not supported");
            }
            if (version < 686)
            {
                (void)r.read_i32(); // item size
                (void)r.read_i32(); // vertex count
                const BulkArrayHeader shadow = read_bulk_array_header(r, 4, "StaticMesh shadow stream");
                lod.shadow_extrusion.resize(shadow.count);
                for (auto& f : lod.shadow_extrusion) f = r.read_f32();
            }
            lod.num_vertices = r.read_i32();
            lod.indices = read_index_buffer(r, "StaticMesh indices");
            lod.wireframe_indices = read_index_buffer(r, "StaticMesh wireframe indices");
            if (version < 686)
            {
                const BulkArrayHeader edges = read_bulk_array_header(r, 16, "StaticMesh edges");
                lod.num_edges = static_cast<std::int32_t>(edges.count);
                (void)r.read_view(static_cast<std::size_t>(edges.count) * 16);
                const std::uint32_t n = r.read_array_count(1);
                lod.shadow_triangle_double_sided.resize(n);
                for (auto& b : lod.shadow_triangle_double_sided) b = r.read_u8();
            }
            for (const StaticMeshSection& s : lod.sections)
            {
                if (s.first_index < 0 || s.num_triangles < 0 ||
                    static_cast<std::uint64_t>(s.first_index) + static_cast<std::uint64_t>(s.num_triangles) * 3 >
                        lod.indices.size())
                {
                    throw SerializationError("StaticMesh section exceeds the index buffer");
                }
            }
            for (const std::uint16_t i : lod.indices)
            {
                if (i >= lod.positions.size())
                {
                    throw SerializationError(fmt::format("StaticMesh index {} exceeds {} vertices", i, lod.positions.size()));
                }
            }
            return lod;
        }
    }

    StaticMesh read_static_mesh(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        const AssetSource source = open_asset(linker, object, schema);
        if (!Text::iequals(source.class_name, "StaticMesh"))
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("'{}' is a {}, not a StaticMesh",
                                                                linker.qualified_path(object), source.class_name));
        }
        BinaryReader r = source.native_reader();
        const std::uint16_t version = r.context().file_version;
        if (version < 472 || version >= 770)
        {
            throw UnsupportedError(fmt::format("StaticMesh layout of file version {} is not supported", version));
        }
        StaticMesh mesh;
        mesh.name = object.package->object_name(object.index);
        mesh.bounds = read_bounds(r);
        mesh.body_setup = ObjectIndex{r.read_i32()};
        const BulkArrayHeader nodes = read_bulk_array_header(r, 32, "StaticMesh kDOP nodes");
        mesh.kdop_node_count = static_cast<std::int32_t>(nodes.count);
        (void)r.read_view(static_cast<std::size_t>(nodes.count) * 32);
        const BulkArrayHeader triangles = read_bulk_array_header(r, 8, "StaticMesh kDOP triangles");
        mesh.kdop_triangle_count = static_cast<std::int32_t>(triangles.count);
        (void)r.read_view(static_cast<std::size_t>(triangles.count) * 8);
        mesh.internal_version = r.read_i32();
        if (mesh.internal_version >= 17 && version < 593)
        {
            const std::uint32_t n = r.read_array_count(8);
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const FName name = read_fname(r);
                mesh.content_tags.push_back(object.package->names().to_string(name));
            }
        }
        const std::uint32_t lods = r.read_array_count(64);
        for (std::uint32_t i = 0; i < lods; ++i) mesh.lods.push_back(read_lod(r, version));
        for (auto& a : mesh.thumbnail_angle) a = r.read_i32();
        mesh.thumbnail_distance = r.read_f32();
        if (object.package->profile().assets.static_mesh_trailing_float)
        {
            mesh.trailing_float = r.read_f32();
        }
        expect_end(r, linker.qualified_path(object));
        mesh.properties = source.properties;
        return mesh;
    }

    std::string to_obj(const Package& package, const StaticMesh& mesh, std::size_t lod_index)
    {
        if (lod_index >= mesh.lods.size())
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("'{}' has {} LODs", mesh.name, mesh.lods.size()));
        }
        const StaticMeshLod& lod = mesh.lods[lod_index];
        std::string out = fmt::format("# {} (LOD {}) exported by UnrealLib\n", mesh.name, lod_index);
        for (const Vec3& p : lod.positions) out += fmt::format("v {} {} {}\n", p.x, p.z, p.y);
        for (const Vec3& n : lod.normals) out += fmt::format("vn {} {} {}\n", n.x, n.z, n.y);
        const bool has_uv = !lod.uvs.empty();
        if (has_uv)
        {
            for (const Vec2& t : lod.uvs[0]) out += fmt::format("vt {} {}\n", t.u, 1.0f - t.v);
        }
        for (std::size_t s = 0; s < lod.sections.size(); ++s)
        {
            const StaticMeshSection& section = lod.sections[s];
            const std::string material =
                section.material.is_null() || !package.is_valid(section.material) ? "None"
                                                                                   : package.object_name(section.material);
            out += fmt::format("g section{}\nusemtl {}\n", s, material);
            for (std::int32_t t = 0; t < section.num_triangles; ++t)
            {
                out += "f";
                for (int k = 0; k < 3; ++k)
                {
                    const std::uint32_t v =
                        lod.indices[static_cast<std::size_t>(section.first_index) + static_cast<std::size_t>(t) * 3 +
                                    static_cast<std::size_t>(k)] + 1u;
                    out += has_uv ? fmt::format(" {}/{}/{}", v, v, v) : fmt::format(" {}//{}", v, v);
                }
                out += "\n";
            }
        }
        return out;
    }
}
