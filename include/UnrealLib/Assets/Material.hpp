#pragma once

/// @file
/// @brief UE3 materials (Material, MaterialInstanceConstant, ...): metadata from script properties.
///
/// The native data of materials is the compiled shader cache (FMaterialResource / shader maps), which is
/// platform-specific and not decoded (upstream UELib does not decode it either); its size is reported. Everything
/// needed to understand a material offline — parent, parameters, expressions and the textures it samples — is in
/// the tagged properties of the material and of its expression subobjects.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <UnrealLib/Assets/AssetObject.hpp>

namespace UnrealLib::Assets
{
    /// @brief A parameter override of a material instance.
    struct MaterialParameter
    {
        /// @brief Parameter kind.
        enum class Kind : std::uint8_t
        {
            Scalar,  ///< ScalarParameterValues.
            Vector,  ///< VectorParameterValues (LinearColor).
            Texture, ///< TextureParameterValues.
        };
        Kind kind = Kind::Scalar;            ///< Kind.
        std::string name;                    ///< ParameterName.
        float scalar = 0;                    ///< Scalar value.
        std::array<float, 4> vector{};       ///< R, G, B, A.
        ObjectIndex texture;                 ///< Texture value (may be null).
    };

    /// @brief Material metadata.
    struct MaterialInfo
    {
        std::string name;                    ///< Object name.
        std::string class_name;              ///< Class.
        ObjectIndex parent;                  ///< Parent (material instances).
        std::vector<MaterialParameter> parameters; ///< Parameter overrides (material instances).
        std::vector<ObjectIndex> expressions; ///< Expression subobjects (materials).
        std::vector<ObjectIndex> textures;   ///< Textures referenced by the material, its parameters or expressions.
        std::string blend_mode;              ///< BlendMode enumerator (materials; default BLEND_Opaque).
        bool two_sided = false;              ///< TwoSided.
        std::size_t native_bytes = 0;        ///< Size of the undecoded native data (compiled shaders).
        std::size_t undecoded_parameters = 0; ///< Parameter arrays that stayed raw (no ReflectionSchema given).
        AssetProperties properties;          ///< Tagged properties.
    };

    /// @brief Whether @p object is a material: its class derives from Engine.MaterialInterface.
    [[nodiscard]] bool is_material(Linker& linker, ObjectRef object);

    /// @brief Reads material metadata. Pass a ReflectionSchema so the parameter arrays (arrays of script structs)
    ///        decode; without one they are counted in `undecoded_parameters`.
    [[nodiscard]] MaterialInfo read_material(Linker& linker, ObjectRef object, const PropertySchema* schema = nullptr);
}
