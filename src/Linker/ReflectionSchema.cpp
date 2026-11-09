#include <UnrealLib/Linker/ReflectionSchema.hpp>

#include <unordered_set>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>

namespace UnrealLib
{
    ReflectionSchema::ReflectionSchema(Linker& linker, bool cooked) : m_linker(linker), m_cooked(cooked) {}

    void ReflectionSchema::add_package(const Package& package)
    {
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ExportEntry& e = package.exports()[i];
            const ObjectIndex index = ObjectIndex::from_export(i);
            const bool is_class = e.class_index.is_null();
            if (!is_class)
            {
                const std::string class_name = package.class_name(index);
                if (!Text::iequals(class_name, "ScriptStruct"))
                {
                    continue;
                }
            }
            m_types.try_emplace(package.object_name(index), ObjectRef{&package, index});
        }
    }

    std::optional<ObjectRef> ReflectionSchema::find_type(std::string_view name) const
    {
        const auto it = m_types.find(name);
        if (it == m_types.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    const FieldObject* ReflectionSchema::field(ObjectRef ref) const
    {
        if (!ref.is_export())
        {
            return nullptr;
        }
        const auto key = std::make_pair(ref.package, ref.index.value);
        if (const auto it = m_fields.find(key); it != m_fields.end())
        {
            return it->second.get();
        }
        std::unique_ptr<FieldObject> result;
        std::string failure;
        try
        {
            // Struct defaults are not needed for layouts; read them with the builtin schema to avoid recursion.
            result = std::make_unique<FieldObject>(m_linker.read_field(ref, nullptr));
        }
        catch (const Error& e)
        {
            failure = e.what();
        }
        if (!result)
        {
            m_diagnostics.warning("reflection.unreadable",
                                  fmt::format("{}: {}", m_linker.qualified_path(ref), failure));
        }
        const FieldObject* out = result.get();
        m_fields.emplace(key, std::move(result));
        return out;
    }

    std::optional<ObjectRef> ReflectionSchema::resolve_export(ObjectRef ref) const
    {
        if (ref.index.is_null() || ref.package == nullptr)
        {
            return std::nullopt;
        }
        const Resolution& r = m_linker.resolve(ref);
        if (r.status != ResolveStatus::Export)
        {
            return std::nullopt;
        }
        return r.target;
    }

    std::optional<ObjectRef> ReflectionSchema::find_member(ObjectRef owner, std::string_view name) const
    {
        std::unordered_set<const FieldObject*> visited_structs;
        std::optional<ObjectRef> current = owner;
        while (current)
        {
            const FieldObject* s = field(*current);
            if (s == nullptr || !s->structure || !visited_structs.insert(s).second)
            {
                return std::nullopt;
            }
            std::unordered_set<std::int32_t> visited;
            for (ObjectIndex child = s->structure->children; !child.is_null();)
            {
                if (!visited.insert(child.value).second || !current->package->is_valid(child) || !child.is_export())
                {
                    break;
                }
                const ObjectRef child_ref{current->package, child};
                if (Text::iequals(current->package->object_name(child), name) &&
                    field_kind_of(current->package->class_name(child)) == FieldKind::Property)
                {
                    return child_ref;
                }
                const FieldObject* f = field(child_ref);
                if (f == nullptr)
                {
                    break;
                }
                child = f->field.next;
            }
            current = resolve_export(ObjectRef{current->package, s->field.super});
        }
        return std::nullopt;
    }

    std::optional<TypeDesc> ReflectionSchema::type_of(ObjectRef property) const
    {
        const FieldObject* f = field(property);
        if (f == nullptr || !f->property)
        {
            return std::nullopt;
        }
        const PropertyData& p = *f->property;
        TypeDesc desc;
        desc.type = p.type;
        const Package& pkg = *property.package;
        switch (p.type)
        {
        case PropertyType::Byte: desc.is_enum = !p.reference.is_null(); break;
        case PropertyType::Struct:
            if (!pkg.is_valid(p.reference) || p.reference.is_null())
            {
                return std::nullopt;
            }
            desc.struct_name = pkg.object_name(p.reference);
            break;
        case PropertyType::Array:
        case PropertyType::FixedArray:
        {
            const auto inner = resolve_export(ObjectRef{&pkg, p.reference});
            if (!inner)
            {
                return std::nullopt;
            }
            auto inner_type = type_of(*inner);
            if (!inner_type)
            {
                return std::nullopt;
            }
            desc.inner = std::make_shared<TypeDesc>(std::move(*inner_type));
            desc.fixed_count = p.fixed_count;
            break;
        }
        case PropertyType::Map:
        {
            const auto key = resolve_export(ObjectRef{&pkg, p.reference});
            const auto value = resolve_export(ObjectRef{&pkg, p.reference2});
            auto key_type = key ? type_of(*key) : std::nullopt;
            auto value_type = value ? type_of(*value) : std::nullopt;
            if (key_type)
            {
                desc.inner = std::make_shared<TypeDesc>(std::move(*key_type));
            }
            if (value_type)
            {
                desc.value = std::make_shared<TypeDesc>(std::move(*value_type));
            }
            break;
        }
        default: break;
        }
        return desc;
    }

    std::optional<TypeDesc> ReflectionSchema::find_property(std::string_view owner, std::string_view property) const
    {
        const auto key = std::make_pair(Text::to_lower_ascii(owner), Text::to_lower_ascii(property));
        if (const auto it = m_property_cache.find(key); it != m_property_cache.end())
        {
            return it->second;
        }
        std::optional<TypeDesc> result;
        if (const auto type = find_type(owner))
        {
            if (const auto member = find_member(*type, property))
            {
                result = type_of(*member);
            }
        }
        m_property_cache.emplace(key, result);
        return result;
    }

    const StructLayout* ReflectionSchema::find_struct(std::string_view name) const
    {
        if (const auto it = m_layouts.find(name); it != m_layouts.end())
        {
            return it->second.get();
        }
        std::unique_ptr<StructLayout> layout;
        const auto type = find_type(name);
        const FieldObject* s = type ? field(*type) : nullptr;
        if (s != nullptr && s->script_struct)
        {
            layout = std::make_unique<StructLayout>();
            layout->name = std::string(name);
            const std::uint32_t flags = s->script_struct->flags;
            layout->binary = (flags & StructFlag::Immutable) != 0 ||
                             (m_cooked && (flags & StructFlag::ImmutableWhenCooked) != 0);

            // Collect the chain (struct, super, super's super, …), then emit fields from the root down.
            std::vector<std::pair<ObjectRef, const FieldObject*>> chain;
            std::unordered_set<const FieldObject*> seen;
            std::optional<ObjectRef> current = *type;
            bool complete = true;
            while (current)
            {
                const FieldObject* f = field(*current);
                if (f == nullptr || !f->structure || !seen.insert(f).second)
                {
                    complete = false;
                    break;
                }
                chain.emplace_back(*current, f);
                if (f->field.super.is_null())
                {
                    break;
                }
                current = resolve_export(ObjectRef{current->package, f->field.super});
                if (!current)
                {
                    complete = false;
                }
            }
            for (auto it = chain.rbegin(); complete && it != chain.rend(); ++it)
            {
                const auto& [ref, f] = *it;
                std::unordered_set<std::int32_t> visited;
                for (ObjectIndex child = f->structure->children; !child.is_null();)
                {
                    if (!visited.insert(child.value).second || !child.is_export() || !ref.package->is_valid(child))
                    {
                        break;
                    }
                    const ObjectRef child_ref{ref.package, child};
                    const FieldObject* cf = field(child_ref);
                    if (cf == nullptr)
                    {
                        complete = false;
                        break;
                    }
                    if (cf->property)
                    {
                        auto type_desc = type_of(child_ref);
                        if (!type_desc)
                        {
                            complete = false;
                            break;
                        }
                        layout->fields.push_back(StructField{ref.package->object_name(child), std::move(*type_desc),
                                                             cf->property->dimension()});
                    }
                    child = cf->field.next;
                }
            }
            if (!complete)
            {
                layout.reset();
            }
        }
        const StructLayout* out = layout.get();
        m_layouts.emplace(std::string(name), std::move(layout));
        return out;
    }
}
