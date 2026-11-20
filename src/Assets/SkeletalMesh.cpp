#include <UnrealLib/Assets/SkeletalMesh.hpp>

#include <algorithm>
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
    using namespace detail;

    namespace
    {
        struct Rules
        {
            std::uint16_t version = 0;
            int chunk_uv_sets = 1;
            const AssetRules* assets = nullptr;
            bool late = false;              // file version >= late_skeletal_mesh_version
            bool has_vertex_colors = false; // bHasVertexColors property
        };

        /// First file version with the late UE3 SkeletalMesh layout UnrealLib reads (see SkeletalMesh).
        constexpr std::uint16_t late_skeletal_mesh_version = 868;

        /// FMultiSizeIndexContainer (file version >= 806): bNeedsCPUAccess, element size, bulk array.
        std::vector<std::uint32_t> read_multi_size_indices(BinaryReader& r, std::string_view what)
        {
            (void)r.read_i32(); // bNeedsCPUAccess
            const std::uint8_t type_size = r.read_u8();
            if (type_size != 2 && type_size != 4)
            {
                throw SerializationError(fmt::format("{}: index size {}", what, type_size));
            }
            const BulkArrayHeader h = read_bulk_array_header(r, type_size, what);
            std::vector<std::uint32_t> out(h.count);
            for (auto& i : out) i = type_size == 2 ? r.read_u16() : r.read_u32();
            return out;
        }

        std::vector<std::uint16_t> read_u16_array(BinaryReader& r)
        {
            const std::uint32_t n = r.read_array_count(2);
            std::vector<std::uint16_t> out(n);
            for (auto& v : out) v = r.read_u16();
            return out;
        }

        std::vector<std::int16_t> read_i16_array(BinaryReader& r)
        {
            const std::uint32_t n = r.read_array_count(2);
            std::vector<std::int16_t> out(n);
            for (auto& v : out) v = r.read_i16();
            return out;
        }

        std::vector<std::uint8_t> read_u8_array(BinaryReader& r)
        {
            const std::uint32_t n = r.read_array_count(1);
            std::vector<std::uint8_t> out(n);
            for (auto& v : out) v = r.read_u8();
            return out;
        }

        MeshBone read_bone(BinaryReader& r, const NameTable& names, std::uint16_t version)
        {
            MeshBone b;
            b.name = names.to_string(read_fname(r));
            b.flags = r.read_u32();
            b.orientation = read_quat(r);
            b.position = read_vec3(r);
            b.num_children = r.read_i32();
            b.parent_index = r.read_i32();
            if (version >= 515) b.color = read_color(r);
            return b;
        }

        SkeletalChunk read_chunk(BinaryReader& r, const Rules& rules)
        {
            SkeletalChunk c;
            c.first_vertex = r.read_i32();
            // Rigid: position, 3 packed normals, UV sets, bone index. Soft: ... 4 bone indices, 4 weights.
            const std::size_t uv_bytes = static_cast<std::size_t>(rules.chunk_uv_sets) * 8;
            const std::size_t color_bytes = rules.late ? 4 : 0;
            const std::size_t rigid_size = 12 + 12 + uv_bytes + color_bytes + 1;
            const std::size_t soft_size = 12 + 12 + uv_bytes + color_bytes + 8;
            const auto bytes = [&](std::size_t n) {
                const auto view = r.read_view(n);
                return std::vector<std::byte>(view.begin(), view.end());
            };
            c.stored_rigid_vertices = r.read_array_count(rigid_size);
            c.stored_rigid_bytes = bytes(c.stored_rigid_vertices * rigid_size);
            c.stored_soft_vertices = r.read_array_count(soft_size);
            c.stored_soft_bytes = bytes(c.stored_soft_vertices * soft_size);
            c.bone_map = read_i16_array(r);
            c.rigid_vertices = r.read_i32();
            c.soft_vertices = r.read_i32();
            c.max_influences = r.read_i32();
            return c;
        }

        void read_gpu_skin(BinaryReader& r, SkeletalLod& lod, const Rules& rules)
        {
            if (rules.version >= 709 || rules.assets->skeletal_gpu_uv_count)
            {
                lod.num_uv_sets = r.read_i32();
            }
            if (lod.num_uv_sets < 1 || lod.num_uv_sets > 4)
            {
                throw SerializationError(fmt::format("skeletal GPU skin has {} UV sets", lod.num_uv_sets));
            }
            lod.full_precision_uvs = r.read_bool32();
            if (rules.late)
            {
                // bUsePackedPosition, MeshExtension, MeshOrigin. PC packages keep float positions even when the flag
                // is set; the element size below tells.
                (void)r.read_i32();
                (void)read_vec3(r);
                (void)read_vec3(r);
            }
            const auto uv_size = static_cast<std::uint32_t>(lod.full_precision_uvs ? 8 : 4);
            const std::uint32_t vertex_size = 12 + 8 + 8 + uv_size * static_cast<std::uint32_t>(lod.num_uv_sets);
            const BulkArrayHeader h = read_bulk_array_header(r, vertex_size, "SkeletalMesh GPU vertices");
            lod.vertices.resize(h.count);
            for (SkinnedVertex& v : lod.vertices)
            {
                if (!rules.late) v.position = read_vec3(r); // before the normals below version 592
                v.packed_tangent = r.read_u32();
                v.packed_normal = r.read_u32();
                v.tangent = unpack_normal(v.packed_tangent);
                v.normal = unpack_normal(v.packed_normal);
                for (auto& b : v.bones) b = r.read_u8();
                for (auto& w : v.weights) w = r.read_u8();
                if (rules.late) v.position = read_vec3(r);
                for (int i = 0; i < lod.num_uv_sets; ++i)
                {
                    if (lod.full_precision_uvs)
                    {
                        v.uv[static_cast<std::size_t>(i)] = {r.read_f32(), r.read_f32()};
                    }
                    else
                    {
                        const float u = half_to_float(r.read_u16());
                        v.uv[static_cast<std::size_t>(i)] = {u, half_to_float(r.read_u16())};
                    }
                }
            }
        }

        SkeletalLod read_lod(BinaryReader& r, const Rules& rules)
        {
            SkeletalLod lod;
            const std::uint32_t sections = r.read_array_count(14);
            for (std::uint32_t i = 0; i < sections; ++i)
            {
                SkeletalSection s;
                s.material_index = r.read_i16();
                s.chunk_index = r.read_i16();
                s.first_index = r.read_i32();
                if (rules.version >= 806) s.num_triangles = r.read_i32();
                else s.num_triangles = r.read_u16();
                if (rules.version >= 599) (void)r.read_u8();
                lod.sections.push_back(s);
            }
            if (rules.version >= 806)
            {
                lod.indices = read_multi_size_indices(r, "SkeletalMesh indices");
            }
            else
            {
                const BulkArrayHeader indices = read_bulk_array_header(r, 2, "SkeletalMesh indices");
                lod.indices.resize(indices.count);
                for (auto& i : lod.indices) i = r.read_u16();
            }
            if (rules.version < 297) (void)r.read_i32();
            if (rules.version < 686) lod.shadow_indices = read_u16_array(r);
            lod.active_bones = read_i16_array(r);
            if (rules.version < 686) lod.shadow_triangle_double_sided = read_u8_array(r);
            const std::uint32_t chunks = r.read_array_count(20);
            for (std::uint32_t i = 0; i < chunks; ++i) lod.chunks.push_back(read_chunk(r, rules));
            lod.size = r.read_i32();
            lod.num_vertices = r.read_i32();
            if (rules.version < 686)
            {
                lod.num_edges = static_cast<std::int32_t>(r.read_array_count(16));
                const auto edges = r.read_view(static_cast<std::size_t>(lod.num_edges) * 16);
                lod.edges.assign(edges.begin(), edges.end());
            }
            lod.required_bones = read_u8_array(r);
            lod.raw_point_indices = read_bulk_data(r);
            if (lod.raw_point_indices.is_inline() && lod.raw_point_indices.size_on_disk > 0)
            {
                const auto bytes = r.data().subspan(r.position() - static_cast<std::size_t>(lod.raw_point_indices.size_on_disk),
                                                    static_cast<std::size_t>(lod.raw_point_indices.size_on_disk));
                lod.raw_point_index_bytes.assign(bytes.begin(), bytes.end());
            }
            if (rules.version >= 709 && !rules.assets->skeletal_gpu_uv_count) (void)r.read_i32();
            read_gpu_skin(r, lod, rules);
            if (rules.late)
            {
                if (rules.has_vertex_colors)
                {
                    const BulkArrayHeader colors = read_bulk_array_header(r, 4, "SkeletalMesh vertex colors");
                    if (colors.count != lod.vertices.size())
                    {
                        throw SerializationError(fmt::format("skeletal vertex colors: {} for {} vertices", colors.count,
                                                             lod.vertices.size()));
                    }
                    lod.colors.reserve(colors.count);
                    for (std::uint32_t i = 0; i < colors.count; ++i) lod.colors.push_back(read_color(r));
                }
                if (const std::int32_t influences = r.read_i32(); influences != 0)
                {
                    throw UnsupportedError(fmt::format("skeletal VertexInfluences ({}) are not supported", influences));
                }
                lod.adjacency_indices = read_multi_size_indices(r, "SkeletalMesh adjacency indices");
            }
            else if (!rules.assets->skeletal_lod_ends_after_gpu_skin && rules.version >= 534)
            {
                throw UnsupportedError("skeletal ExtraVertexInfluences are not supported");
            }

            // Resolve chunk-local bone indices to skeleton bones.
            for (const SkeletalChunk& c : lod.chunks)
            {
                const std::int64_t end = static_cast<std::int64_t>(c.first_vertex) + c.rigid_vertices + c.soft_vertices;
                if (c.first_vertex < 0 || end > static_cast<std::int64_t>(lod.vertices.size()))
                {
                    throw SerializationError(fmt::format("skeletal chunk [{}, {}) exceeds {} vertices", c.first_vertex,
                                                         end, lod.vertices.size()));
                }
                for (auto v = static_cast<std::size_t>(c.first_vertex); v < static_cast<std::size_t>(end); ++v)
                {
                    for (auto& b : lod.vertices[v].bones)
                    {
                        if (static_cast<std::size_t>(b) >= c.bone_map.size())
                        {
                            throw SerializationError(fmt::format("skeletal vertex {} uses chunk bone {} of {}", v, b,
                                                                 c.bone_map.size()));
                        }
                        b = c.bone_map[static_cast<std::size_t>(b)];
                    }
                }
            }
            for (const SkeletalSection& s : lod.sections)
            {
                if (s.first_index < 0 ||
                    static_cast<std::uint64_t>(s.first_index) + static_cast<std::uint64_t>(s.num_triangles) * 3 >
                        lod.indices.size())
                {
                    throw SerializationError("skeletal section exceeds the index buffer");
                }
            }
            for (const std::uint32_t i : lod.indices)
            {
                if (i >= lod.vertices.size())
                {
                    throw SerializationError(fmt::format("skeletal index {} exceeds {} vertices", i, lod.vertices.size()));
                }
            }
            return lod;
        }
    }

    SkeletalMesh read_skeletal_mesh(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        const AssetSource source = open_asset(linker, object, schema);
        if (!Text::iequals(source.class_name, "SkeletalMesh"))
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("'{}' is a {}, not a SkeletalMesh",
                                                                linker.qualified_path(object), source.class_name));
        }
        BinaryReader r = source.native_reader();
        const Package& package = *object.package;
        Rules rules;
        rules.version = r.context().file_version;
        rules.assets = &package.profile().assets;
        if (rules.version < 493 || (rules.version >= 592 && rules.version < late_skeletal_mesh_version))
        {
            throw UnsupportedError(fmt::format("SkeletalMesh layout of file version {} is not supported", rules.version));
        }
        rules.chunk_uv_sets = rules.version >= 709 ? 4 : std::max(1, int{rules.assets->skeletal_chunk_uv_sets});
        rules.late = rules.version >= late_skeletal_mesh_version;
        rules.has_vertex_colors = source.properties.get_bool("bHasVertexColors").value_or(false);

        SkeletalMesh mesh;
        mesh.name = package.object_name(object.index);
        if (rules.assets->skeletal_mesh_leading_int) mesh.leading_value = r.read_i32();
        mesh.bounds = read_bounds(r);
        const std::uint32_t materials = r.read_array_count(4);
        for (std::uint32_t i = 0; i < materials; ++i) mesh.materials.push_back(ObjectIndex{r.read_i32()});
        mesh.origin = read_vec3(r);
        for (auto& a : mesh.rotation_origin) a = r.read_i32();
        const std::uint32_t bones = r.read_array_count(48);
        for (std::uint32_t i = 0; i < bones; ++i) mesh.skeleton.push_back(read_bone(r, package.names(), rules.version));
        mesh.skeletal_depth = r.read_i32();
        const std::uint32_t lods = r.read_array_count(64);
        for (std::uint32_t i = 0; i < lods; ++i) mesh.lods.push_back(read_lod(r, rules));
        const std::uint32_t names = r.read_array_count(12);
        for (std::uint32_t i = 0; i < names; ++i)
        {
            std::string name = package.names().to_string(read_fname(r));
            mesh.name_index_map.emplace_back(std::move(name), r.read_i32());
        }
        if (rules.version >= 435)
        {
            // PerPolyKDOPs (UE Viewer: "PerPolyKDOPs (ArVer >= 435)"): per-bone collision data, each a kDOP tree (nodes
            // and triangles, bulk arrays with their own element sizes) and the collision vertices. Kept as counts.
            const std::uint32_t count = r.read_array_count(20);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                PerPolyCollision c;
                const BulkArrayHeader nodes = read_bulk_array_header(r, 0, "SkeletalMesh PerPolyKDOP nodes");
                const auto node_bytes = r.read_view(static_cast<std::size_t>(nodes.count) * nodes.element_size);
                const BulkArrayHeader tris = read_bulk_array_header(r, 0, "SkeletalMesh PerPolyKDOP triangles");
                const auto tri_bytes = r.read_view(static_cast<std::size_t>(tris.count) * tris.element_size);
                c.node_size = nodes.element_size;
                c.triangle_size = tris.element_size;
                c.node_bytes.assign(node_bytes.begin(), node_bytes.end());
                c.triangle_bytes.assign(tri_bytes.begin(), tri_bytes.end());
                c.kdop_nodes = static_cast<std::int32_t>(nodes.count);
                c.kdop_triangles = static_cast<std::int32_t>(tris.count);
                const std::uint32_t verts = r.read_array_count(12);
                for (std::uint32_t v = 0; v < verts; ++v) c.vertices.push_back(read_vec3(r));
                mesh.per_poly_collision.push_back(std::move(c));
            }
        }
        if (rules.late)
        {
            const std::uint32_t break_names = r.read_array_count(4);
            for (std::uint32_t i = 0; i < break_names; ++i) mesh.bone_break_names.push_back(r.read_string());
            mesh.bone_break_options = read_u8_array(r);
            mesh.late_lod_entries = r.read_array_count(4);
            for (std::size_t i = 0; i < mesh.late_lod_entries; ++i)
            {
                if (const std::int32_t n = r.read_i32(); n != 0)
                {
                    throw UnsupportedError(fmt::format("SkeletalMesh per-LOD array entry with {} elements is not "
                                                       "supported", n));
                }
            }
            const std::uint32_t factors = r.read_array_count(4);
            for (std::uint32_t i = 0; i < factors; ++i) mesh.streaming_texture_factors.push_back(r.read_f32());
            mesh.late_trailer = r.read_i32();
        }
        expect_end(r, linker.qualified_path(object));

        for (const MeshBone& b : mesh.skeleton)
        {
            if (b.parent_index < 0 || static_cast<std::size_t>(b.parent_index) >= mesh.skeleton.size())
            {
                throw SerializationError(fmt::format("bone '{}' has parent {}", b.name, b.parent_index));
            }
        }
        for (const SkeletalLod& lod : mesh.lods)
        {
            for (const SkinnedVertex& v : lod.vertices)
            {
                for (const std::int16_t b : v.bones)
                {
                    if (b < 0 || static_cast<std::size_t>(b) >= mesh.skeleton.size())
                    {
                        throw SerializationError(fmt::format("vertex bone {} exceeds {} bones", b, mesh.skeleton.size()));
                    }
                }
            }
        }
        mesh.properties = source.properties;
        return mesh;
    }

    namespace
    {
        void write_u16_array(BinaryWriter& w, const std::vector<std::uint16_t>& values)
        {
            w.write_i32(static_cast<std::int32_t>(values.size()));
            for (const std::uint16_t v : values) w.write_u16(v);
        }

        void write_i16_array(BinaryWriter& w, const std::vector<std::int16_t>& values)
        {
            w.write_i32(static_cast<std::int32_t>(values.size()));
            for (const std::int16_t v : values) w.write_i16(v);
        }

        void write_u8_array(BinaryWriter& w, const std::vector<std::uint8_t>& values)
        {
            w.write_i32(static_cast<std::int32_t>(values.size()));
            for (const std::uint8_t v : values) w.write_u8(v);
        }

        /// The chunk-local index of skeleton bone @p bone (its first entry in the chunk's bone map).
        std::uint8_t local_bone(const SkeletalChunk& chunk, std::int16_t bone, std::size_t vertex)
        {
            const auto at = std::ranges::find(chunk.bone_map, bone);
            if (at == chunk.bone_map.end() || at - chunk.bone_map.begin() > 255)
            {
                throw SerializationError(
                    fmt::format("skeletal vertex {} uses bone {}, which its chunk doesn't map", vertex, bone));
            }
            return static_cast<std::uint8_t>(at - chunk.bone_map.begin());
        }

        void write_skeletal_lod(BinaryWriter& w, const SkeletalLod& lod, const AssetRules& rules, std::uint16_t version,
                                std::uint64_t base_offset)
        {
            w.write_i32(static_cast<std::int32_t>(lod.sections.size()));
            for (const SkeletalSection& s : lod.sections)
            {
                w.write_i16(s.material_index);
                w.write_i16(s.chunk_index);
                w.write_i32(s.first_index);
                w.write_u16(static_cast<std::uint16_t>(s.num_triangles));
            }
            write_bulk_array_header(w, 2, lod.indices.size());
            for (const std::uint32_t i : lod.indices) w.write_u16(static_cast<std::uint16_t>(i));
            write_u16_array(w, lod.shadow_indices);
            write_i16_array(w, lod.active_bones);
            write_u8_array(w, lod.shadow_triangle_double_sided);
            const std::size_t uv_bytes = static_cast<std::size_t>(std::max(1, int{rules.skeletal_chunk_uv_sets})) * 8;
            w.write_i32(static_cast<std::int32_t>(lod.chunks.size()));
            for (const SkeletalChunk& c : lod.chunks)
            {
                w.write_i32(c.first_vertex);
                w.write_i32(static_cast<std::int32_t>(c.stored_rigid_bytes.size() / (12 + 12 + uv_bytes + 1)));
                w.write_bytes(c.stored_rigid_bytes);
                w.write_i32(static_cast<std::int32_t>(c.stored_soft_bytes.size() / (12 + 12 + uv_bytes + 8)));
                w.write_bytes(c.stored_soft_bytes);
                write_i16_array(w, c.bone_map);
                w.write_i32(c.rigid_vertices);
                w.write_i32(c.soft_vertices);
                w.write_i32(c.max_influences);
            }
            w.write_i32(lod.size);
            w.write_i32(lod.num_vertices);
            if (lod.edges.size() % 16 != 0) throw SerializationError("SkeletalMesh edges are not 16 bytes each");
            w.write_i32(static_cast<std::int32_t>(lod.edges.size() / 16));
            w.write_bytes(lod.edges);
            write_u8_array(w, lod.required_bones);
            // A stored offset that isn't the payload's position (an empty record's, a cooker quirk, see
            // write_static_mesh; or a package saved again by another tool) is kept; it is computed only for a new mesh.
            BulkData raw = lod.raw_point_indices;
            const bool keep_offset = raw.payload_offset != 0 && raw.offset_mismatch();
            if ((raw.size_on_disk == 0 && raw.offset_in_file != 0 && lod.raw_point_index_bytes.empty()) || keep_offset)
            {
                w.write_u32(raw.flags);
                w.write_i32(raw.element_count);
                w.write_i32(static_cast<std::int32_t>(lod.raw_point_index_bytes.size()));
                w.write_i32(raw.offset_in_file);
                w.write_bytes(lod.raw_point_index_bytes);
            }
            else write_bulk_data(w, raw, lod.raw_point_index_bytes, base_offset);

            if (rules.skeletal_gpu_uv_count) w.write_i32(lod.num_uv_sets);
            else if (lod.num_uv_sets != 1)
            {
                throw SerializationError(fmt::format("this game's skeletal meshes have 1 UV set, not {}", lod.num_uv_sets));
            }
            w.write_bool32(lod.full_precision_uvs);
            const std::uint32_t vertex_size =
                12 + 8 + 8 + (lod.full_precision_uvs ? 8u : 4u) * static_cast<std::uint32_t>(lod.num_uv_sets);
            write_bulk_array_header(w, vertex_size, lod.vertices.size());
            // A vertex's chunk: the one whose range holds it
            std::vector<const SkeletalChunk*> chunk_of(lod.vertices.size(), nullptr);
            for (const SkeletalChunk& c : lod.chunks)
            {
                const auto end = static_cast<std::size_t>(c.first_vertex + c.rigid_vertices + c.soft_vertices);
                for (auto v = static_cast<std::size_t>(c.first_vertex); v < end && v < chunk_of.size(); ++v)
                {
                    chunk_of[v] = &c;
                }
            }
            for (std::size_t i = 0; i < lod.vertices.size(); ++i)
            {
                const SkinnedVertex& v = lod.vertices[i];
                if (!chunk_of[i]) throw SerializationError(fmt::format("skeletal vertex {} is in no chunk", i));
                write_vec3(w, v.position);
                const bool packed = v.packed_tangent != 0 || v.packed_normal != 0;
                w.write_u32(packed ? v.packed_tangent : pack_normal(v.tangent));
                w.write_u32(packed ? v.packed_normal : pack_normal(v.normal));
                for (const std::int16_t b : v.bones) w.write_u8(local_bone(*chunk_of[i], b, i));
                for (const std::uint8_t weight : v.weights) w.write_u8(weight);
                for (int k = 0; k < lod.num_uv_sets; ++k)
                {
                    const Vec2& uv = v.uv[static_cast<std::size_t>(k)];
                    if (lod.full_precision_uvs)
                    {
                        w.write_f32(uv.u);
                        w.write_f32(uv.v);
                    }
                    else
                    {
                        w.write_u16(float_to_half(uv.u));
                        w.write_u16(float_to_half(uv.v));
                    }
                }
            }
            if (!rules.skeletal_lod_ends_after_gpu_skin && version >= 534)
            {
                throw UnsupportedError("skeletal ExtraVertexInfluences are not supported");
            }
        }
    }

    std::vector<std::byte> write_skeletal_mesh(const SkeletalMesh& mesh, const SerializationContext& context,
                                               const AssetRules& rules,
                                               const std::function<FName(std::string_view)>& make_name,
                                               std::uint64_t base_offset)
    {
        const std::uint16_t version = context.file_version;
        if (version < 493 || version >= 592)
        {
            throw UnsupportedError(
                fmt::format("writing the SkeletalMesh layout of file version {} is not supported", version));
        }
        BinaryWriter w(context);
        if (rules.skeletal_mesh_leading_int) w.write_i32(mesh.leading_value);
        write_vec3(w, mesh.bounds.origin);
        write_vec3(w, mesh.bounds.box_extent);
        w.write_f32(mesh.bounds.sphere_radius);
        w.write_i32(static_cast<std::int32_t>(mesh.materials.size()));
        for (const ObjectIndex m : mesh.materials) w.write_i32(m.value);
        write_vec3(w, mesh.origin);
        for (const std::int32_t a : mesh.rotation_origin) w.write_i32(a);
        w.write_i32(static_cast<std::int32_t>(mesh.skeleton.size()));
        for (const MeshBone& b : mesh.skeleton)
        {
            write_fname(w, make_name(b.name));
            w.write_u32(b.flags);
            w.write_f32(b.orientation.x);
            w.write_f32(b.orientation.y);
            w.write_f32(b.orientation.z);
            w.write_f32(b.orientation.w);
            write_vec3(w, b.position);
            w.write_i32(b.num_children);
            w.write_i32(b.parent_index);
            if (version >= 515)
            {
                w.write_u8(b.color.b);
                w.write_u8(b.color.g);
                w.write_u8(b.color.r);
                w.write_u8(b.color.a);
            }
        }
        w.write_i32(mesh.skeletal_depth);
        w.write_i32(static_cast<std::int32_t>(mesh.lods.size()));
        for (const SkeletalLod& lod : mesh.lods) write_skeletal_lod(w, lod, rules, version, base_offset);
        w.write_i32(static_cast<std::int32_t>(mesh.name_index_map.size()));
        for (const auto& [name, index] : mesh.name_index_map)
        {
            write_fname(w, make_name(name));
            w.write_i32(index);
        }
        if (version >= 435)
        {
            w.write_i32(static_cast<std::int32_t>(mesh.per_poly_collision.size()));
            for (const PerPolyCollision& c : mesh.per_poly_collision)
            {
                write_bulk_array_header(w, c.node_size, c.node_size ? c.node_bytes.size() / c.node_size : 0);
                w.write_bytes(c.node_bytes);
                write_bulk_array_header(w, c.triangle_size,
                                        c.triangle_size ? c.triangle_bytes.size() / c.triangle_size : 0);
                w.write_bytes(c.triangle_bytes);
                w.write_i32(static_cast<std::int32_t>(c.vertices.size()));
                for (const Vec3& v : c.vertices) write_vec3(w, v);
            }
        }
        return w.take();
    }

    namespace
    {
        void chunk_header(BinaryWriter& w, const char* id, std::int32_t size, std::int32_t count)
        {
            const std::string_view text(id);
            for (std::size_t i = 0; i < 20; ++i)
            {
                w.write_u8(i < text.size() ? static_cast<std::uint8_t>(text[i]) : std::uint8_t{0});
            }
            w.write_u32(20100422);
            w.write_i32(size);
            w.write_i32(count);
        }

        void fixed_string(BinaryWriter& w, std::string_view text, std::size_t size)
        {
            for (std::size_t i = 0; i < size; ++i)
            {
                w.write_u8(i + 1 < size && i < text.size() ? static_cast<std::uint8_t>(text[i]) : std::uint8_t{0});
            }
        }
    }

    std::vector<std::byte> to_psk(const Package& package, const SkeletalMesh& mesh, std::size_t lod_index)
    {
        if (lod_index >= mesh.lods.size())
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("'{}' has {} LODs", mesh.name, mesh.lods.size()));
        }
        const SkeletalLod& lod = mesh.lods[lod_index];
        if (lod.vertices.size() > 65536)
        {
            throw UnsupportedError("PSK export supports at most 65536 vertices");
        }
        BinaryWriter w;
        chunk_header(w, "ACTRHEAD", 0, 0);

        // One point and one wedge per vertex.
        chunk_header(w, "PNTS0000", 12, static_cast<std::int32_t>(lod.vertices.size()));
        for (const SkinnedVertex& v : lod.vertices)
        {
            w.write_f32(v.position.x);
            w.write_f32(-v.position.y);
            w.write_f32(v.position.z);
        }
        std::vector<std::uint8_t> wedge_material(lod.vertices.size(), 0);
        std::size_t faces = 0;
        for (std::size_t s = 0; s < lod.sections.size(); ++s)
        {
            const SkeletalSection& sec = lod.sections[s];
            faces += static_cast<std::size_t>(sec.num_triangles);
            for (std::int32_t i = 0; i < sec.num_triangles * 3; ++i)
            {
                wedge_material[lod.indices[static_cast<std::size_t>(sec.first_index + i)]] = static_cast<std::uint8_t>(s);
            }
        }
        chunk_header(w, "VTXW0000", 16, static_cast<std::int32_t>(lod.vertices.size()));
        for (std::size_t i = 0; i < lod.vertices.size(); ++i)
        {
            w.write_u32(static_cast<std::uint32_t>(i));
            w.write_f32(lod.vertices[i].uv[0].u);
            w.write_f32(lod.vertices[i].uv[0].v);
            w.write_u8(wedge_material[i]);
            w.write_u8(0);
            w.write_u16(0);
        }
        chunk_header(w, "FACE0000", 12, static_cast<std::int32_t>(faces));
        for (std::size_t s = 0; s < lod.sections.size(); ++s)
        {
            const SkeletalSection& sec = lod.sections[s];
            for (std::int32_t t = 0; t < sec.num_triangles; ++t)
            {
                const auto at = [&](int k) {
                    // PSK wedge indices are 16-bit; to_psk() rejects meshes above 65536 vertices.
                    return static_cast<std::uint16_t>(lod.indices[static_cast<std::size_t>(sec.first_index + t * 3 + k)]);
                };
                w.write_u16(at(1)); // mirrored: swap the first two corners
                w.write_u16(at(0));
                w.write_u16(at(2));
                w.write_u8(static_cast<std::uint8_t>(s));
                w.write_u8(0);
                w.write_u32(1);
            }
        }
        chunk_header(w, "MATT0000", 88, static_cast<std::int32_t>(lod.sections.size()));
        for (const SkeletalSection& sec : lod.sections)
        {
            std::string name = "None";
            const auto m = static_cast<std::size_t>(sec.material_index);
            if (m < mesh.materials.size() && package.is_valid(mesh.materials[m]) && !mesh.materials[m].is_null())
            {
                name = package.object_name(mesh.materials[m]);
            }
            fixed_string(w, name, 64);
            for (int i = 0; i < 6; ++i) w.write_i32(0);
        }
        chunk_header(w, "REFSKELT", 120, static_cast<std::int32_t>(mesh.skeleton.size()));
        for (std::size_t i = 0; i < mesh.skeleton.size(); ++i)
        {
            const MeshBone& b = mesh.skeleton[i];
            fixed_string(w, b.name, 64);
            w.write_u32(0);
            std::int32_t children = 0;
            for (std::size_t j = 0; j < mesh.skeleton.size(); ++j)
            {
                if (j != i && mesh.skeleton[j].parent_index == static_cast<std::int32_t>(i)) ++children;
            }
            w.write_i32(children);
            w.write_i32(b.parent_index);
            // UE Viewer convention: W negated for non-root bones, then mirrored (Y and W negated).
            const float sign_w = i == 0 ? -1.0f : 1.0f;
            w.write_f32(b.orientation.x);
            w.write_f32(-b.orientation.y);
            w.write_f32(b.orientation.z);
            w.write_f32(sign_w * b.orientation.w);
            w.write_f32(b.position.x);
            w.write_f32(-b.position.y);
            w.write_f32(b.position.z);
            for (int k = 0; k < 4; ++k) w.write_f32(0.0f); // length, size
        }
        std::int32_t influences = 0;
        for (const SkinnedVertex& v : lod.vertices)
        {
            for (const std::uint8_t weight : v.weights) influences += weight != 0 ? 1 : 0;
        }
        chunk_header(w, "RAWWEIGHTS", 12, influences);
        for (std::size_t i = 0; i < lod.vertices.size(); ++i)
        {
            const SkinnedVertex& v = lod.vertices[i];
            for (std::size_t k = 0; k < 4; ++k)
            {
                if (v.weights[k] == 0) continue;
                w.write_f32(static_cast<float>(v.weights[k]) / 255.0f);
                w.write_i32(static_cast<std::int32_t>(i));
                w.write_i32(v.bones[k]);
            }
        }
        return w.take();
    }
}
