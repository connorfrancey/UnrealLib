#include <UnrealLib/Properties/PropertyTag.hpp>

#include <array>
#include <string>
#include <utility>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    namespace
    {
        constexpr std::array<std::pair<PropertyType, std::string_view>, 17> type_names{{
            {PropertyType::Byte, "ByteProperty"},
            {PropertyType::Int, "IntProperty"},
            {PropertyType::Bool, "BoolProperty"},
            {PropertyType::Float, "FloatProperty"},
            {PropertyType::Object, "ObjectProperty"},
            {PropertyType::Name, "NameProperty"},
            {PropertyType::Delegate, "DelegateProperty"},
            {PropertyType::Class, "ClassProperty"},
            {PropertyType::Array, "ArrayProperty"},
            {PropertyType::Struct, "StructProperty"},
            {PropertyType::Str, "StrProperty"},
            {PropertyType::Map, "MapProperty"},
            {PropertyType::Interface, "InterfaceProperty"},
            {PropertyType::Component, "ComponentProperty"},
            {PropertyType::String, "StringProperty"},
            {PropertyType::FixedArray, "FixedArrayProperty"},
            {PropertyType::Pointer, "PointerProperty"},
        }};

        const std::string& name_text(const NameTable& names, const FName& name, std::uint64_t offset)
        {
            if (!names.contains_index(name.index))
            {
                throw SerializationError("property tag: name index " + std::to_string(name.index) + " out of range",
                                         offset);
            }
            return names.at(name.index).text.text;
        }
    }

    std::string_view to_string(PropertyType type) noexcept
    {
        for (const auto& [value, text] : type_names)
        {
            if (value == type)
            {
                return text;
            }
        }
        return "Unknown";
    }

    PropertyType parse_property_type(std::string_view name) noexcept
    {
        for (const auto& [value, text] : type_names)
        {
            if (Text::iequals(text, name))
            {
                return value;
            }
        }
        return PropertyType::Unknown;
    }

    std::optional<PropertyTag> read_property_tag(BinaryReader& reader, const NameTable& names)
    {
        const SerializationContext& ctx = reader.context();
        const std::uint64_t start = reader.absolute_position();
        if (ctx.file_version < PropertyTagVersion::RefactoredPropertyTags)
        {
            throw UnsupportedError("property tags of file version " + std::to_string(ctx.file_version) +
                                       " (< 223) are not supported",
                                   start);
        }

        PropertyTag tag;
        tag.name = read_fname(reader);
        if (Text::iequals(name_text(names, tag.name, start), "None"))
        {
            return std::nullopt;
        }

        const std::uint64_t type_offset = reader.absolute_position();
        tag.type_name = read_fname(reader);
        tag.type = parse_property_type(name_text(names, tag.type_name, type_offset));
        if (tag.type == PropertyType::Unknown)
        {
            throw SerializationError("property tag: '" + names.to_string(tag.type_name) + "' is not a property type",
                                     type_offset);
        }

        const std::uint64_t size_offset = reader.absolute_position();
        tag.size = reader.read_i32();
        tag.array_index = reader.read_i32();
        if (tag.size < 0)
        {
            throw SerializationError("property tag: negative value size " + std::to_string(tag.size), size_offset);
        }

        switch (tag.type)
        {
        case PropertyType::Struct:
        {
            const std::uint64_t offset = reader.absolute_position();
            tag.struct_name = read_fname(reader);
            (void)name_text(names, *tag.struct_name, offset);
            break;
        }
        case PropertyType::Byte:
            if (ctx.file_version >= PropertyTagVersion::EnumNameAddedToBytePropertyTag)
            {
                const std::uint64_t offset = reader.absolute_position();
                tag.enum_name = read_fname(reader);
                (void)name_text(names, *tag.enum_name, offset);
            }
            break;
        case PropertyType::Bool:
            tag.bool_value = ctx.file_version >= PropertyTagVersion::BoolValueToByteForBoolPropertyTag
                                 ? reader.read_u8() != 0
                                 : reader.read_i32() != 0;
            break;
        default:
            break;
        }

        if (static_cast<std::size_t>(tag.size) > reader.remaining())
        {
            throw SerializationError("property tag '" + names.to_string(tag.name) + "': value size " +
                                         std::to_string(tag.size) + " exceeds the remaining " +
                                         std::to_string(reader.remaining()) + " bytes",
                                     size_offset);
        }
        return tag;
    }

    void write_property_tag(BinaryWriter& writer, const PropertyTag& tag)
    {
        const SerializationContext& ctx = writer.context();
        write_fname(writer, tag.name);
        write_fname(writer, tag.type_name);
        writer.write_i32(tag.size);
        writer.write_i32(tag.array_index);
        switch (tag.type)
        {
        case PropertyType::Struct:
            write_fname(writer, tag.struct_name.value_or(FName{}));
            break;
        case PropertyType::Byte:
            if (ctx.file_version >= PropertyTagVersion::EnumNameAddedToBytePropertyTag)
            {
                write_fname(writer, tag.enum_name.value_or(FName{}));
            }
            break;
        case PropertyType::Bool:
            if (ctx.file_version >= PropertyTagVersion::BoolValueToByteForBoolPropertyTag)
            {
                writer.write_u8(tag.bool_value.value_or(false) ? 1 : 0);
            }
            else
            {
                writer.write_i32(tag.bool_value.value_or(false) ? 1 : 0);
            }
            break;
        default:
            break;
        }
    }

    void write_property_terminator(BinaryWriter& writer, const FName& none)
    {
        write_fname(writer, none);
    }
}
