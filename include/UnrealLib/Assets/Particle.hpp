#pragma once

/// @file
/// @brief UE3 particle system, emitter and LOD relationships from tagged properties.

#include <UnrealLib/Assets/AssetObject.hpp>

namespace UnrealLib::Assets
{
    /// @brief An object link and its resolution. The index belongs to the package of the containing asset.
    struct ParticleLink
    {
        ObjectIndex index;      ///< Serialized index, including null slots.
        Resolution resolution; ///< Resolved export or the reason the link could not be followed.
    };

    struct ParticleSystem
    {
        AssetSource source;
        std::vector<ParticleLink> emitters; ///< Emitters in serialized order.
    };

    struct ParticleEmitter
    {
        AssetSource source;
        std::vector<ParticleLink> lod_levels; ///< LODLevels in serialized order.
    };

    struct ParticleLodLevel
    {
        AssetSource source;
        ParticleLink required_module;      ///< RequiredModule.
        ParticleLink spawn_module;         ///< SpawnModule, if present in this UE3 branch.
        ParticleLink type_data_module;     ///< TypeDataModule.
        std::vector<ParticleLink> modules; ///< Modules in serialized order.
    };

    /// @brief Reads the emitter links. Provide a schema that describes the particle object arrays.
    [[nodiscard]] ParticleSystem read_particle_system(Linker& linker, ObjectRef object, const PropertySchema& schema);

    /// @brief Reads the LOD links of an emitter, including subclasses such as ParticleSpriteEmitter.
    [[nodiscard]] ParticleEmitter read_particle_emitter(Linker& linker, ObjectRef object, const PropertySchema& schema);

    /// @brief Reads the module links of a LOD. Use open_asset on a resolved module to read its tagged properties.
    [[nodiscard]] ParticleLodLevel read_particle_lod_level(Linker& linker, ObjectRef object, const PropertySchema& schema);
}
