#include <UnrealLib/Assets/AssetObject.hpp>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>

namespace UnrealLib::Assets
{
    const Property* AssetProperties::find(std::string_view name, std::int32_t array_index) const
    {
        return find_property(m_properties, name, array_index);
    }

    namespace
    {
        template <typename T>
        const T* value_of(const Property* p)
        {
            return p ? std::get_if<T>(&p->value.value) : nullptr;
        }
    }

    std::optional<std::int32_t> AssetProperties::get_int(std::string_view name, std::int32_t index) const
    {
        const auto* v = value_of<std::int32_t>(find(name, index));
        return v ? std::optional(*v) : std::nullopt;
    }

    std::optional<float> AssetProperties::get_float(std::string_view name, std::int32_t index) const
    {
        const auto* v = value_of<float>(find(name, index));
        return v ? std::optional(*v) : std::nullopt;
    }

    std::optional<bool> AssetProperties::get_bool(std::string_view name, std::int32_t index) const
    {
        const auto* v = value_of<bool>(find(name, index));
        return v ? std::optional(*v) : std::nullopt;
    }

    std::optional<std::uint8_t> AssetProperties::get_byte(std::string_view name, std::int32_t index) const
    {
        const auto* v = value_of<std::uint8_t>(find(name, index));
        return v ? std::optional(*v) : std::nullopt;
    }

    std::optional<std::string> AssetProperties::get_name(std::string_view name, std::int32_t index) const
    {
        const Property* p = find(name, index);
        if (const auto* e = value_of<EnumValue>(p))
        {
            return to_string(e->value);
        }
        if (const auto* n = value_of<NameValue>(p))
        {
            return to_string(n->value);
        }
        return std::nullopt;
    }

    std::optional<std::string> AssetProperties::get_string(std::string_view name, std::int32_t index) const
    {
        const auto* v = value_of<FString>(find(name, index));
        return v ? std::optional(v->text) : std::nullopt;
    }

    std::optional<ObjectIndex> AssetProperties::get_object(std::string_view name, std::int32_t index) const
    {
        const auto* v = value_of<ObjectValue>(find(name, index));
        return v ? std::optional(v->value) : std::nullopt;
    }

    const ArrayValue* AssetProperties::get_array(std::string_view name) const
    {
        const auto* v = value_of<ArrayValue>(find(name));
        return v && v->decoded() ? v : nullptr;
    }

    const StructValue* AssetProperties::get_struct(std::string_view name, std::int32_t index) const
    {
        return value_of<StructValue>(find(name, index));
    }

    std::string AssetProperties::to_string(const FName& name) const
    {
        return m_package ? m_package->names().to_string(name) : std::string();
    }

    BinaryReader AssetSource::native_reader() const
    {
        const ExportEntry& entry = object.export_entry();
        BinaryReader reader(object.package->export_data(object.index.export_index()), object.package->context(),
                            static_cast<std::uint64_t>(entry.serial_offset));
        reader.seek(native_offset);
        return reader;
    }

    AssetSource open_asset(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        if (!object.is_export())
        {
            throw Error(ErrorCode::InvalidArgument, "an asset must be an export");
        }
        if ((object.export_entry().object_flags & ObjectFlag::ClassDefaultObject) != 0)
        {
            throw Error(ErrorCode::InvalidArgument,
                        fmt::format("'{}' is a class default object: it has no asset data", linker.qualified_path(object)));
        }
        ObjectProperties props = linker.read_properties(object, schema);
        AssetSource source;
        source.object = object;
        source.class_name = object.package->class_name(object.index);
        source.native_offset = props.end_offset;
        source.properties = AssetProperties(*object.package, std::move(props.properties));
        return source;
    }

    void expect_end(const BinaryReader& reader, std::string_view what)
    {
        if (!reader.at_end())
        {
            throw SerializationError(fmt::format("{}: {} unread bytes after the native data", what, reader.remaining()),
                                     reader.absolute_position());
        }
    }
}
