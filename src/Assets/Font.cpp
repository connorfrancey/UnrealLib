#include <UnrealLib/Assets/Font.hpp>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib::Assets
{
    namespace
    {
        void read_native_glyphs(BinaryReader& reader, Font& font)
        {
            const bool has_vertical_offset = reader.context().file_version >= 506;
            const auto characters = reader.read_array_count(has_vertical_offset ? 21 : 17);
            font.characters.reserve(characters);
            for (std::uint32_t i = 0; i < characters; ++i)
            {
                FontCharacter character;
                character.start_u = reader.read_i32();
                character.start_v = reader.read_i32();
                character.u_size = reader.read_i32();
                character.v_size = reader.read_i32();
                character.texture_index = reader.read_u8();
                if (has_vertical_offset)
                {
                    character.vertical_offset = reader.read_i32();
                }
                font.characters.push_back(character);
            }

            const auto textures = reader.read_array_count(4);
            font.textures.reserve(textures);
            for (std::uint32_t i = 0; i < textures; ++i)
            {
                font.textures.push_back(ObjectIndex{reader.read_index()});
            }
            font.kerning = reader.read_i32();
        }
    }

    Font read_font(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        auto source = open_asset(linker, object, schema);
        const bool multi_font = Text::iequals(source.class_name, "MultiFont");
        if (!multi_font && !Text::iequals(source.class_name, "Font"))
        {
            throw Error(ErrorCode::InvalidArgument, "read_font requires a Font or MultiFont export");
        }
        auto reader = source.native_reader();
        const auto version = reader.context().file_version;
        if (version < 249)
        {
            throw UnsupportedError("read_font requires a UE3 package");
        }

        Font font;
        const bool has_native_glyphs = version < 511;
        if (has_native_glyphs)
        {
            read_native_glyphs(reader, font);
        }

        const auto remap_count = reader.read_array_count(4);
        font.char_remap.reserve(remap_count);
        for (std::uint32_t i = 0; i < remap_count; ++i)
        {
            const auto character = reader.read_u16();
            font.char_remap.emplace_back(character, reader.read_u16());
        }

        if (has_native_glyphs)
        {
            font.is_remapped = reader.read_bool32();
            if (multi_font)
            {
                font.resolution_test_table.resize(reader.read_array_count(4));
                for (float& resolution : font.resolution_test_table)
                {
                    resolution = reader.read_f32();
                }
            }
        }
        else
        {
            font.kerning = source.properties.get_int("Kerning").value_or(0);
            font.is_remapped = source.properties.get_bool("IsRemapped").value_or(
                source.properties.get_int("IsRemapped").value_or(!font.char_remap.empty()) != 0);
        }
        if (object.package->profile().assets.font_kerning_tables)
        {
            font.kerning_pairs.resize(reader.read_array_count(8));
            for (FontKerningPair& pair : font.kerning_pairs)
            {
                pair.first = reader.read_u16();
                pair.second = reader.read_u16();
                pair.adjustment = reader.read_f32();
            }
            font.kerning_resolution_offsets.resize(reader.read_array_count(4));
            for (std::uint32_t& offset : font.kerning_resolution_offsets)
            {
                offset = reader.read_u32();
            }
        }
        expect_end(reader, source.class_name);
        font.properties = std::move(source.properties);
        return font;
    }
}
