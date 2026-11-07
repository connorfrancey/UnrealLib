#include <UnrealLib/Assets/Brush.hpp>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Utilities/Text.hpp>

#include "MeshReading.hpp"

namespace UnrealLib::Assets
{
    using namespace detail;

    namespace
    {
        // Version gates of FPoly fields (upstream PackageObjectLegacyVersion).
        constexpr std::uint16_t kPanUVRemoved = 78;
        constexpr std::uint16_t kLightMapScaleAdded = 106;
        constexpr std::uint16_t kTextureReplacedByMaterial = 170;
        constexpr std::uint16_t kVerticesToArray = 227;
        constexpr std::uint16_t kShadowMapScaleAdded = 300; // LightMapScale removed at the same version
        constexpr std::uint16_t kElementOwnerAdded = 321;
        constexpr std::uint16_t kLightingChannelsAdded = 417;
        constexpr std::uint16_t kLightmassAdded = 600;

        /// Smallest serialized FPoly at version >= 227: 4 vectors, an empty array, flags, actor, name, material,
        /// link, brush poly.
        constexpr std::size_t kMinPolySize = 4 * 12 + 4 + 4 + 4 + 8 + 4 + 4 + 4;

        void require_class(Linker& linker, ObjectRef object, const AssetSource& source, std::string_view expected)
        {
            if (!Text::iequals(source.class_name, expected))
            {
                throw Error(ErrorCode::InvalidArgument, fmt::format("'{}' is a {}, not a {}",
                                                                    linker.qualified_path(object), source.class_name,
                                                                    expected));
            }
        }

        Poly read_poly(BinaryReader& r, const NameTable& names)
        {
            const std::uint16_t version = r.context().file_version;
            Poly p;
            std::int32_t vertex_count = -1;
            if (version < kVerticesToArray)
            {
                vertex_count = r.read_compact_index();
                if (vertex_count < 0 || static_cast<std::size_t>(vertex_count) * 12 > r.remaining())
                {
                    throw SerializationError(fmt::format("FPoly with {} vertices", vertex_count),
                                             r.absolute_position());
                }
            }
            p.base = read_vec3(r);
            p.normal = read_vec3(r);
            p.texture_u = read_vec3(r);
            p.texture_v = read_vec3(r);
            const std::uint32_t n =
                vertex_count >= 0 ? static_cast<std::uint32_t>(vertex_count) : r.read_array_count(12);
            p.vertices.resize(n);
            for (Vec3& v : p.vertices) v = read_vec3(r);
            p.poly_flags = r.read_u32();
            p.actor = ObjectIndex{r.read_i32()};
            if (version < kTextureReplacedByMaterial) p.material = ObjectIndex{r.read_i32()};
            p.item_name = names.to_string(read_fname(r));
            if (version >= kTextureReplacedByMaterial) p.material = ObjectIndex{r.read_i32()};
            p.link = r.read_index();
            p.brush_poly = r.read_index();
            if (version >= kLightMapScaleAdded && version < kShadowMapScaleAdded) p.light_map_scale = r.read_f32();
            if (version >= kShadowMapScaleAdded) p.shadow_map_scale = r.read_f32();
            if (version >= kLightingChannelsAdded) p.lighting_channels = r.read_u32();
            return p;
        }

        BspSurface read_surface(BinaryReader& r)
        {
            BspSurface s;
            s.material = ObjectIndex{r.read_i32()};
            s.poly_flags = r.read_u32();
            s.base = r.read_i32();
            s.normal = r.read_i32();
            s.texture_u = r.read_i32();
            s.texture_v = r.read_i32();
            s.brush_poly = r.read_i32();
            s.actor = ObjectIndex{r.read_i32()};
            for (float& f : s.plane) f = r.read_f32();
            s.shadow_map_scale = r.read_f32();
            s.lighting_channels = r.read_u32();
            return s;
        }

        std::vector<Vec3> read_vectors(BinaryReader& r, std::string_view what)
        {
            const BulkArrayHeader h = read_bulk_array_header(r, 12, what);
            std::vector<Vec3> out(h.count);
            for (Vec3& v : out) v = read_vec3(r);
            return out;
        }
    }

    Polys read_polys(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        const AssetSource source = open_asset(linker, object, schema);
        require_class(linker, object, source, "Polys");
        BinaryReader r = source.native_reader();
        const std::uint16_t version = r.context().file_version;
        if (version < kPanUVRemoved || version >= kLightmassAdded)
        {
            throw UnsupportedError(fmt::format("FPoly layout of file version {} is not supported", version));
        }
        Polys polys;
        polys.name = object.package->object_name(object.index);
        const std::int32_t num = r.read_i32();
        const std::int32_t max = r.read_i32();
        if (num < 0 || max < num)
        {
            throw SerializationError(fmt::format("Polys with Num={} Max={}", num, max), r.absolute_position());
        }
        if (version >= kElementOwnerAdded) polys.element_owner = ObjectIndex{r.read_i32()};
        if (static_cast<std::uint64_t>(num) * kMinPolySize > r.remaining())
        {
            throw SerializationError(fmt::format("Polys: {} polygons exceed the data", num), r.absolute_position());
        }
        polys.elements.reserve(static_cast<std::size_t>(num));
        for (std::int32_t i = 0; i < num; ++i) polys.elements.push_back(read_poly(r, object.package->names()));
        expect_end(r, linker.qualified_path(object));
        return polys;
    }

    Model read_model(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        const AssetSource source = open_asset(linker, object, schema);
        require_class(linker, object, source, "Model");
        BinaryReader r = source.native_reader();
        const std::uint16_t version = r.context().file_version;
        if (version != 536)
        {
            throw UnsupportedError(fmt::format("Model layout of file version {} is not supported", version));
        }
        Model m;
        m.name = object.package->object_name(object.index);
        m.bounds = read_bounds(r);
        m.vectors = read_vectors(r, "Model vectors");
        m.points = read_vectors(r, "Model points");
        const BulkArrayHeader nodes = read_bulk_array_header(r, 0, "Model nodes");
        m.node_count = static_cast<std::int32_t>(nodes.count);
        m.node_size = nodes.element_size;
        r.skip(static_cast<std::size_t>(nodes.count) * nodes.element_size);
        m.surface_owner = ObjectIndex{r.read_i32()};
        const std::uint32_t surfaces = r.read_array_count(56);
        m.surfaces.reserve(surfaces);
        for (std::uint32_t i = 0; i < surfaces; ++i) m.surfaces.push_back(read_surface(r));
        const BulkArrayHeader verts = read_bulk_array_header(r, 0, "Model verts");
        m.vert_count = static_cast<std::int32_t>(verts.count);
        m.vert_size = verts.element_size;
        r.skip(static_cast<std::size_t>(verts.count) * verts.element_size);
        m.num_shared_sides = r.read_i32();
        const std::int32_t num_zones = r.read_i32();
        // UE3 caps zones at 64 (FZoneSet is a 64-bit mask).
        if (num_zones < 0 || num_zones > 64)
        {
            throw SerializationError(fmt::format("{}: Model with {} zones", linker.qualified_path(object), num_zones),
                                     r.absolute_position());
        }
        m.zones.resize(static_cast<std::size_t>(num_zones));
        for (ZoneProperties& z : m.zones)
        {
            z.zone_actor = ObjectIndex{r.read_i32()};
            z.connectivity = r.read_u64();
            z.visibility = r.read_u64();
            z.last_render_time = r.read_f32();
        }
        m.polys = ObjectIndex{r.read_i32()};
        m.unparsed_offset = r.position();
        return m;
    }
}
