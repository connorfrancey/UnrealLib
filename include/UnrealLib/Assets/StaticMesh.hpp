#pragma once

/// @file
/// @brief UE3 StaticMesh: render LODs (sections, vertices, indices) and collision data, plus OBJ export.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <UnrealLib/Assets/AssetObject.hpp>
#include <UnrealLib/Assets/BulkData.hpp>
#include <UnrealLib/Assets/MeshTypes.hpp>
#include <UnrealLib/Core/Guid.hpp>

namespace UnrealLib::Assets
{
    /// @brief A FStaticMeshElement / section: a material applied to a range of the index buffer.
    struct StaticMeshSection
    {
        ObjectIndex material;          ///< Material (null if none).
        bool enable_collision = false; ///< bEnableCollision.
        bool old_enable_collision = false; ///< OldEnableCollision.
        bool enable_shadow_casting = false; ///< bEnableShadowCasting (version >= 473).
        std::int32_t first_index = 0;  ///< First index into the LOD's index buffer.
        std::int32_t num_triangles = 0; ///< Triangle count.
        std::int32_t min_vertex = 0;   ///< Lowest vertex index used.
        std::int32_t max_vertex = 0;   ///< Highest vertex index used.
        std::int32_t material_index = 0; ///< Index of the section's material in the mesh (version >= 492).
        std::vector<std::array<std::int32_t, 2>> fragments; ///< Fragment ranges (version >= 514).
    };

    /// @brief One render LOD.
    struct StaticMeshLod
    {
        BulkData raw_triangles;          ///< Source triangles (uncooked; empty in cooked packages).
        std::vector<StaticMeshSection> sections; ///< Sections.
        std::vector<Vec3> positions;     ///< Vertex positions.
        std::vector<Vec3> tangents;      ///< Tangent X per vertex (unpacked).
        std::vector<Vec3> normals;       ///< Tangent Z (normal) per vertex (unpacked).
        /// Tangent X and Z as stored (FPackedNormal: x, y, z and a fourth byte the unpacked vectors drop; Z's holds the
        /// binormal's sign). write_static_mesh() writes these when there is one per vertex, else packs the vectors.
        std::vector<std::uint32_t> packed_tangents;
        std::vector<std::uint32_t> packed_normals;
        std::vector<Color> colors;       ///< Per-vertex color (UV stream color in versions 434..614; from 615 on
                                         ///< a separate color stream, empty when the mesh has none).
        std::int32_t num_uv_sets = 0;    ///< Texture coordinate sets.
        std::vector<std::vector<Vec2>> uvs; ///< `uvs[set][vertex]`.
        bool full_precision_uvs = true;  ///< UVs stored as floats (else halves).
        std::vector<float> shadow_extrusion; ///< Shadow volume stream (version < 686).
        std::int32_t num_vertices = 0;   ///< NumVertices field.
        std::vector<std::uint16_t> indices;  ///< Triangle list.
        std::vector<std::uint16_t> wireframe_indices; ///< Line list.
        std::vector<std::uint16_t> adjacency_indices; ///< Adjacency index buffer (12 per triangle; late UE3).
        std::int32_t num_edges = 0;      ///< Edges (version < 686).
        std::vector<std::byte> edges;    ///< The edges as stored, 16 bytes each (two vertex and two face indices).
        std::vector<std::uint8_t> shadow_triangle_double_sided; ///< ShadowTriangleDoubleSided (version < 686).
    };

    /// @brief A collision triangle of the kDOP tree: three vertex indices into LOD 0 and the section it belongs to.
    struct KDopTriangle
    {
        std::array<std::uint16_t, 3> vertices{};
        std::uint16_t material_index = 0;
    };

    /// @brief A node of the kDOP collision tree as stored before file version 868 (32 bytes): the box, then a 4-byte
    ///        bIsLeaf, then two 16-bit values (a leaf: its triangle count and first triangle; else its two children).
    struct KDopNode
    {
        Vec3 min;                ///< Box minimum.
        Vec3 max;                ///< Box maximum.
        bool leaf = false;       ///< bIsLeaf.
        std::uint16_t first = 0; ///< Leaf: NumTriangles; else LeftNode.
        std::uint16_t second = 0; ///< Leaf: StartIndex; else RightNode.
    };

