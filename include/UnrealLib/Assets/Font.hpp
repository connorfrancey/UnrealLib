#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <UnrealLib/Assets/AssetObject.hpp>

namespace UnrealLib::Assets
{
    /// @brief A glyph rectangle on a font texture page.
    struct FontCharacter
    {
        std::int32_t start_u = 0;
        std::int32_t start_v = 0;
        std::int32_t u_size = 0;
        std::int32_t v_size = 0;
        std::uint8_t texture_index = 0;
        std::int32_t vertical_offset = 0;
    };

    /// @brief Kerning adjustment for a character pair in Mirror's Edge.
    struct FontKerningPair
    {
        std::uint16_t first = 0;
        std::uint16_t second = 0;
        float adjustment = 0.0f;
    };

    /// @brief UE3 Font data. From version 511, glyphs, textures, and kerning are tagged properties.
    struct Font
    {
        std::vector<FontCharacter> characters;
        std::vector<ObjectIndex> textures;
        std::int32_t kerning = 0;
        std::vector<std::pair<std::uint16_t, std::uint16_t>> char_remap;
        bool is_remapped = false;
        AssetProperties properties;
        std::vector<float> resolution_test_table; ///< Native MultiFont resolutions before version 511.
        std::vector<FontKerningPair> kerning_pairs; ///< Native pair adjustments in Mirror's Edge.
        std::vector<std::uint32_t> kerning_resolution_offsets; ///< Boundaries in kerning_pairs for each resolution.
    };

    /// @brief Reads a UE3 Font or MultiFont export and its native character remapping table.
    [[nodiscard]] Font read_font(Linker& linker, ObjectRef object, const PropertySchema* schema = nullptr);
}
