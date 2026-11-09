// Fuzz target "properties": property tags and values (read_properties with the built-in schema), plus invariants:
//   * write(read(x)) is a fixed point: reading and writing it again gives the same bytes;
//   * every decoded value survives format_property_value() -> parse_property_value() (serialized comparison).

#include <optional>
#include <string>

#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "../FuzzTargets.hpp"

namespace UnrealLib::Fuzz
{
    /// The name table the property inputs index into (seeds are written against it).
    const NameTable& property_names()
    {
        static const NameTable names = [] {
            NameTable t;
            for (const char* n : {"None", "IntProperty", "FloatProperty", "BoolProperty", "ByteProperty",
                                  "NameProperty", "StrProperty", "ObjectProperty", "ClassProperty",
                                  "ComponentProperty", "InterfaceProperty", "DelegateProperty", "ArrayProperty",
                                  "StructProperty", "MapProperty", "Vector", "Rotator", "Color", "LinearColor", "Guid",
                                  "Box", "Plane", "Quat", "Matrix", "Vector2D", "IntPoint", "A", "B", "C", "Value", "X",
                                  "Y", "Z", "ROLE_None", "ROLE_Authority"})
            {
                t.add(n);
            }
            return t;
        }();
        return names;
    }

    namespace
    {
        SerializationContext property_context()
        {
            SerializationContext c;
            c.file_version = 536;
            c.limits.max_allocation_bytes = 1u << 20;
            return c;
        }

        std::vector<std::byte> serialize(const std::vector<Property>& list)
        {
            BinaryWriter writer(property_context());
            write_properties(writer, list, FName{0, 0});
            return writer.take();
        }

        std::vector<std::byte> serialize(const PropertyValue& value)
        {
            BinaryWriter writer(property_context());
            write_property_value(writer, value, FName{0, 0});
            return writer.take();
        }

        std::vector<Property> read(std::span<const std::byte> bytes)
        {
            BinaryReader reader(bytes, property_context());
            PropertyReadOptions options;
            options.names = &property_names();
            options.owner = "Object";
            return read_properties(reader, options);
        }

        /// Struct fields that share a name and index cannot be told apart in text.
        bool has_ambiguous_fields(const PropertyValue& v)
        {
            if (const auto* s = std::get_if<StructValue>(&v.value))
            {
                for (std::size_t i = 0; i < s->fields.size(); ++i)
                {
                    if (has_ambiguous_fields(s->fields[i].value)) return true;
                    for (std::size_t j = i + 1; j < s->fields.size(); ++j)
                    {
                        if (s->fields[i].array_index == s->fields[j].array_index &&
                            Text::iequals(s->fields[i].name, s->fields[j].name))
                        {
                            return true;
                        }
                    }
                }
            }
            if (const auto* a = std::get_if<ArrayValue>(&v.value))
            {
                for (const PropertyValue& e : a->elements)
                {
                    if (has_ambiguous_fields(e)) return true;
                }
            }
            return false;
        }

        std::optional<FName> name_of_text(std::string_view text)
        {
            const NameTable& names = property_names();
            if (const auto i = names.find(text)) return FName{*i, 0};
            const std::size_t u = text.rfind('_');
            if (u != std::string_view::npos && u + 1 < text.size() && text[u + 1] != '0' && text.size() - u < 10 &&
                text.find_first_not_of("0123456789", u + 1) == std::string_view::npos)
            {
                if (const auto i = names.find(text.substr(0, u)))
                {
                    return FName{*i, std::stoi(std::string(text.substr(u + 1))) + 1};
                }
            }
            return std::nullopt;
        }
    }
}

UNREALLIB_FUZZ_TARGET(properties)
{
    using namespace UnrealLib;
    using namespace UnrealLib::Fuzz;
    const std::vector<Property> first = read(as_bytes(data, size));

    const std::vector<std::byte> once = serialize(first);
    const std::vector<std::byte> twice = serialize(read(once));
    fuzz_check(once == twice, "write(read(write(read(x)))) == write(read(x))");

    PropertyFormatOptions format;
    format.names = &property_names();
    PropertyParseOptions parse;
    parse.make_name = [](std::string_view text) {
        const std::optional<FName> n = name_of_text(text);
        if (!n) throw Error(ErrorCode::NotFound, "name not in the fuzz name table");
        return *n;
    };
    parse.resolve_object = [](std::string_view text) -> std::optional<ObjectIndex> {
        if (text.size() < 2 || text.front() != '#') return std::nullopt;
        return ObjectIndex{std::stoi(std::string(text.substr(1)))};
    };
    for (const Property& p : first)
    {
        if (has_ambiguous_fields(p.value)) continue;
        const std::string text = format_property_value(p.value, format);
        std::optional<PropertyValue> back;
        try
        {
            back = parse_property_value(text, p.value, parse);
        }
        catch (const Error&)
        {
            continue; // e.g. a name index outside the table ("<invalid>")
        }
        fuzz_check(serialize(*back) == serialize(p.value), "parse(format(v)) serializes like v");
    }
}
