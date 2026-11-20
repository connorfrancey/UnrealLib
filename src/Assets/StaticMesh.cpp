#include <UnrealLib/Assets/StaticMesh.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
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
            else if (exponent == 31)
            {
                // A NaN keeps its payload in the float's top mantissa bits, so float_to_half() gives the same half
                const std::uint32_t bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
                std::memcpy(&value, &bits, 4);
                return mantissa != 0 ? value : (sign != 0 ? -INFINITY : INFINITY);
            }
            else value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
            return sign != 0 ? -value : value;
        }

        std::uint32_t pack_normal(const Vec3& v) noexcept
        {
            const auto component = [](float f) {
                return static_cast<std::uint32_t>(std::round((std::clamp(f, -1.0f, 1.0f) + 1.0f) * 127.5f));
            };
            return component(v.x) | (component(v.y) << 8) | (component(v.z) << 16) | (128u << 24);
        }

        /// Round to nearest even; the inverse of half_to_float() for every finite half and the infinities.
        std::uint16_t float_to_half(float f) noexcept
        {
            std::uint32_t x = 0;
            std::memcpy(&x, &f, 4);
            const std::uint32_t sign = (x >> 16) & 0x8000;
            const std::uint32_t raw_exponent = (x >> 23) & 0xFF;
            std::uint32_t mantissa = x & 0x7FFFFF;
            if (raw_exponent == 0xFF)
            {
                const std::uint32_t payload = mantissa >> 13; // half_to_float() put a half's NaN payload there
                return static_cast<std::uint16_t>(sign | 0x7C00 | (mantissa == 0 ? 0 : payload != 0 ? payload : 0x200));
            }
            const std::int32_t exponent = static_cast<std::int32_t>(raw_exponent) - 127 + 15;
            if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00);
            if (exponent <= 0)
            {
                if (exponent < -10) return static_cast<std::uint16_t>(sign);
                mantissa |= 0x800000;
                const int shift = 14 - exponent;
                std::uint32_t h = mantissa >> shift;
                const std::uint32_t rest = mantissa & ((1u << shift) - 1), half = 1u << (shift - 1);
                if (rest > half || (rest == half && (h & 1) != 0)) ++h;
                return static_cast<std::uint16_t>(sign | h);
            }
            std::uint32_t h = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
            const std::uint32_t rest = mantissa & 0x1FFF;
            if (rest > 0x1000 || (rest == 0x1000 && (h & 1) != 0)) ++h; // a carry into the exponent is right
            return static_cast<std::uint16_t>(sign | h);
        }

        void write_vec3(BinaryWriter& w, const Vec3& v)
        {
            w.write_f32(v.x);
            w.write_f32(v.y);
            w.write_f32(v.z);
        }

        void write_bulk_array_header(BinaryWriter& w, std::uint32_t element_size, std::size_t count)
        {
            w.write_i32(static_cast<std::int32_t>(element_size));
            w.write_i32(static_cast<std::int32_t>(count));
        }
    }

    using namespace detail;

    namespace
    {
        /// First file version with the late UE3 StaticMesh layout UnrealLib reads (see StaticMesh).
        constexpr std::uint16_t late_static_mesh_version = 868;

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
                // Platform data flag (zero in PC packages; a non-zero value would be followed by console data).
                const std::uint8_t flag = r.read_u8();
                if (flag != 0)
                {
                    throw UnsupportedError(fmt::format("StaticMesh section platform data (flag {}) is not supported", flag));
                }
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
            lod.packed_tangents.resize(uv.count);
            lod.packed_normals.resize(uv.count);
            if (has_color) lod.colors.resize(uv.count);
            lod.uvs.assign(static_cast<std::size_t>(lod.num_uv_sets), std::vector<Vec2>(uv.count));
            for (std::uint32_t i = 0; i < uv.count; ++i)
            {
                lod.packed_tangents[i] = r.read_u32();
                lod.packed_normals[i] = r.read_u32();
                lod.tangents[i] = unpack_normal(lod.packed_tangents[i]);
                lod.normals[i] = unpack_normal(lod.packed_normals[i]);
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
            if (version >= 615) // color stream: item size, vertex count, then the colors only if there are any
            {
                const std::int32_t color_size = r.read_i32();
                const std::int32_t color_vertices = r.read_i32();
                if (color_vertices != 0)
                {
                    const BulkArrayHeader colors = read_bulk_array_header(r, 4, "StaticMesh color stream");
                    if (color_size != 4 || color_vertices != static_cast<std::int32_t>(colors.count) ||
                        colors.count != positions.count)
                    {
                        throw SerializationError(fmt::format("StaticMesh color stream: item size {}, {} vertices, {} "
                                                             "stored, {} positions",
                                                             color_size, color_vertices, colors.count, positions.count));
                    }
                    lod.colors.reserve(colors.count);
                    for (std::uint32_t i = 0; i < colors.count; ++i) lod.colors.push_back(read_color(r));
                }
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
            if (version >= late_static_mesh_version)
            {
                lod.adjacency_indices = read_index_buffer(r, "StaticMesh adjacency indices");
            }
            if (version < 686)
            {
                const BulkArrayHeader edges = read_bulk_array_header(r, 16, "StaticMesh edges");
                lod.num_edges = static_cast<std::int32_t>(edges.count);
                const auto edge_bytes = r.read_view(static_cast<std::size_t>(edges.count) * 16);
                lod.edges.assign(edge_bytes.begin(), edge_bytes.end());
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
        // Versions 770..867 changed the layout step by step; only the endpoints are verified.
        if (version < 472 || (version >= 770 && version < late_static_mesh_version))
        {
            throw UnsupportedError(fmt::format("StaticMesh layout of file version {} is not supported", version));
        }
        const bool late = version >= late_static_mesh_version;
        StaticMesh mesh;
        mesh.name = object.package->object_name(object.index);
        mesh.bounds = read_bounds(r);
        mesh.body_setup = ObjectIndex{r.read_i32()};
        if (late) (void)r.read_view(24); // compact kDOP tree: bounding box (recomputable from the nodes)
        const std::uint32_t node_size = late ? 6u : 32u;
        const BulkArrayHeader nodes = read_bulk_array_header(r, node_size, "StaticMesh kDOP nodes");
        mesh.kdop_node_count = static_cast<std::int32_t>(nodes.count);
        const auto node_bytes = r.read_view(static_cast<std::size_t>(nodes.count) * node_size);
        mesh.kdop_nodes.assign(node_bytes.begin(), node_bytes.end());
        const BulkArrayHeader triangles = read_bulk_array_header(r, 8, "StaticMesh kDOP triangles");
        mesh.kdop_triangle_count = static_cast<std::int32_t>(triangles.count);
        mesh.kdop_triangles.resize(triangles.count);
        for (KDopTriangle& t : mesh.kdop_triangles)
        {
            for (auto& v : t.vertices) v = r.read_u16();
            t.material_index = r.read_u16();
        }
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
        if (late)
        {
            // Source data, optimization settings, bHasBeenSimplified, bIsMeshProxy: all empty when cooked.
            for (const char* what : {"source data", "optimization settings", "simplification", "mesh proxy"})
            {
                if (const std::int32_t v = r.read_i32(); v != 0)
                {
                    throw UnsupportedError(fmt::format("StaticMesh {} ({}) is not supported", what, v));
                }
            }
        }
        const std::uint32_t lods = r.read_array_count(64);
        for (std::uint32_t i = 0; i < lods; ++i) mesh.lods.push_back(read_lod(r, version));
        if (late) mesh.late_leading_value = r.read_i32();
        for (auto& a : mesh.thumbnail_angle) a = r.read_i32();
        mesh.thumbnail_distance = r.read_f32();
        if (late)
        {
            mesh.high_res_source_mesh_name = r.read_string();
            mesh.high_res_source_mesh_crc = r.read_u32();
            mesh.lighting_guid = r.read_guid();
            mesh.vertex_position_version = r.read_i32();
            const std::uint32_t factors = r.read_array_count(4);
            mesh.streaming_texture_factors.reserve(factors);
            for (std::uint32_t i = 0; i < factors; ++i) mesh.streaming_texture_factors.push_back(r.read_f32());
            mesh.remove_degenerates = r.read_bool32();
            for (auto& v : mesh.late_trailer) v = r.read_i32();
        }
        if (object.package->profile().assets.static_mesh_trailing_float)
        {
            mesh.trailing_float = r.read_f32();
        }
        expect_end(r, linker.qualified_path(object));
        mesh.properties = source.properties;
        return mesh;
    }

    namespace
    {
        void write_index_buffer(BinaryWriter& w, const std::vector<std::uint16_t>& indices)
        {
            write_bulk_array_header(w, 2, indices.size());
            for (const std::uint16_t i : indices) w.write_u16(i);
        }

        void require_count(std::size_t count, std::size_t vertices, std::string_view what)
        {
            if (count != vertices)
            {
                throw SerializationError(fmt::format("StaticMesh {}: {} entries for {} vertices", what, count, vertices));
            }
        }

        void write_lod(BinaryWriter& w, const StaticMeshLod& lod, std::uint16_t version, std::uint64_t base_offset)
        {
            // Cooked meshes carry no source triangles. Their empty record's offset is never read, and the game's
            // point 216 bytes before the record (a cooker quirk): kept as stored, computed only for a new mesh.
            BulkData raw = lod.raw_triangles;
            if (raw.has_payload())
            {
                throw UnsupportedError("writing a StaticMesh with source triangles (an uncooked mesh) is not supported");
            }
            if (raw.size_on_disk == 0 && raw.offset_in_file != 0)
            {
                w.write_u32(raw.flags);
                w.write_i32(raw.element_count);
                w.write_i32(0);
                w.write_i32(raw.offset_in_file);
            }
            else write_bulk_data(w, raw, {}, base_offset);
            w.write_i32(static_cast<std::int32_t>(lod.sections.size()));
            for (const StaticMeshSection& s : lod.sections)
            {
                w.write_i32(s.material.value);
                w.write_bool32(s.enable_collision);
                w.write_bool32(s.old_enable_collision);
                if (version >= 473) w.write_bool32(s.enable_shadow_casting);
                w.write_i32(s.first_index);
                w.write_i32(s.num_triangles);
                w.write_i32(s.min_vertex);
                w.write_i32(s.max_vertex);
                if (version >= 492) w.write_i32(s.material_index);
                if (version >= 514)
                {
                    w.write_i32(static_cast<std::int32_t>(s.fragments.size()));
                    for (const auto& f : s.fragments)
                    {
                        w.write_i32(f[0]);
                        w.write_i32(f[1]);
                    }
                }
            }

            const std::size_t n = lod.positions.size();
            w.write_i32(12);
            w.write_i32(static_cast<std::int32_t>(n));
            write_bulk_array_header(w, 12, n);
            for (const Vec3& p : lod.positions) write_vec3(w, p);

            // UV stream: packed tangent X and Z, a color (434..614), the UV sets
            const bool has_color = version >= 434;
            const bool packed = lod.packed_tangents.size() == n && lod.packed_normals.size() == n;
            if (!packed)
            {
                require_count(lod.tangents.size(), n, "tangents");
                require_count(lod.normals.size(), n, "normals");
            }
            if (has_color && !lod.colors.empty()) require_count(lod.colors.size(), n, "colors");
            if (lod.uvs.size() != static_cast<std::size_t>(lod.num_uv_sets))
            {
                throw SerializationError(
                    fmt::format("StaticMesh has {} UV sets but num_uv_sets {}", lod.uvs.size(), lod.num_uv_sets));
            }
            for (const auto& set : lod.uvs) require_count(set.size(), n, "UVs");
            const std::uint32_t item = 8u + (has_color ? 4u : 0u) +
                                       static_cast<std::uint32_t>(lod.num_uv_sets) * (lod.full_precision_uvs ? 8u : 4u);
            w.write_i32(lod.num_uv_sets);
            w.write_i32(static_cast<std::int32_t>(item));
            w.write_i32(static_cast<std::int32_t>(n));
            if (version >= 474) w.write_bool32(lod.full_precision_uvs);
            write_bulk_array_header(w, item, n);
            for (std::size_t i = 0; i < n; ++i)
            {
                w.write_u32(packed ? lod.packed_tangents[i] : pack_normal(lod.tangents[i]));
                w.write_u32(packed ? lod.packed_normals[i] : pack_normal(lod.normals[i]));
                if (has_color)
                {
                    const Color c = lod.colors.empty() ? Color{255, 255, 255, 255} : lod.colors[i];
                    w.write_u8(c.b);
                    w.write_u8(c.g);
                    w.write_u8(c.r);
                    w.write_u8(c.a);
                }
                for (const auto& set : lod.uvs)
                {
                    if (lod.full_precision_uvs)
                    {
                        w.write_f32(set[i].u);
                        w.write_f32(set[i].v);
                    }
                    else
                    {
                        w.write_u16(float_to_half(set[i].u));
                        w.write_u16(float_to_half(set[i].v));
                    }
                }
            }
            // Shadow volume stream (item size 4 and the float count, as stored)
            w.write_i32(4);
            w.write_i32(static_cast<std::int32_t>(lod.shadow_extrusion.size()));
            write_bulk_array_header(w, 4, lod.shadow_extrusion.size());
            for (const float f : lod.shadow_extrusion) w.write_f32(f);
            w.write_i32(lod.num_vertices);
            write_index_buffer(w, lod.indices);
            write_index_buffer(w, lod.wireframe_indices);
            if (lod.edges.size() % 16 != 0) throw SerializationError("StaticMesh edges are not 16 bytes each");
            write_bulk_array_header(w, 16, lod.edges.size() / 16);
            w.write_bytes(lod.edges);
            w.write_i32(static_cast<std::int32_t>(lod.shadow_triangle_double_sided.size()));
            for (const std::uint8_t b : lod.shadow_triangle_double_sided) w.write_u8(b);
        }
    }

    std::vector<std::byte> write_static_mesh(const StaticMesh& mesh, const SerializationContext& context,
                                             std::uint64_t base_offset)
    {
        const std::uint16_t version = context.file_version;
        if (version < 472 || version >= 615)
        {
            throw UnsupportedError(
                fmt::format("writing the StaticMesh layout of file version {} is not supported", version));
        }
        if (mesh.kdop_nodes.size() % 32 != 0) throw SerializationError("StaticMesh kDOP nodes are not 32 bytes each");
        BinaryWriter w(context);
        write_vec3(w, mesh.bounds.origin);
        write_vec3(w, mesh.bounds.box_extent);
        w.write_f32(mesh.bounds.sphere_radius);
        w.write_i32(mesh.body_setup.value);
        write_bulk_array_header(w, 32, mesh.kdop_nodes.size() / 32);
        w.write_bytes(mesh.kdop_nodes);
        write_bulk_array_header(w, 8, mesh.kdop_triangles.size());
        for (const KDopTriangle& t : mesh.kdop_triangles)
        {
            for (const std::uint16_t v : t.vertices) w.write_u16(v);
            w.write_u16(t.material_index);
        }
        w.write_i32(mesh.internal_version);
        if (mesh.internal_version >= 17 && version < 593)
        {
            if (!mesh.content_tags.empty()) throw UnsupportedError("writing StaticMesh content tags is not supported");
            w.write_i32(0);
        }
        w.write_i32(static_cast<std::int32_t>(mesh.lods.size()));
        for (const StaticMeshLod& lod : mesh.lods) write_lod(w, lod, version, base_offset);
        for (const std::int32_t a : mesh.thumbnail_angle) w.write_i32(a);
        w.write_f32(mesh.thumbnail_distance);
        if (mesh.trailing_float) w.write_f32(*mesh.trailing_float);
        return w.take();
    }

    std::vector<KDopNode> decode_kdop_nodes(const StaticMesh& mesh)
    {
        if (mesh.kdop_nodes.size() % 32 != 0) throw SerializationError("StaticMesh kDOP nodes are not 32 bytes each");
        std::vector<KDopNode> nodes(mesh.kdop_nodes.size() / 32);
        const auto f32 = [&](std::size_t at) {
            float v = 0;
            std::memcpy(&v, mesh.kdop_nodes.data() + at, 4);
            return v;
        };
        const auto u16 = [&](std::size_t at) {
            std::uint16_t v = 0;
            std::memcpy(&v, mesh.kdop_nodes.data() + at, 2);
            return v;
        };
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const std::size_t at = i * 32;
            KDopNode& n = nodes[i];
            n.min = {f32(at), f32(at + 4), f32(at + 8)};
            n.max = {f32(at + 12), f32(at + 16), f32(at + 20)};
            std::uint32_t leaf = 0;
            std::memcpy(&leaf, mesh.kdop_nodes.data() + at + 24, 4);
            n.leaf = leaf != 0;
            n.first = u16(at + 28);
            n.second = u16(at + 30);
        }
        return nodes;
    }

    namespace
    {
        constexpr std::size_t kMaxTrianglesPerLeaf = 5;

        struct BuildTriangle
        {
            KDopTriangle triangle;
            Vec3 centroid;
        };

        struct KDopBuilder
        {
            const std::vector<Vec3>& positions;
            std::vector<BuildTriangle>& triangles;
            std::vector<KDopNode> nodes;

            float axis(const Vec3& v, int a) const { return a == 0 ? v.x : a == 1 ? v.y : v.z; }

            void bound(KDopNode& node, std::size_t start, std::size_t count) const
            {
                node.min = {FLT_MAX, FLT_MAX, FLT_MAX};
                node.max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
                for (std::size_t t = start; t < start + count; ++t)
                {
                    for (const std::uint16_t v : triangles[t].triangle.vertices)
                    {
                        const Vec3& p = positions[v];
                        node.min = {std::min(node.min.x, p.x), std::min(node.min.y, p.y), std::min(node.min.z, p.z)};
                        node.max = {std::max(node.max.x, p.x), std::max(node.max.y, p.y), std::max(node.max.z, p.z)};
                    }
                }
            }

            void split(std::size_t index, std::size_t start, std::size_t count)
            {
                bound(nodes[index], start, count);
                if (count <= kMaxTrianglesPerLeaf)
                {
                    nodes[index].leaf = true;
                    nodes[index].first = static_cast<std::uint16_t>(count);
                    nodes[index].second = static_cast<std::uint16_t>(start);
                    return;
                }
                // The axis where the centroids spread most, split at their mean
                int best = 0;
                float best_variance = -1, best_mean = 0;
                for (int a = 0; a < 3; ++a)
                {
                    double mean = 0, variance = 0;
                    for (std::size_t t = start; t < start + count; ++t) mean += axis(triangles[t].centroid, a);
                    mean /= static_cast<double>(count);
                    for (std::size_t t = start; t < start + count; ++t)
                    {
                        const double d = axis(triangles[t].centroid, a) - mean;
                        variance += d * d;
                    }
                    if (variance > best_variance)
                    {
                        best = a;
                        best_variance = static_cast<float>(variance);
                        best_mean = static_cast<float>(mean);
                    }
                }
                const auto begin = triangles.begin() + static_cast<std::ptrdiff_t>(start);
                const auto middle = std::stable_partition(begin, begin + static_cast<std::ptrdiff_t>(count),
                                                          [&](const BuildTriangle& t) { return axis(t.centroid, best) <= best_mean; });
                std::size_t left = static_cast<std::size_t>(middle - begin);
                if (left == 0 || left == count) left = count / 2; // every centroid on one side: halves
                nodes[index].leaf = false;
                const std::size_t left_node = nodes.size();
                nodes.emplace_back();
                nodes.emplace_back();
                if (nodes.size() > 0xFFFF) throw Error(ErrorCode::LimitExceeded, "StaticMesh kDOP tree has more than 65535 nodes");
                nodes[index].first = static_cast<std::uint16_t>(left_node);
                nodes[index].second = static_cast<std::uint16_t>(left_node + 1);
                split(left_node, start, left);
                split(left_node + 1, start + left, count - left);
            }
        };
    }

    void build_kdop(StaticMesh& mesh)
    {
        if (mesh.lods.empty()) throw Error(ErrorCode::InvalidArgument, "StaticMesh has no LOD to build collision from");
        const StaticMeshLod& lod = mesh.lods.front();
        std::vector<BuildTriangle> triangles;
        for (std::size_t s = 0; s < lod.sections.size(); ++s)
        {
            const StaticMeshSection& section = lod.sections[s];
            if (!section.enable_collision) continue;
            for (std::int32_t t = 0; t < section.num_triangles; ++t)
            {
                BuildTriangle b;
                const std::size_t at = static_cast<std::size_t>(section.first_index) + static_cast<std::size_t>(t) * 3;
                for (int k = 0; k < 3; ++k) b.triangle.vertices[static_cast<std::size_t>(k)] = lod.indices.at(at + static_cast<std::size_t>(k));
                b.triangle.material_index = static_cast<std::uint16_t>(s);
                const Vec3 &a = lod.positions.at(b.triangle.vertices[0]), &c = lod.positions.at(b.triangle.vertices[1]),
                           &d = lod.positions.at(b.triangle.vertices[2]);
                b.centroid = {(a.x + c.x + d.x) / 3, (a.y + c.y + d.y) / 3, (a.z + c.z + d.z) / 3};
                triangles.push_back(b);
            }
        }
        if (triangles.size() > 0xFFFF) throw Error(ErrorCode::LimitExceeded, "StaticMesh has more than 65535 collision triangles");
        KDopBuilder builder{lod.positions, triangles, {}};
        if (!triangles.empty())
        {
            builder.nodes.emplace_back();
            builder.split(0, 0, triangles.size());
        }
        mesh.kdop_triangles.clear();
        for (const BuildTriangle& t : triangles) mesh.kdop_triangles.push_back(t.triangle);
        mesh.kdop_nodes.assign(builder.nodes.size() * 32, std::byte{0});
        for (std::size_t i = 0; i < builder.nodes.size(); ++i)
        {
            const KDopNode& n = builder.nodes[i];
            std::byte* at = mesh.kdop_nodes.data() + i * 32;
            const float box[6] = {n.min.x, n.min.y, n.min.z, n.max.x, n.max.y, n.max.z};
            std::memcpy(at, box, 24);
            const std::uint32_t leaf = n.leaf ? 1u : 0u;
            std::memcpy(at + 24, &leaf, 4);
            std::memcpy(at + 28, &n.first, 2);
            std::memcpy(at + 30, &n.second, 2);
        }
        mesh.kdop_node_count = static_cast<std::int32_t>(builder.nodes.size());
        mesh.kdop_triangle_count = static_cast<std::int32_t>(mesh.kdop_triangles.size());
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
