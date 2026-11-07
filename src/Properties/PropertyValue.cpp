#include <UnrealLib/Properties/PropertyValue.hpp>

#include <string>
#include <utility>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

namespace UnrealLib
{
    // ------------------------------------------------------------------------------------------------------------------
    // Builtin schema
    // ------------------------------------------------------------------------------------------------------------------

    namespace
    {
        TypeDesc scalar(PropertyType type)
        {
            TypeDesc desc;
            desc.type = type;
            return desc;
        }

        TypeDesc struct_type(std::string name)
        {
            TypeDesc desc;
            desc.type = PropertyType::Struct;
            desc.struct_name = std::move(name);
            return desc;
        }

        StructLayout binary_layout(std::string name, std::initializer_list<std::pair<const char*, TypeDesc>> fields)
        {
            StructLayout layout;
            layout.name = std::move(name);
            layout.binary = true;
            for (const auto& [field, type] : fields)
            {
                layout.fields.push_back(StructField{field, type, 1});
            }
            return layout;
        }
    }

    BuiltinPropertySchema::BuiltinPropertySchema()
    {
        const TypeDesc f = scalar(PropertyType::Float);
        const TypeDesc i = scalar(PropertyType::Int);
        const TypeDesc b = scalar(PropertyType::Byte);
        const TypeDesc vec = struct_type("Vector");
        const TypeDesc plane = struct_type("Plane");

        // Field order follows the UE3 Core Object.uc declarations (Plane/Sphere extend Vector: X,Y,Z then W).
        for (StructLayout layout : {
                 binary_layout("Vector", {{"X", f}, {"Y", f}, {"Z", f}}),
                 binary_layout("Vector2D", {{"X", f}, {"Y", f}}),
                 binary_layout("Vector4", {{"X", f}, {"Y", f}, {"Z", f}, {"W", f}}),
                 binary_layout("Plane", {{"X", f}, {"Y", f}, {"Z", f}, {"W", f}}),
                 binary_layout("Sphere", {{"X", f}, {"Y", f}, {"Z", f}, {"W", f}}),
                 binary_layout("Quat", {{"X", f}, {"Y", f}, {"Z", f}, {"W", f}}),
                 binary_layout("Rotator", {{"Pitch", i}, {"Yaw", i}, {"Roll", i}}),
                 binary_layout("Color", {{"B", b}, {"G", b}, {"R", b}, {"A", b}}),
                 binary_layout("LinearColor", {{"R", f}, {"G", f}, {"B", f}, {"A", f}}),
                 binary_layout("Guid", {{"A", i}, {"B", i}, {"C", i}, {"D", i}}),
                 binary_layout("IntPoint", {{"X", i}, {"Y", i}}),
                 binary_layout("Box", {{"Min", vec}, {"Max", vec}, {"IsValid", b}}),
                 binary_layout("TwoVectors", {{"v1", vec}, {"v2", vec}}),
                 binary_layout("Matrix", {{"XPlane", plane}, {"YPlane", plane}, {"ZPlane", plane}, {"WPlane", plane}}),
             })
        {
            std::string key = layout.name;
            m_structs.emplace(std::move(key), std::move(layout));
        }
    }

    const BuiltinPropertySchema& BuiltinPropertySchema::instance()
    {
        static const BuiltinPropertySchema schema;
        return schema;
    }

    const StructLayout* BuiltinPropertySchema::find_struct(std::string_view name) const
    {
        const auto it = m_structs.find(name);
        return it == m_structs.end() ? nullptr : &it->second;
    }

