#pragma once

/// @file
/// @brief UE3 SkeletalMesh: reference skeleton, LOD models with skinned vertices, and ActorX PSK export.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <UnrealLib/Assets/AssetObject.hpp>
#include <UnrealLib/Assets/BulkData.hpp>
#include <UnrealLib/Assets/MeshTypes.hpp>

namespace UnrealLib::Assets
{
    /// @brief A reference skeleton bone (FMeshBone).
    struct MeshBone
    {
        std::string name;            ///< Bone name.
        std::uint32_t flags = 0;     ///< Flags.
        Quat orientation;            ///< Local rotation (relative to the parent).
        Vec3 position;               ///< Local translation.
        std::int32_t num_children = 0; ///< Number of child bones.
        std::int32_t parent_index = 0; ///< Parent bone (the root is its own parent: 0).
        Color color;                 ///< Editor color (version >= 515).
    };

    /// @brief A section: one material over a range of the index buffer.
    struct SkeletalSection
    {
        std::int16_t material_index = 0; ///< Index into SkeletalMesh::materials.
        std::int16_t chunk_index = 0;    ///< Chunk whose bone map the section's vertices use.
        std::int32_t first_index = 0;    ///< First index.
        std::int32_t num_triangles = 0;  ///< Triangle count.
    };

    /// @brief A chunk: a vertex range with its own bone map (vertex bone indices are chunk-local).
    struct SkeletalChunk
    {
        std::int32_t first_vertex = 0;         ///< First vertex of the chunk.
        std::int32_t rigid_vertices = 0;       ///< NumRigidVertices.
        std::int32_t soft_vertices = 0;        ///< NumSoftVertices.
        std::size_t stored_rigid_vertices = 0; ///< Rigid vertices stored in the chunk (CPU copy; often 0 when cooked).
        std::size_t stored_soft_vertices = 0;  ///< Soft vertices stored in the chunk.
        std::vector<std::int16_t> bone_map;    ///< Chunk bone index -> skeleton bone index.
        std::int32_t max_influences = 0;       ///< Maximum influences per vertex.
    };

    /// @brief A GPU-skinned vertex. Bone indices are resolved to skeleton bones.
    struct SkinnedVertex
    {
        Vec3 position;                         ///< Position (reference pose).
        Vec3 tangent;                          ///< Tangent X.
        Vec3 normal;                           ///< Tangent Z.
        std::array<Vec2, 4> uv{};              ///< UV sets (SkeletalLod::num_uv_sets used).
        std::array<std::int16_t, 4> bones{};   ///< Skeleton bone of each influence.
        std::array<std::uint8_t, 4> weights{}; ///< Weights (0..255; sum 255).
    };

    /// @brief One LOD model.
    struct SkeletalLod
    {
        std::vector<SkeletalSection> sections;  ///< Sections.
        std::vector<std::uint16_t> indices;     ///< Triangle list.
        std::vector<std::uint16_t> shadow_indices; ///< ShadowIndices (version < 686).
        std::vector<std::int16_t> active_bones; ///< Bones used by this LOD.
        std::vector<std::uint8_t> shadow_triangle_double_sided; ///< Version < 686.
        std::vector<SkeletalChunk> chunks;      ///< Chunks.
        std::int32_t size = 0;                  ///< Size field.
        std::int32_t num_vertices = 0;          ///< NumVertices.
        std::int32_t num_edges = 0;             ///< Edge count (version < 686).
        std::vector<std::uint8_t> required_bones; ///< Bones required by this LOD.
        BulkData raw_point_indices;             ///< Source point indices (uncooked).
        std::int32_t num_uv_sets = 1;           ///< UV sets in the GPU vertices.
        bool full_precision_uvs = false;        ///< GPU vertex UVs stored as floats (else halves).
        std::vector<SkinnedVertex> vertices;    ///< GPU skin vertex buffer.
    };

    /// @brief Per-bone polygon collision (FPerPolyBoneCollisionData).
    struct PerPolyCollision
    {
        std::int32_t kdop_nodes = 0;     ///< kDOP tree nodes.
        std::int32_t kdop_triangles = 0; ///< kDOP tree triangles.
        std::vector<Vec3> vertices;      ///< Collision vertices.
    };

    /// @brief A SkeletalMesh export.
    ///
    /// Native layout (file version 536 with the Mirror's Edge AssetRules; verified on every Mirror's Edge skeletal
    /// mesh; format reference: UE Viewer): [licensee int32], FBoxSphereBounds, material references, origin,
    /// rotation origin, reference skeleton, skeletal depth, LOD models, a name-to-bone map, then PerPolyKDOPs
    /// (non-empty in a single Mirror's Edge mesh, which verifies that layout only once).
    struct SkeletalMesh
    {
        std::string name;                       ///< Object name.
        std::int32_t leading_value = 0;         ///< Licensee int32 before the bounds (Mirror's Edge; unknown).
        BoxSphereBounds bounds;                 ///< Bounds.
        std::vector<ObjectIndex> materials;     ///< Materials (indexed by sections).
        Vec3 origin;                            ///< MeshOrigin.
        std::array<std::int32_t, 3> rotation_origin{}; ///< RotOrigin (pitch, yaw, roll).
        std::vector<MeshBone> skeleton;         ///< Reference skeleton.
        std::int32_t skeletal_depth = 0;        ///< Depth of the bone hierarchy.
        std::vector<SkeletalLod> lods;          ///< LOD models.
        std::vector<std::pair<std::string, std::int32_t>> name_index_map; ///< Bone name -> index.
        std::vector<PerPolyCollision> per_poly_collision; ///< PerPolyKDOPs (version >= 435).
        AssetProperties properties;             ///< Tagged properties.
    };

    /// @brief Reads a SkeletalMesh export. The native data must be consumed exactly.
    [[nodiscard]] SkeletalMesh read_skeletal_mesh(Linker& linker, ObjectRef object,
                                                  const PropertySchema* schema = nullptr);

    /// @brief Writes LOD @p lod of @p mesh as an ActorX PSK file (points, wedges, faces, materials, skeleton,
    ///        weights). Like UE Viewer's exporter, the mesh is mirrored on Y (positions, bone positions, and
    ///        quaternion Y/W) and triangle winding swapped, so importers see a right-handed mesh.
    [[nodiscard]] std::vector<std::byte> to_psk(const Package& package, const SkeletalMesh& mesh, std::size_t lod = 0);
}
