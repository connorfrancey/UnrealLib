#include <UnrealLib/Reflection/Fields.hpp>

#include <fmt/format.h>

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    namespace
    {
        // Version gates (upstream PackageObjectLegacyVersion).
        constexpr std::uint16_t vRelease64 = 64;
        constexpr std::uint16_t vAddedCppTextToUStruct = 120;
        constexpr std::uint16_t vMovedFriendlyNameToUFunction = 160;
        constexpr std::uint16_t vAddedFuncMapToUState = 160;
        constexpr std::uint16_t vUE3 = 178;
        constexpr std::uint16_t vAddedAutoExpandCategoriesToUClass = 185;
        constexpr std::uint16_t vAddedDelegateSourceToUDelegateProperty = 185;
        constexpr std::uint16_t vPropertyFlagsSizeExpandedTo64Bits = 190;
        constexpr std::uint16_t vDisplacedScriptPropertiesWithClassDefaultObject = 219;
        constexpr std::uint16_t vAddedStructFlagsToScriptStruct = 242;
        constexpr std::uint16_t vComponentClassBridgeMapDeprecated = 248;
        constexpr std::uint16_t vClassGuidDeprecated = 276;
        constexpr std::uint16_t vInterfaceClassesDeprecated = 288;
        constexpr std::uint16_t vChangedDelegateSourceFromNameToObject = 375;
        constexpr std::uint16_t vAddedArrayEnumToUProperty = 401;
        constexpr std::uint16_t vHideCategoriesOldOrder = 539;
        constexpr std::uint16_t vClassPlatformFlagsDeprecated = 547;
        constexpr std::uint16_t vAddedDontSortCategoriesToUClass = 603;
        constexpr std::uint16_t vAddedDataScriptSizeToUStruct = 639;
        constexpr std::uint16_t vAddedDLLBindFeature = 655;
        constexpr std::uint16_t vProbeMaskReducedAndIgnoreMaskRemoved = 691;
        constexpr std::uint16_t vForceScriptOrderAddedToUClass = 749;
        constexpr std::uint16_t vSuperReferenceMovedToUStruct = 756;
        constexpr std::uint16_t vAddedClassGroupsToUClass = 789;
        constexpr std::uint16_t vAddedNativeClassNameToUClass = 813;
        constexpr std::uint16_t vAddedAutoCollapseCategoriesToUClass = 671; // upstream: Version > 670

        constexpr std::pair<FieldKind, std::string_view> kind_names[] = {
            {FieldKind::Class, "Class"},         {FieldKind::State, "State"},
            {FieldKind::Function, "Function"},   {FieldKind::ScriptStruct, "ScriptStruct"},
            {FieldKind::Struct, "Struct"},       {FieldKind::Enum, "Enum"},
            {FieldKind::Const, "Const"},         {FieldKind::Property, "Property"},
            {FieldKind::TextBuffer, "TextBuffer"},
            {FieldKind::MetaData, "MetaData"},
        };

        std::vector<FName> read_name_list(BinaryReader& reader)
        {
            const std::uint32_t count = reader.read_array_count(8);
            std::vector<FName> names;
            names.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                names.push_back(read_fname(reader));
            }
            return names;
        }

        std::vector<std::pair<FName, ObjectIndex>> read_name_object_map(BinaryReader& reader)
        {
            const std::uint32_t count = reader.read_array_count(12);
            std::vector<std::pair<FName, ObjectIndex>> map;
            map.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const FName key = read_fname(reader);
                map.emplace_back(key, ObjectIndex{reader.read_index()});
            }
            return map;
        }

        std::vector<FieldMetadata> read_metadata(BinaryReader& reader)
        {
            const bool has_field_path = reader.context().file_version <= 540;
            const auto field_count = reader.read_array_count(8);
            std::vector<FieldMetadata> fields;
            fields.reserve(field_count);
            for (std::uint32_t i = 0; i < field_count; ++i)
            {
                FieldMetadata entry;
                if (has_field_path)
                {
                    entry.field = reader.read_fstring();
                }
                else
                {
                    entry.field = ObjectIndex{reader.read_index()};
                }
                const auto tag_count = reader.read_array_count(12);
                entry.tags.reserve(tag_count);
                for (std::uint32_t j = 0; j < tag_count; ++j)
                {
                    const auto name = read_fname(reader);
                    entry.tags.emplace_back(name, reader.read_fstring());
                }
                fields.push_back(std::move(entry));
            }
            return fields;
        }

        void read_struct(BinaryReader& reader, FieldObject& out, const FieldReadOptions& options)
        {
            const SerializationContext& ctx = reader.context();
            StructData data;
            if (ctx.file_version >= vSuperReferenceMovedToUStruct)
            {
                out.field.super = ObjectIndex{reader.read_index()};
            }
            if (!options.console_cooked)
            {
                data.script_text = ObjectIndex{reader.read_index()};
            }
            data.children = ObjectIndex{reader.read_index()};
            if (ctx.file_version < vMovedFriendlyNameToUFunction)
            {
                (void)read_fname(reader); // FriendlyName (moved to UFunction)
            }
            if (ctx.file_version >= vAddedCppTextToUStruct && !options.console_cooked)
            {
                data.cpp_text = ObjectIndex{reader.read_index()};
            }
            if (!options.console_cooked)
            {
                data.line = reader.read_i32();
                data.text_pos = reader.read_i32();
            }
            data.script_size = reader.read_i32();
            if (data.script_size < 0)
            {
                throw SerializationError("negative ScriptSize", reader.absolute_position());
            }
            std::int32_t storage = data.script_size;
            if (ctx.file_version >= vAddedDataScriptSizeToUStruct)
            {
                storage = reader.read_i32();
            }
            data.script_offset = reader.position();

            const bool same_size = ctx.file_version >= vAddedDataScriptSizeToUStruct ||
                                   (ctx.file_version >= 343 && ctx.file_version < 587);
            if (same_size)
            {
                if (storage < 0 || static_cast<std::size_t>(storage) > reader.remaining())
                {
                    throw SerializationError(fmt::format("bytecode size {} exceeds the remaining {} bytes", storage,
                                                         reader.remaining()),
                                             reader.absolute_position());
                }
                reader.skip(static_cast<std::size_t>(storage));
                data.script_storage_size = static_cast<std::size_t>(storage);
            }
            else
            {
                const TokenMap map = TokenMap::ue3(ctx.file_version);
                BytecodeOptions bytecode;
                bytecode.token_map = &map;
                bytecode.name_count = static_cast<std::int32_t>(options.names->size());
                bytecode.none_index = options.names->find("None").value_or(-1);
                const Script script = decode_script(reader, static_cast<std::size_t>(data.script_size), bytecode);
                data.script_storage_size = script.storage_size;
            }
            out.structure = data;
        }

        void read_state(BinaryReader& reader, FieldObject& out)
        {
            const SerializationContext& ctx = reader.context();
            StateData data;
            if (ctx.file_version < vProbeMaskReducedAndIgnoreMaskRemoved)
            {
                data.probe_mask = reader.read_u64();
                data.ignore_mask = reader.read_u64();
            }
            else
            {
                data.probe_mask = reader.read_u32();
            }
            data.label_table_offset = reader.read_u16();
            data.flags = reader.read_u32();
            if (ctx.file_version >= vAddedFuncMapToUState)
            {
                data.function_map = read_name_object_map(reader);
            }
            out.state = std::move(data);
        }

        void read_class(BinaryReader& reader, FieldObject& out, const FieldReadOptions& options)
        {
            const std::uint16_t v = reader.context().file_version;
            if (v < vUE3)
            {
                throw UnsupportedError("UClass data before version 178 is not supported", reader.absolute_position());
            }
            ClassData data;
            data.flags = reader.read_u32();
            if (v >= vClassGuidDeprecated)
            {
                if (v < vClassPlatformFlagsDeprecated)
                {
                    data.platform_flags = reader.read_u8();
                }
            }
            else
            {
                (void)reader.read_guid();
            }
            data.within = ObjectIndex{reader.read_index()};
            data.config_name = read_fname(reader);

            const bool old_order = v <= vHideCategoriesOldOrder;
            if (old_order && !options.console_cooked)
            {
                data.hide_categories = read_name_list(reader);
            }
            if (v < vComponentClassBridgeMapDeprecated)
            {
                throw UnsupportedError("UClass ComponentClassBridgeMap (version < 248) is not supported",
                                       reader.absolute_position());
            }
            data.component_defaults = read_name_object_map(reader);
            if (v < vInterfaceClassesDeprecated)
            {
                throw UnsupportedError("UClass interface classes (version < 288) are not supported",
                                       reader.absolute_position());
            }
            {
                const std::uint32_t count = reader.read_array_count(8);
                data.interfaces.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const ObjectIndex cls{reader.read_i32()};
                    data.interfaces.emplace_back(cls, ObjectIndex{reader.read_i32()});
                }
            }
            // Editor metadata (absent from console-cooked packages), in upstream UClass.Deserialize order.
            if (!options.console_cooked)
            {
                if (v >= vAddedDontSortCategoriesToUClass)
                {
                    data.dont_sort_categories = read_name_list(reader);
                }
                if (!old_order)
                {
                    data.hide_categories = read_name_list(reader);
                }
                if (v >= vAddedAutoExpandCategoriesToUClass &&
                    ((data.flags & ClassFlag::CollapseCategories) == 0 || v <= vHideCategoriesOldOrder || v >= 576))
                {
                    data.auto_expand_categories = read_name_list(reader);
                }
                if (v >= vAddedAutoCollapseCategoriesToUClass)
                {
                    data.auto_collapse_categories = read_name_list(reader);
                }
                if (v >= vForceScriptOrderAddedToUClass)
                {
                    data.force_script_order = reader.read_bool32();
                }
                if (options.dishonored_class_layout)
                {
                    (void)read_fname(reader); // unknown name (upstream `unknownName`)
                    data.native_class_name = reader.read_string();
                }
                else
                {
                    if (v >= vAddedClassGroupsToUClass)
                    {
                        data.class_groups = read_name_list(reader);
                    }
                    if (v >= vAddedNativeClassNameToUClass)
                    {
                        data.native_class_name = reader.read_string();
                    }
                }
                if (v >= vAddedDLLBindFeature)
                {
                    data.dll_bind_name = read_fname(reader);
                }
                if (options.dishonored_class_layout)
                {
                    data.class_groups = read_name_list(reader);
                }
            }
            if (v >= vDisplacedScriptPropertiesWithClassDefaultObject)
            {
                data.default_object = ObjectIndex{reader.read_index()};
            }
            out.class_data = std::move(data);
        }

        void read_property(BinaryReader& reader, FieldObject& out, PropertyType type, const FieldReadOptions& options)
        {
            const std::uint16_t v = reader.context().file_version;
            PropertyData data;
            data.type = type;
            data.array_dim = reader.read_i32();
            data.flags = v >= vPropertyFlagsSizeExpandedTo64Bits ? reader.read_u64() : reader.read_u32();
            if (!options.console_cooked)
            {
                data.category = read_fname(reader);
                if (v >= vAddedArrayEnumToUProperty)
                {
                    data.array_enum = ObjectIndex{reader.read_index()};
                }
            }
            if ((data.flags & PropertyFlag::Net) != 0)
            {
                data.rep_offset = reader.read_u16();
            }
            switch (type)
            {
            case PropertyType::Byte:
            case PropertyType::Object:
            case PropertyType::Component:
            case PropertyType::Interface:
            case PropertyType::Struct:
            case PropertyType::Array: data.reference = ObjectIndex{reader.read_index()}; break;
            case PropertyType::Class:
                data.reference = ObjectIndex{reader.read_index()};
                data.reference2 = ObjectIndex{reader.read_index()};
                break;
            case PropertyType::Map:
                data.reference = ObjectIndex{reader.read_index()};
                data.reference2 = ObjectIndex{reader.read_index()};
                break;
            case PropertyType::FixedArray:
                data.reference = ObjectIndex{reader.read_index()};
                data.fixed_count = reader.read_index();
                break;
            case PropertyType::Delegate:
                data.reference = ObjectIndex{reader.read_index()};
                if (v >= vAddedDelegateSourceToUDelegateProperty)
                {
                    if (v < vChangedDelegateSourceFromNameToObject)
                    {
                        (void)read_fname(reader);
                    }
                    else
                    {
                        data.reference2 = ObjectIndex{reader.read_index()};
                    }
                }
                break;
            default: break;
            }
            out.property = data;
        }
    }

    std::string_view to_string(FieldKind kind) noexcept
    {
        for (const auto& [value, text] : kind_names)
        {
            if (value == kind)
            {
                return text;
            }
        }
        return "Unknown";
    }

    std::optional<FieldKind> field_kind_of(std::string_view class_name, PropertyType* property_type) noexcept
    {
        for (const auto& [value, text] : kind_names)
        {
            if (value != FieldKind::Property && Text::iequals(class_name, text))
            {
                return value;
            }
        }
        const PropertyType type = parse_property_type(class_name);
        if (type != PropertyType::Unknown)
        {
            if (property_type != nullptr)
            {
                *property_type = type;
            }
            return FieldKind::Property;
        }
        // UE3 class name of booleans etc. are all covered above; "Property" itself is abstract.
        return std::nullopt;
    }

    FieldObject read_field(BinaryReader& reader, FieldKind kind, PropertyType property_type,
                           const ObjectHeaderInput& input, const FieldReadOptions& options)
    {
        if (options.names == nullptr)
        {
            throw Error(ErrorCode::InvalidArgument, "read_field: options.names is required");
        }
        const std::uint16_t v = reader.context().file_version;
        if (v < vRelease64)
        {
            throw UnsupportedError("reflection data before version 64 is not supported", reader.absolute_position());
        }

        FieldObject out;
        out.kind = kind;
        out.header = read_object_header(reader, input);
        if (!input.is_class)
        {
            PropertyReadOptions props;
            props.names = options.names;
            props.schema = options.schema;
            props.diagnostics = options.diagnostics;
            out.properties = read_properties(reader, props);
        }

        if (kind == FieldKind::TextBuffer)
        {
            TextBufferData text;
            text.top = reader.read_u32();
            text.pos = reader.read_u32();
            text.text = reader.read_fstring();
            out.text_buffer = std::move(text);
            out.end_offset = reader.position();
            return out;
        }

        if (kind == FieldKind::MetaData)
        {
            out.metadata = read_metadata(reader);
            out.end_offset = reader.position();
            return out;
        }

        // UField
        if (v < vSuperReferenceMovedToUStruct)
        {
            out.field.super = ObjectIndex{reader.read_index()};
        }
        out.field.next = ObjectIndex{reader.read_index()};

        switch (kind)
        {
        case FieldKind::Enum: out.enum_names = read_name_list(reader); break;
        case FieldKind::Const: out.const_value = reader.read_fstring(); break;
        case FieldKind::Property: read_property(reader, out, property_type, options); break;
        case FieldKind::Struct:
        case FieldKind::ScriptStruct:
        case FieldKind::Function:
        case FieldKind::State:
        case FieldKind::Class:
        {
            read_struct(reader, out, options);
            if (kind == FieldKind::Function)
            {
                FunctionData f;
                if (v < vRelease64)
                {
                    (void)reader.read_u16();
                }
                f.native_token = reader.read_u16();
                f.oper_precedence = reader.read_u8();
                f.flags = reader.read_u32();
                if ((f.flags & FunctionFlag::Net) != 0)
                {
                    f.rep_offset = reader.read_u16();
                }
                if (v >= vMovedFriendlyNameToUFunction && !options.console_cooked)
                {
                    f.friendly_name = read_fname(reader);
                }
                out.function = f;
            }
            else if (kind == FieldKind::ScriptStruct)
            {
                ScriptStructData s;
                if (v >= vAddedStructFlagsToScriptStruct)
                {
                    s.flags = reader.read_u32();
                }
                PropertyReadOptions props;
                props.names = options.names;
                props.schema = options.schema;
                props.owner = options.owner_name;
                props.diagnostics = options.diagnostics;
                s.defaults = read_properties(reader, props);
                out.script_struct = std::move(s);
            }
            else if (kind == FieldKind::Struct)
            {
                if (v >= vUE3)
                {
                    PropertyReadOptions props;
                    props.names = options.names;
                    props.schema = options.schema;
                    props.owner = options.owner_name;
                    props.diagnostics = options.diagnostics;
                    out.struct_defaults = read_properties(reader, props);
                }
            }
            else
            {
                read_state(reader, out);
                if (kind == FieldKind::Class)
                {
                    read_class(reader, out, options);
                }
            }
            break;
        }
        case FieldKind::TextBuffer: break;
        case FieldKind::MetaData: break;
        }
        out.end_offset = reader.position();
        return out;
    }

    namespace
    {
        void write_name_list(BinaryWriter& writer, const std::vector<FName>& names)
        {
            writer.write_i32(static_cast<std::int32_t>(names.size()));
            for (const FName& n : names) write_fname(writer, n);
        }

        void write_name_object_map(BinaryWriter& writer, const std::vector<std::pair<FName, ObjectIndex>>& map)
        {
            writer.write_i32(static_cast<std::int32_t>(map.size()));
            for (const auto& [key, value] : map)
            {
                write_fname(writer, key);
                writer.write_index(value.value);
            }
        }
    }

    void write_field(BinaryWriter& writer, const FieldObject& field, std::span<const std::byte> script,
                     const ObjectHeaderInput& input, const FieldWriteOptions& options)
    {
        const std::uint16_t v = writer.context().file_version;
        if (v < vUE3)
        {
            throw UnsupportedError("write_field: versions before 178 are not supported", writer.position());
        }
        write_object_header(writer, field.header, input);
        if (!input.is_class)
        {
            write_properties(writer, field.properties, options.none);
        }
        if (field.kind == FieldKind::TextBuffer)
        {
            const TextBufferData text = field.text_buffer.value_or(TextBufferData{});
            writer.write_u32(text.top);
            writer.write_u32(text.pos);
            writer.write_fstring(text.text);
            return;
        }
        if (field.kind == FieldKind::MetaData)
        {
            writer.write_i32(static_cast<std::int32_t>(field.metadata.size()));
            for (const FieldMetadata& entry : field.metadata)
            {
                if (v <= 540)
                {
                    const auto* path = std::get_if<FString>(&entry.field);
                    if (path == nullptr) throw SerializationError("metadata field must be a path", writer.position());
                    writer.write_fstring(*path);
                }
                else
                {
                    const auto* index = std::get_if<ObjectIndex>(&entry.field);
                    if (index == nullptr) throw SerializationError("metadata field must be an object index", writer.position());
                    writer.write_index(index->value);
                }
                writer.write_i32(static_cast<std::int32_t>(entry.tags.size()));
                for (const auto& [name, value] : entry.tags)
                {
                    write_fname(writer, name);
                    writer.write_fstring(value);
                }
            }
            return;
        }

        // UField
        if (v < vSuperReferenceMovedToUStruct)
        {
            writer.write_index(field.field.super.value);
        }
        writer.write_index(field.field.next.value);

        switch (field.kind)
        {
        case FieldKind::Enum: write_name_list(writer, field.enum_names); break;
        case FieldKind::Const: writer.write_fstring(field.const_value.value_or(FString{})); break;
        case FieldKind::Property:
        {
            const PropertyData& data = field.property.value();
            writer.write_i32(data.array_dim);
            if (v >= vPropertyFlagsSizeExpandedTo64Bits) writer.write_u64(data.flags);
            else writer.write_u32(static_cast<std::uint32_t>(data.flags));
            if (!options.console_cooked)
            {
                write_fname(writer, data.category.value_or(FName{}));
                if (v >= vAddedArrayEnumToUProperty) writer.write_index(data.array_enum.value);
            }
            if ((data.flags & PropertyFlag::Net) != 0) writer.write_u16(data.rep_offset.value_or(0));
            switch (data.type)
            {
            case PropertyType::Byte:
            case PropertyType::Object:
            case PropertyType::Component:
            case PropertyType::Interface:
            case PropertyType::Struct:
            case PropertyType::Array: writer.write_index(data.reference.value); break;
            case PropertyType::Class:
            case PropertyType::Map:
                writer.write_index(data.reference.value);
                writer.write_index(data.reference2.value);
                break;
            case PropertyType::FixedArray:
                writer.write_index(data.reference.value);
                writer.write_index(data.fixed_count);
                break;
            case PropertyType::Delegate:
                writer.write_index(data.reference.value);
                if (v >= vAddedDelegateSourceToUDelegateProperty)
                {
                    if (v < vChangedDelegateSourceFromNameToObject) write_fname(writer, FName{});
                    else writer.write_index(data.reference2.value);
                }
                break;
            default: break;
            }
            break;
        }
        case FieldKind::Struct:
        case FieldKind::ScriptStruct:
        case FieldKind::Function:
        case FieldKind::State:
        case FieldKind::Class:
        {
            const StructData& data = field.structure.value();
            if (v >= vSuperReferenceMovedToUStruct) writer.write_index(field.field.super.value);
            if (!options.console_cooked) writer.write_index(data.script_text.value);
            writer.write_index(data.children.value);
            if (v >= vAddedCppTextToUStruct && !options.console_cooked) writer.write_index(data.cpp_text.value);
            if (!options.console_cooked)
            {
                writer.write_i32(data.line);
                writer.write_i32(data.text_pos);
            }
            writer.write_i32(data.script_size);
            if (v >= vAddedDataScriptSizeToUStruct) writer.write_i32(static_cast<std::int32_t>(script.size()));
            writer.write_bytes(script);

            if (field.kind == FieldKind::Function)
            {
                const FunctionData& f = field.function.value();
                writer.write_u16(f.native_token);
                writer.write_u8(f.oper_precedence);
                writer.write_u32(f.flags);
                if ((f.flags & FunctionFlag::Net) != 0) writer.write_u16(f.rep_offset.value_or(0));
                if (!options.console_cooked) write_fname(writer, f.friendly_name.value_or(FName{}));
            }
            else if (field.kind == FieldKind::ScriptStruct)
            {
                const ScriptStructData s = field.script_struct.value_or(ScriptStructData{});
                if (v >= vAddedStructFlagsToScriptStruct) writer.write_u32(s.flags);
                write_properties(writer, s.defaults, options.none);
            }
            else if (field.kind == FieldKind::Struct)
            {
                write_properties(writer, field.struct_defaults, options.none);
            }
            else
            {
                const StateData& st = field.state.value();
                if (v < vProbeMaskReducedAndIgnoreMaskRemoved)
                {
                    writer.write_u64(st.probe_mask);
                    writer.write_u64(st.ignore_mask.value_or(0));
                }
                else
                {
                    writer.write_u32(static_cast<std::uint32_t>(st.probe_mask));
                }
                writer.write_u16(st.label_table_offset);
                writer.write_u32(st.flags);
                write_name_object_map(writer, st.function_map);
                if (field.kind == FieldKind::Class)
                {
                    const ClassData& c = field.class_data.value();
                    writer.write_u32(c.flags);
                    if (v >= vClassGuidDeprecated)
                    {
                        if (v < vClassPlatformFlagsDeprecated) writer.write_u8(c.platform_flags.value_or(0));
                    }
                    else
                    {
                        writer.write_guid(Guid{});
                    }
                    writer.write_index(c.within.value);
                    write_fname(writer, c.config_name);
                    const bool old_order = v <= vHideCategoriesOldOrder;
                    if (old_order && !options.console_cooked) write_name_list(writer, c.hide_categories);
                    write_name_object_map(writer, c.component_defaults);
                    writer.write_i32(static_cast<std::int32_t>(c.interfaces.size()));
                    for (const auto& [cls, prop] : c.interfaces)
                    {
                        writer.write_i32(cls.value);
                        writer.write_i32(prop.value);
                    }
                    if (!options.console_cooked)
                    {
                        if (v >= vAddedDontSortCategoriesToUClass) write_name_list(writer, c.dont_sort_categories);
                        if (!old_order) write_name_list(writer, c.hide_categories);
                        if (v >= vAddedAutoExpandCategoriesToUClass &&
                            ((c.flags & ClassFlag::CollapseCategories) == 0 || v <= vHideCategoriesOldOrder || v >= 576))
                            write_name_list(writer, c.auto_expand_categories);
                        if (v >= vAddedAutoCollapseCategoriesToUClass) write_name_list(writer, c.auto_collapse_categories);
                        if (v >= vForceScriptOrderAddedToUClass) writer.write_bool32(c.force_script_order);
                        if (v >= vAddedClassGroupsToUClass) write_name_list(writer, c.class_groups);
                        if (v >= vAddedNativeClassNameToUClass)
                            throw UnsupportedError("write_field: the native class name (version >= 813) is not written", writer.position());
                        if (v >= vAddedDLLBindFeature) write_fname(writer, c.dll_bind_name);
                    }
                    if (v >= vDisplacedScriptPropertiesWithClassDefaultObject) writer.write_index(c.default_object.value);
                }
            }
            break;
        }
        case FieldKind::TextBuffer:
        case FieldKind::MetaData: break;
        }
    }
}
