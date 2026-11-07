#pragma once

/// @file
/// @brief Brush geometry: UPolys (the editable polygons of a brush) and the prefix of UModel (BSP data up to its
///        Polys reference).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <UnrealLib/Assets/AssetObject.hpp>
#include <UnrealLib/Assets/MeshTypes.hpp>

namespace UnrealLib::Assets
{
    /// @brief FPoly: one polygon of a brush.
    ///
    /// Layout (upstream `Poly.Deserialize`, verified on Mirror's Edge, version 536: 136 bytes per 4-vertex polygon):
    /// [CompactIndex vertex count < 227], Base, Normal, TextureU, TextureV, Vertices (TArray<FVector> >= 227),
    /// PolyFlags, Actor, [Texture < 170], ItemName, [Material >= 170], iLink, iBrushPoly, [LightMapScale 106..299],
    /// [ShadowMapScale >= 300], [LightingChannels >= 417].
    struct Poly
    {
        Vec3 base;                       ///< Origin of the texture mapping (`Origin` in T3D).
        Vec3 normal;                     ///< Plane normal.
        Vec3 texture_u;                  ///< Texture U axis.
        Vec3 texture_v;                  ///< Texture V axis.
        std::vector<Vec3> vertices;      ///< Vertices, in order.
        std::uint32_t poly_flags = 0;    ///< EPolyFlags.
        ObjectIndex actor;               ///< Brush actor the polygon belongs to (often null when cooked).
        std::string item_name;           ///< ItemName ("None" if unset).
        ObjectIndex material;            ///< Material (null if none).
        std::int32_t link = -1;          ///< iLink: index of the polygon this one was split from (-1 = none).
        std::int32_t brush_poly = -1;    ///< iBrushPoly.
        float light_map_scale = 32.0f;   ///< LightMapScale (versions 106–299 only).
        float shadow_map_scale = 32.0f;  ///< ShadowMapScale (version >= 300).
        std::uint32_t lighting_channels = 0; ///< FLightingChannelContainer bits, as stored (version >= 417).
    };

    /// @brief A UPolys export.
    struct Polys
    {
        std::string name;               ///< Object name.
        ObjectIndex element_owner;      ///< ElementOwner (version >= 321): owner of the polygon array — the Polys
                                        ///< itself in most Mirror's Edge packages, the outer Model in others.
        std::vector<Poly> elements;     ///< Polygons.
        std::int32_t max = 0;           ///< Array capacity as stored (written as max(max, element count)).
        std::size_t native_offset = 0;  ///< Offset of the native data (the element count) in the export data.
    };

    /// @brief FBspSurf: a BSP surface (56 bytes in version 536).
    struct BspSurface
    {
        ObjectIndex material;           ///< Material.
        std::uint32_t poly_flags = 0;   ///< EPolyFlags.
        std::int32_t base = 0;          ///< pBase: index into Model::points.
        std::int32_t normal = 0;        ///< vNormal: index into Model::vectors.
        std::int32_t texture_u = 0;     ///< vTextureU: index into Model::vectors.
        std::int32_t texture_v = 0;     ///< vTextureV: index into Model::vectors.
        std::int32_t brush_poly = -1;   ///< iBrushPoly: index into the brush's Polys.
        ObjectIndex actor;              ///< Brush actor.
        std::array<float, 4> plane{};   ///< Plane (X, Y, Z, W).
        float shadow_map_scale = 32.0f; ///< ShadowMapScale.
        std::uint32_t lighting_channels = 0; ///< FLightingChannelContainer bits, as stored.
    };

    /// @brief FZoneProperties: one zone of a BSP model (24 bytes).
    struct ZoneProperties
    {
        ObjectIndex zone_actor;          ///< ZoneActor (often null when cooked).
        std::uint64_t connectivity = 0;  ///< Connectivity: bit set of zones this zone is connected to.
        std::uint64_t visibility = 0;    ///< Visibility: bit set of zones visible from this zone.
        float last_render_time = 0.0f;   ///< LastRenderTime.
    };

    /// @brief The leading part of a UModel export, up to and including its Polys reference.
    ///
    /// Layout (verified on every Model of Mirror's Edge, version 536): Bounds, Vectors (bulk FVector), Points (bulk
    /// FVector), Nodes (bulk FBspNode), Surfs (owner reference, count, FBspSurf...), Verts (bulk FVert),
    /// NumSharedSides, NumZones, Zones[NumZones] (FZoneProperties), Polys. The data after Polys (leaf hulls, leaves, portal nodes,
    /// vertex buffer, lighting data) is not decoded; its offset is `unparsed_offset`.
    struct Model
    {
        std::string name;                 ///< Object name.
        BoxSphereBounds bounds;           ///< Bounds.
        std::vector<Vec3> vectors;        ///< Normals and texture axes referenced by surfaces.
        std::vector<Vec3> points;         ///< Points referenced by surfaces and vertices.
        std::int32_t node_count = 0;      ///< Number of FBspNode (not decoded).
        std::uint32_t node_size = 0;      ///< Stored FBspNode size.
        ObjectIndex surface_owner;        ///< Owner stored with the Surfs array (the model itself).
        std::vector<BspSurface> surfaces; ///< BSP surfaces.
        std::int32_t vert_count = 0;      ///< Number of FVert (not decoded).
        std::uint32_t vert_size = 0;      ///< Stored FVert size.
        std::int32_t num_shared_sides = 0; ///< NumSharedSides.
        std::vector<ZoneProperties> zones; ///< Zones (NumZones entries).
        ObjectIndex polys;                ///< The brush polygons (null if none).
        std::size_t unparsed_offset = 0;  ///< Offset in the export data where decoding stopped.
    };

    /// @brief Reads a Polys export.
    /// @param linker Resolves the class hierarchy.
    /// @param object The Polys export.
    /// @param schema Type information (nullptr: BuiltinPropertySchema).
    /// @throws Error (InvalidArgument) if @p object is not a Polys; UnsupportedError for layouts that were not
    ///         verified (version < 78: PanU/PanV; version >= 600: Lightmass settings); SerializationError on bad data or trailing bytes.
    [[nodiscard]] Polys read_polys(Linker& linker, ObjectRef object, const PropertySchema* schema = nullptr);

    /// @brief Serializes the native data of a Polys export (everything from the element count on), the inverse
    ///        of read_polys(): writing an unmodified Polys reproduces its native bytes exactly.
    /// @param polys     The polygons.
    /// @param context   Version information (the package's file version).
    /// @param make_name Returns the FName of an ItemName text in the target package's name table.
    /// @throws UnsupportedError for the file versions read_polys() rejects.
    [[nodiscard]] std::vector<std::byte> write_polys(const Polys& polys, const SerializationContext& context,
                                                     const std::function<FName(std::string_view)>& make_name);

    /// @brief Reads the leading part of a Model export (see Model).
    /// @param linker Resolves the class hierarchy.
    /// @param object The Model export.
    /// @param schema Type information (nullptr: BuiltinPropertySchema).
    /// @throws Error (InvalidArgument) if @p object is not a Model; UnsupportedError for file versions other than 536;
    ///         SerializationError on bad data.
    [[nodiscard]] Model read_model(Linker& linker, ObjectRef object, const PropertySchema* schema = nullptr);
}