    /// @brief A StaticMesh export.
    ///
    /// Native layout (file version 536, verified on every Mirror's Edge static mesh; format reference: UE Viewer):
    /// FBoxSphereBounds, BodySetup, kDOP nodes and triangles (bulk arrays), InternalVersion, a name array
    /// (InternalVersion >= 17, versions < 593), the LOD models, ThumbnailAngle (rotator), ThumbnailDistance, and, in
    /// games whose profile sets `AssetRules::static_mesh_trailing_float` (Mirror's Edge), one more float of unknown meaning.
    ///
    /// Late UE3 layout (file version 868, verified on A Story About My Uncle): the kDOP tree is the compact form
    /// (bounding box, 6-byte nodes, 8-byte triangles); InternalVersion is followed by four int32 that are zero in
    /// cooked meshes (source data, optimization settings, simplification and proxy flags; anything else is rejected
    /// as unsupported); sections end with a zero byte; the LOD has a color stream (item size, vertex count, colors
    /// only if any), no shadow stream or edges, and an adjacency index buffer; the mesh ends with an unidentified
    /// int32, ThumbnailAngle, ThumbnailDistance, HighResSourceMeshName, HighResSourceMeshCRC, LightingGuid,
    /// VertexPositionVersionNumber, CachedStreamingTextureFactors, bRemoveDegenerates and two more int32.
    struct StaticMesh
    {
        std::string name;                   ///< Object name.
        BoxSphereBounds bounds;             ///< Bounds.
        ObjectIndex body_setup;             ///< RB_BodySetup (collision), may be null.
        std::int32_t kdop_node_count = 0;   ///< Collision tree nodes.
        std::int32_t kdop_triangle_count = 0; ///< Collision triangles.
        std::vector<std::byte> kdop_nodes;  ///< The tree's nodes as stored (32 bytes each before file version 868).
        std::vector<KDopTriangle> kdop_triangles; ///< The tree's triangles, in leaf order.
        std::int32_t internal_version = 0;  ///< InternalVersion.
        std::vector<std::string> content_tags; ///< Name array after InternalVersion (InternalVersion >= 17, < 593).
        std::vector<StaticMeshLod> lods;    ///< Render LODs.
        std::array<std::int32_t, 3> thumbnail_angle{}; ///< Rotator (pitch, yaw, roll).
        float thumbnail_distance = 0;       ///< ThumbnailDistance.
        /// Unidentified trailing float present in every Mirror's Edge static mesh (licensee data: 0.0 in about half of
        /// them, otherwise mostly small whole numbers such as 3.0, 5.0 or 14.0). Kept verbatim.
        std::optional<float> trailing_float;
        // Late UE3 fields (file version 868).
        std::int32_t late_leading_value = 0;       ///< Unidentified int32 after the LODs (1 in every mesh seen).
        std::string high_res_source_mesh_name;     ///< HighResSourceMeshName.
        std::uint32_t high_res_source_mesh_crc = 0; ///< HighResSourceMeshCRC.
        Guid lighting_guid;                        ///< LightingGuid.
        std::int32_t vertex_position_version = 0;  ///< VertexPositionVersionNumber.
        std::vector<float> streaming_texture_factors; ///< CachedStreamingTextureFactors.
        bool remove_degenerates = false;           ///< bRemoveDegenerates.
        std::array<std::int32_t, 2> late_trailer{}; ///< The two int32 after bRemoveDegenerates (unidentified; zero).
        AssetProperties properties;         ///< Tagged properties.
    };

    /// @brief Reads a StaticMesh export. The native data must be consumed exactly.
    [[nodiscard]] StaticMesh read_static_mesh(Linker& linker, ObjectRef object, const PropertySchema* schema = nullptr);

    /// @brief Serializes the native data of a StaticMesh export (everything after the tagged properties), the inverse
    ///        of read_static_mesh(): writing an unmodified mesh reproduces its native bytes exactly.
    /// @param mesh        The mesh.
    /// @param context     Version information (the package's file version).
    /// @param base_offset Package offset at which the native data will be written (inline bulk data records store
    ///                    absolute offsets).
    /// @throws UnsupportedError for file versions outside 472..614 (Mirror's Edge is 536);
    ///         SerializationError if a stream's length doesn't match the vertex count.
    [[nodiscard]] std::vector<std::byte> write_static_mesh(const StaticMesh& mesh, const SerializationContext& context,
                                                           std::uint64_t base_offset);

    /// @brief The nodes of @p mesh's kDOP tree (early layout: 32 bytes a node). @throws SerializationError otherwise.
    [[nodiscard]] std::vector<KDopNode> decode_kdop_nodes(const StaticMesh& mesh);

    /// @brief Builds @p mesh's collision tree (kdop_nodes and kdop_triangles) from LOD 0's sections with collision
    ///        enabled, the way UE3's kDOP builder does: a node splits its triangles at the mean of their centroids on
    ///        the axis where they spread most, until a node has 5 or fewer (a leaf). @throws Error if there is no LOD.
    void build_kdop(StaticMesh& mesh);

    /// @brief Formats LOD @p lod of @p mesh as Wavefront OBJ: positions, normals, the first UV set, one group per
    ///        section (`usemtl` = material object name).
    ///
    /// Unreal is Z-up and left-handed; OBJ consumers expect Y-up and right-handed, so Y and Z are swapped. The swap
    /// mirrors the mesh, which also turns Unreal's clockwise front faces counter-clockwise, as OBJ expects. V is
    /// flipped (`1 - v`).
    [[nodiscard]] std::string to_obj(const Package& package, const StaticMesh& mesh, std::size_t lod = 0);
}
