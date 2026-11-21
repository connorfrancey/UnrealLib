#include <UnrealLib/Assets/Particle.hpp>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>

namespace UnrealLib::Assets
{
    namespace
    {
        AssetSource open_particle(Linker& linker, ObjectRef object, const PropertySchema& schema,
                                  std::string_view class_path)
        {
            if (!object.is_export() || !object.package->is_valid(object.index) ||
                !linker.is_a(object, class_path).value_or(false))
            {
                throw Error(ErrorCode::InvalidArgument, fmt::format("expected an export of class {}", class_path));
            }
            return open_asset(linker, object, &schema);
        }

        ParticleLink read_link(Linker& linker, const AssetSource& source, const Property& property,
                               const PropertyValue& value)
        {
            const auto* object = std::get_if<ObjectValue>(&value.value);
            if (object == nullptr)
            {
                throw Error(ErrorCode::InvalidData, fmt::format("{}: {} contains a non-object value",
                            linker.qualified_path(source.object), property.name));
            }
            return {object->value, linker.resolve(*source.object.package, object->value)};
        }

        ParticleLink read_link(Linker& linker, const AssetSource& source, std::string_view name)
        {
            const Property* property = source.properties.find(name);
            return property ? read_link(linker, source, *property, property->value) : ParticleLink{};
        }

        std::vector<ParticleLink> read_links(Linker& linker, const AssetSource& source, std::string_view name)
        {
            const Property* property = source.properties.find(name);
            if (property == nullptr)
            {
                return {};
            }
            const ArrayValue* array = source.properties.get_array(name);
            if (array == nullptr)
            {
                throw Error(ErrorCode::InvalidData, fmt::format("{}: could not decode {} as a particle object array",
                            linker.qualified_path(source.object), name));
            }
            std::vector<ParticleLink> links;
            links.reserve(array->elements.size());
            for (const PropertyValue& value : array->elements)
            {
                links.push_back(read_link(linker, source, *property, value));
            }
            return links;
        }
    }

    ParticleSystem read_particle_system(Linker& linker, ObjectRef object, const PropertySchema& schema)
    {
        AssetSource source = open_particle(linker, object, schema, "Engine.ParticleSystem");
        auto emitters = read_links(linker, source, "Emitters");
        return {std::move(source), std::move(emitters)};
    }

    ParticleEmitter read_particle_emitter(Linker& linker, ObjectRef object, const PropertySchema& schema)
    {
        AssetSource source = open_particle(linker, object, schema, "Engine.ParticleEmitter");
        auto lod_levels = read_links(linker, source, "LODLevels");
        return {std::move(source), std::move(lod_levels)};
    }

    ParticleLodLevel read_particle_lod_level(Linker& linker, ObjectRef object, const PropertySchema& schema)
    {
        AssetSource source = open_particle(linker, object, schema, "Engine.ParticleLODLevel");
        auto required = read_link(linker, source, "RequiredModule");
        auto spawn = read_link(linker, source, "SpawnModule");
        auto type_data = read_link(linker, source, "TypeDataModule");
        auto modules = read_links(linker, source, "Modules");
        return {std::move(source), std::move(required), std::move(spawn), std::move(type_data), std::move(modules)};
    }
}