    std::optional<TypeDesc> BuiltinPropertySchema::find_property(std::string_view, std::string_view) const
    {
        return std::nullopt;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Reading
    // ------------------------------------------------------------------------------------------------------------------

    namespace
    {
        struct ReadState
        {
            const NameTable& names;
            const PropertySchema& schema;
            Diagnostics* diagnostics;
            std::size_t max_depth;
        };

        const StructLayout* find_layout(const ReadState& state, std::string_view name)
        {
            if (name.empty())
            {
                return nullptr;
            }
            if (const StructLayout* layout = state.schema.find_struct(name))
            {
                return layout;
            }
            return BuiltinPropertySchema::instance().find_struct(name);
        }

        void check_depth(const ReadState& state, std::size_t depth, const BinaryReader& reader)
        {
            if (depth > state.max_depth)
            {
                throw SerializationError("property values nested deeper than " + std::to_string(state.max_depth),
                                         reader.absolute_position());
            }
        }

        FName read_checked_fname(BinaryReader& reader, const NameTable& names)
        {
            const std::uint64_t offset = reader.absolute_position();
            const FName name = read_fname(reader);
            if (!names.contains_index(name.index) || name.number < 0)
            {
                throw SerializationError("invalid name reference " + std::to_string(name.index), offset);
            }
            return name;
        }

        std::vector<Property> read_tagged_list(BinaryReader& reader, const ReadState& state, std::string_view owner,
                                               std::size_t depth);
        PropertyValue read_element(BinaryReader& reader, const TypeDesc& type, const ReadState& state,
                                   std::string_view owner, std::size_t depth);

        StructValue read_binary_struct(BinaryReader& reader, const StructLayout& layout, const ReadState& state,
                                       std::size_t depth)
        {
            StructValue value;
            value.struct_name = layout.name;
            value.encoding = StructEncoding::Binary;
            for (const StructField& field : layout.fields)
            {
                for (std::int32_t i = 0; i < field.array_dim; ++i)
                {
                    Property property;
                    property.name = field.name;
                    property.array_index = i;
                    TypeDesc type = field.type;
                    type.is_enum = false; // binary structs store enum values as bytes
                    property.value = read_element(reader, type, state, layout.name, depth + 1);
                    value.fields.push_back(std::move(property));
                }
            }
            return value;
        }

        StructValue read_struct(BinaryReader& reader, std::string_view struct_name, const ReadState& state,
                                std::size_t depth)
        {
            check_depth(state, depth, reader);
            if (const StructLayout* layout = find_layout(state, struct_name); layout != nullptr && layout->binary)
            {
                return read_binary_struct(reader, *layout, state, depth);
            }
            StructValue value;
            value.struct_name = std::string(struct_name);
            value.encoding = StructEncoding::Tagged;
            value.fields = read_tagged_list(reader, state, struct_name, depth + 1);
            return value;
        }

        PropertyValue read_element(BinaryReader& reader, const TypeDesc& type, const ReadState& state,
                                   std::string_view owner, std::size_t depth)
        {
            check_depth(state, depth, reader);
            PropertyValue out;
            switch (type.type)
            {
            case PropertyType::Byte:
                // Enum elements of tagged data (array elements) are serialized by name; binary structs clear
                // is_enum for their fields because they store the raw byte.
                if (type.is_enum)
                {
                    out.value = EnumValue{read_checked_fname(reader, state.names)};
                }
                else
                {
                    out.value = reader.read_u8();
                }
                break;
            case PropertyType::Int: out.value = reader.read_i32(); break;
            case PropertyType::Bool: out.value = reader.read_u8() != 0; break;
            case PropertyType::Float: out.value = reader.read_f32(); break;
            case PropertyType::Object:
            case PropertyType::Class:
            case PropertyType::Component:
            case PropertyType::Interface: out.value = ObjectValue{ObjectIndex{reader.read_index()}}; break;
            case PropertyType::Name: out.value = NameValue{read_checked_fname(reader, state.names)}; break;
            case PropertyType::Str: out.value = reader.read_fstring(); break;
            case PropertyType::Delegate:
            {
                DelegateValue delegate;
                delegate.object = ObjectIndex{reader.read_index()};
                delegate.function = read_checked_fname(reader, state.names);
                out.value = delegate;
                break;
            }
            case PropertyType::Struct: out.value = read_struct(reader, type.struct_name, state, depth); break;
            case PropertyType::Array:
            {
                if (!type.inner)
                {
                    throw SerializationError("array element type unknown", reader.absolute_position());
                }
                ArrayValue array;
                array.count = static_cast<std::int32_t>(reader.read_array_count(1));
                array.elements.reserve(static_cast<std::size_t>(array.count));
                for (std::int32_t i = 0; i < array.count; ++i)
                {
                    array.elements.push_back(read_element(reader, *type.inner, state, owner, depth + 1));
                }
                out.value = std::move(array);
                break;
            }
            default:
                throw SerializationError(fmt::format("cannot decode an element of type {}", to_string(type.type)),
                                         reader.absolute_position());
            }
            return out;
        }

        // Tries to decode the elements of an array whose element type is unknown as tagged structs (the only
        // element kind that is self-describing). Succeeds only if every element is a well-formed tag list and the
        // data is consumed exactly.
        std::optional<std::vector<PropertyValue>> try_tagged_struct_elements(BinaryReader reader, std::int32_t count,
                                                                             const ReadState& state, std::size_t depth)
        {
            std::vector<PropertyValue> elements;
            bool ok = true;
            try
            {
                Diagnostics scratch;
                ReadState quiet{state.names, state.schema, &scratch, state.max_depth};
                for (std::int32_t i = 0; i < count && ok; ++i)
                {
                    StructValue value;
                    value.encoding = StructEncoding::Tagged;
                    value.fields = read_tagged_list(reader, quiet, {}, depth + 1);
                    PropertyValue element;
                    element.value = std::move(value);
                    elements.push_back(std::move(element));
                }
                ok = ok && reader.at_end() && scratch.empty();
            }
            catch (const Error&)
            {
                ok = false;
            }
            if (!ok)
            {
                return std::nullopt;
            }
            return elements;
        }

        PropertyValue read_tagged_value(BinaryReader& reader, const PropertyTag& tag, std::string_view name,
                                        const ReadState& state, std::string_view owner, std::size_t depth)
        {
            PropertyValue out;
            switch (tag.type)
            {
            case PropertyType::Bool: out.value = tag.bool_value.value_or(false); break;
            case PropertyType::Byte:
                if (tag.size == 1)
                {
                    out.value = reader.read_u8();
                }
                else
                {
                    out.value = EnumValue{read_checked_fname(reader, state.names)};
                }
                break;
            case PropertyType::Struct:
            {
                const std::string struct_name =
                    tag.struct_name ? state.names.to_string(*tag.struct_name) : std::string();
                out.value = read_struct(reader, struct_name, state, depth + 1);
                break;
            }
            case PropertyType::Array:
            {
                const std::optional<TypeDesc> property = state.schema.find_property(owner, name);
                ArrayValue array;
                array.count = static_cast<std::int32_t>(reader.read_array_count(1));
                if (property && property->type == PropertyType::Array && property->inner)
                {
                    array.elements.reserve(static_cast<std::size_t>(array.count));
                    for (std::int32_t i = 0; i < array.count; ++i)
                    {
                        array.elements.push_back(read_element(reader, *property->inner, state, owner, depth + 1));
                    }
                }
                else if (auto structs = array.count > 0 ? try_tagged_struct_elements(reader, array.count, state, depth)
                                                        : std::optional<std::vector<PropertyValue>>{};
                         structs)
                {
                    array.elements = std::move(*structs);
                    reader.skip(reader.remaining());
                }
                else if (array.count == 0)
                {
                    // Empty: nothing to decode.
                }
                else
                {
                    const auto bytes = reader.read_view(reader.remaining());
                    array.raw = std::vector<std::byte>(bytes.begin(), bytes.end());
                }
                out.value = std::move(array);
                break;
            }
            case PropertyType::Map:
            case PropertyType::FixedArray:
            case PropertyType::Pointer:
            case PropertyType::String:
            case PropertyType::Unknown:
            {
                const auto bytes = reader.read_view(reader.remaining());
                out.value = RawValue{std::vector<std::byte>(bytes.begin(), bytes.end())};
                break;
            }
            default:
            {
                TypeDesc type;
                type.type = tag.type;
                out = read_element(reader, type, state, owner, depth);
                break;
            }
            }
            return out;
        }

        std::vector<Property> read_tagged_list(BinaryReader& reader, const ReadState& state, std::string_view owner,
                                               std::size_t depth)
        {
            check_depth(state, depth, reader);
            std::vector<Property> properties;
            while (true)
            {
                std::optional<PropertyTag> tag = read_property_tag(reader, state.names);
                if (!tag)
                {
                    return properties;
                }

                Property property;
                property.name = state.names.to_string(tag->name);
                property.array_index = tag->array_index;

                const std::size_t value_start = reader.position();
                const auto size = static_cast<std::size_t>(tag->size);
                BinaryReader value_reader = reader.sub_reader(value_start, size);
                reader.skip(size);

                bool decoded = false;
                std::string failure;
                try
                {
                    property.value = read_tagged_value(value_reader, *tag, property.name, state, owner, depth);
                    decoded = value_reader.at_end();
                    if (!decoded)
                    {
                        failure = fmt::format("value used {} of {} bytes", value_reader.position(), size);
                    }
                }
                catch (const Error& e)
                {
                    failure = e.what();
                }

                if (!decoded)
                {
                    // Struct with a binary layout that did not fit: retry as a tagged struct, then give up.
                    bool recovered = false;
                    if (tag->type == PropertyType::Struct)
                    {
                        try
                        {
                            BinaryReader retry = reader.sub_reader(value_start, size);
                            StructValue value;
                            value.struct_name = tag->struct_name ? state.names.to_string(*tag->struct_name) : "";
                            value.fields = read_tagged_list(retry, state, value.struct_name, depth + 1);
                            if (retry.at_end())
                            {
                                property.value.value = std::move(value);
                                recovered = true;
                            }
                        }
                        catch (const Error&)
                        {
                            recovered = false;
                        }
                    }
                    if (!recovered)
                    {
                        const auto bytes = reader.data().subspan(value_start, size);
                        property.value.value = RawValue{std::vector<std::byte>(bytes.begin(), bytes.end())};
                        if (state.diagnostics != nullptr)
                        {
                            state.diagnostics->warning(
                                "property.raw-value",
                                fmt::format("{}.{} ({}): kept as raw bytes: {}", owner, property.name,
                                            to_string(tag->type), failure),
                                reader.base_offset() + value_start);
                        }
                    }
                }

                property.tag = std::move(tag);
                properties.push_back(std::move(property));
            }
        }
    }

    std::vector<Property> read_properties(BinaryReader& reader, const PropertyReadOptions& options)
    {
        if (options.names == nullptr)
        {
            throw Error(ErrorCode::InvalidArgument, "read_properties: options.names is required");
        }
        const ReadState state{*options.names,
                              options.schema != nullptr ? *options.schema : BuiltinPropertySchema::instance(),
                              options.diagnostics, options.max_depth};
        return read_tagged_list(reader, state, options.owner, 0);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Writing
    // ------------------------------------------------------------------------------------------------------------------

    void write_properties(BinaryWriter& writer, const std::vector<Property>& properties, const FName& none)
    {
        for (const Property& property : properties)
        {
            if (!property.tag)
            {
                throw Error(ErrorCode::InvalidArgument,
                            "write_properties: property '" + property.name + "' has no tag");
            }
            PropertyTag tag = *property.tag;
            BinaryWriter value_writer(writer.context());
            if (tag.type == PropertyType::Bool)
            {
                if (const bool* value = std::get_if<bool>(&property.value.value))
                {
                    tag.bool_value = *value;
                }
            }
            else
            {
                write_property_value(value_writer, property.value, none);
            }
            const std::vector<std::byte> bytes = value_writer.take();
            tag.size = static_cast<std::int32_t>(bytes.size());
            write_property_tag(writer, tag);
            writer.write_bytes(bytes);
        }
        write_property_terminator(writer, none);
    }

    void write_property_value(BinaryWriter& writer, const PropertyValue& value, const FName& none)
    {
        struct Visitor
        {
            BinaryWriter& writer;
            const FName& none;

            void operator()(std::monostate) const {}
            void operator()(bool v) const { writer.write_u8(v ? 1 : 0); }
            void operator()(std::uint8_t v) const { writer.write_u8(v); }
            void operator()(const EnumValue& v) const { write_fname(writer, v.value); }
            void operator()(std::int32_t v) const { writer.write_i32(v); }
            void operator()(float v) const { writer.write_f32(v); }
            void operator()(const NameValue& v) const { write_fname(writer, v.value); }
            void operator()(const FString& v) const { writer.write_fstring(v); }
            void operator()(const ObjectValue& v) const { writer.write_index(v.value.value); }
            void operator()(const DelegateValue& v) const
            {
                writer.write_index(v.object.value);
                write_fname(writer, v.function);
            }
            void operator()(const StructValue& v) const
            {
                if (v.encoding == StructEncoding::Tagged)
                {
                    write_properties(writer, v.fields, none);
                    return;
                }
                for (const Property& field : v.fields)
                {
                    write_property_value(writer, field.value, none);
                }
            }
            void operator()(const ArrayValue& v) const
            {
                writer.write_i32(v.count);
                if (v.raw)
                {
                    writer.write_bytes(*v.raw);
                    return;
                }
                for (const PropertyValue& element : v.elements)
                {
                    write_property_value(writer, element, none);
                }
            }
            void operator()(const RawValue& v) const { writer.write_bytes(v.bytes); }
        };
        std::visit(Visitor{writer, none}, value.value);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Formatting / lookup
    // ------------------------------------------------------------------------------------------------------------------

    namespace
    {
        std::string format_float(float value)
        {
            std::string text = fmt::format("{}", value);
            if (text.find_first_of(".eEn") == std::string::npos)
            {
                text += ".0";
            }
            return text;
        }

        std::string quote(std::string_view text)
        {
            std::string out = "\"";
            for (const char c : text)
            {
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                }
                out += c;
            }
            out += '"';
            return out;
        }
    }

    std::string format_property_value(const PropertyValue& value, const PropertyFormatOptions& options)
    {
        if (options.names == nullptr)
        {
            throw Error(ErrorCode::InvalidArgument, "format_property_value: options.names is required");
        }
        const NameTable& names = *options.names;
        auto name_of = [&](const FName& name) { return names.try_to_string(name).value_or("<invalid>"); };
        auto object_of = [&](ObjectIndex index) -> std::string {
            if (index.is_null())
            {
                return "None";
            }
            return options.object_name ? options.object_name(index) : fmt::format("#{}", index.value);
        };

        struct Visitor
        {
            const PropertyFormatOptions& options;
            decltype(name_of)& name_of;
            decltype(object_of)& object_of;

            std::string operator()(std::monostate) const { return {}; }
            std::string operator()(bool v) const { return v ? "True" : "False"; }
            std::string operator()(std::uint8_t v) const { return std::to_string(v); }
            std::string operator()(const EnumValue& v) const { return name_of(v.value); }
            std::string operator()(std::int32_t v) const { return std::to_string(v); }
            std::string operator()(float v) const { return format_float(v); }
            std::string operator()(const NameValue& v) const { return quote(name_of(v.value)); }
            std::string operator()(const FString& v) const { return quote(v.text); }
            std::string operator()(const ObjectValue& v) const { return object_of(v.value); }
            std::string operator()(const DelegateValue& v) const
            {
                return v.object.is_null() ? name_of(v.function)
                                          : object_of(v.object) + "." + name_of(v.function);
            }
            std::string operator()(const StructValue& v) const
            {
                std::string out = "(";
                bool first = true;
                for (const Property& field : v.fields)
                {
                    if (!first)
                    {
                        out += ',';
                    }
                    first = false;
                    out += field.name;
                    if (field.array_index != 0 || (field.tag && field.tag->array_index != 0))
                    {
                        out += fmt::format("[{}]", field.array_index);
                    }
                    out += '=';
                    out += std::visit(*this, field.value.value);
                }
                return out + ")";
            }
            std::string operator()(const ArrayValue& v) const
            {
                if (v.raw)
                {
                    return fmt::format("<{} elements, {} raw bytes>", v.count, v.raw->size());
                }
                std::string out = "(";
                for (std::size_t i = 0; i < v.elements.size(); ++i)
                {
                    if (i != 0)
                    {
                        out += ',';
                    }
                    out += std::visit(*this, v.elements[i].value);
                }
                return out + ")";
            }
            std::string operator()(const RawValue& v) const { return fmt::format("<{} raw bytes>", v.bytes.size()); }
        };
        return std::visit(Visitor{options, name_of, object_of}, value.value);
    }

    const Property* find_property(const std::vector<Property>& properties, std::string_view name,
                                  std::int32_t array_index)
    {
        for (const Property& property : properties)
        {
            if (property.array_index == array_index && Text::iequals(property.name, name))
            {
                return &property;
            }
        }
        return nullptr;
    }
}
