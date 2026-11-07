#include <UnrealLib/Assets/Material.hpp>

#include <algorithm>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib::Assets
{
    namespace
    {
        bool is_texture_class(const std::string& cls)
        {
            return cls.find("Texture") != std::string::npos && cls.find("MaterialExpression") == std::string::npos;
        }

        /// Adds every object reference in @p value that names a texture.
        void collect_textures(const Package& package, const PropertyValue& value, std::vector<ObjectIndex>& out)
        {
            if (const auto* o = std::get_if<ObjectValue>(&value.value))
            {
                if (!o->value.is_null() && package.is_valid(o->value) && is_texture_class(package.class_name(o->value)) &&
                    std::find(out.begin(), out.end(), o->value) == out.end())
                {
                    out.push_back(o->value);
                }
            }
            else if (const auto* a = std::get_if<ArrayValue>(&value.value))
            {
                for (const PropertyValue& e : a->elements) collect_textures(package, e, out);
            }
            else if (const auto* s = std::get_if<StructValue>(&value.value))
            {
                for (const Property& f : s->fields) collect_textures(package, f.value, out);
            }
        }

        const Property* field(const StructValue& s, std::string_view name)
        {
            return find_property(s.fields, name);
        }

        void read_parameters(const Package& package, const AssetProperties& p, std::string_view array_name,
                             MaterialParameter::Kind kind, MaterialInfo& info)
        {
            const Property* prop = p.find(array_name);
            if (prop == nullptr) return;
            const auto* array = std::get_if<ArrayValue>(&prop->value.value);
            if (array == nullptr || !array->decoded())
            {
                ++info.undecoded_parameters;
                return;
            }
            for (const PropertyValue& e : array->elements)
            {
                const auto* s = std::get_if<StructValue>(&e.value);
                if (s == nullptr)
                {
                    ++info.undecoded_parameters;
                    continue;
                }
                MaterialParameter param;
                param.kind = kind;
                if (const Property* n = field(*s, "ParameterName"))
                {
                    if (const auto* v = std::get_if<NameValue>(&n->value.value)) param.name = package.names().to_string(v->value);
                }
                if (const Property* v = field(*s, "ParameterValue"))
                {
                    if (const auto* f = std::get_if<float>(&v->value.value)) param.scalar = *f;
                    if (const auto* o = std::get_if<ObjectValue>(&v->value.value)) param.texture = o->value;
                    if (const auto* c = std::get_if<StructValue>(&v->value.value))
                    {
                        constexpr std::array<std::string_view, 4> channels{"R", "G", "B", "A"};
                        for (std::size_t i = 0; i < 4; ++i)
                        {
                            if (const Property* ch = field(*c, channels[i]))
                            {
                                if (const auto* f = std::get_if<float>(&ch->value.value)) param.vector[i] = *f;
                            }
                        }
                    }
                }
                info.parameters.push_back(std::move(param));
            }
        }
    }

    bool is_material(Linker& linker, ObjectRef object)
    {
        const std::string cls = object.package->class_name(object.index);
        if (cls == "Material" || cls == "MaterialInstanceConstant" || cls == "MaterialInstanceTimeVarying")
        {
            return true;
        }
        return linker.is_a(object, "Engine.MaterialInterface").value_or(false);
    }

    MaterialInfo read_material(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        if (!is_material(linker, object))
        {
            throw Error(ErrorCode::InvalidArgument,
                        fmt::format("'{}' is not a material", linker.qualified_path(object)));
        }
        const AssetSource source = open_asset(linker, object, schema);
        const Package& package = *object.package;
        const AssetProperties& p = source.properties;

        MaterialInfo info;
        info.name = package.object_name(object.index);
        info.class_name = source.class_name;
        info.parent = p.get_object("Parent").value_or(ObjectIndex{});
        info.blend_mode = p.get_name("BlendMode").value_or("BLEND_Opaque");
        info.two_sided = p.get_bool("TwoSided").value_or(false);
        info.native_bytes = static_cast<std::size_t>(object.export_entry().serial_size) - source.native_offset;
        read_parameters(package, p, "ScalarParameterValues", MaterialParameter::Kind::Scalar, info);
        read_parameters(package, p, "VectorParameterValues", MaterialParameter::Kind::Vector, info);
        read_parameters(package, p, "TextureParameterValues", MaterialParameter::Kind::Texture, info);
        for (const Property& prop : p.all()) collect_textures(package, prop.value, info.textures);

        // Expressions: subobjects of the material whose class is a MaterialExpression.
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ObjectIndex index = ObjectIndex::from_export(i);
            if (package.exports()[i].outer != object.index ||
                package.class_name(index).rfind("MaterialExpression", 0) != 0 ||
                (package.exports()[i].object_flags & ObjectFlag::ClassDefaultObject) != 0)
            {
                continue;
            }
            info.expressions.push_back(index);
            const ObjectProperties expression = linker.read_properties(ObjectRef{&package, index}, schema);
            for (const Property& prop : expression.properties) collect_textures(package, prop.value, info.textures);
        }
        info.properties = source.properties;
        return info;
    }
}
