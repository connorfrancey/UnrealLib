#include <UnrealLib/Package/ObjectTables.hpp>

#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

namespace UnrealLib
{
    ImportEntry read_import_entry(BinaryReader& reader)
    {
        ImportEntry entry;
        entry.class_package = read_fname(reader);
        entry.class_name = read_fname(reader);
        entry.outer = ObjectIndex{reader.read_i32()};
        entry.object_name = read_fname(reader);
        return entry;
    }

    void write_import_entry(BinaryWriter& writer, const ImportEntry& entry)
    {
        write_fname(writer, entry.class_package);
        write_fname(writer, entry.class_name);
        writer.write_i32(entry.outer.value);
        write_fname(writer, entry.object_name);
    }

    ExportEntry read_export_entry(BinaryReader& reader)
    {
        const SerializationContext& ctx = reader.context();
        ExportEntry entry;
        entry.class_index = ObjectIndex{reader.read_index()};
        entry.super_index = ObjectIndex{reader.read_index()};
        entry.outer = ObjectIndex{reader.read_i32()};
        entry.object_name = read_fname(reader);
        if (ctx.at_least(LegacyVersion::ArchetypeAddedToExports))
        {
            entry.archetype = ObjectIndex{reader.read_i32()};
        }

        entry.object_flags = ctx.at_least(LegacyVersion::ObjectFlagsSizeExpandedTo64Bits) ? reader.read_u64()
                                                                                          : reader.read_u32();

        entry.serial_size = reader.read_index();
        if (entry.serial_size > 0 || ctx.at_least(LegacyVersion::SerialSizeConditionRemoved))
        {
            entry.serial_offset = reader.read_index();
        }

        if (ctx.at_least(LegacyVersion::AddedComponentMapToExports) &&
            !ctx.at_least(LegacyVersion::ComponentMapDeprecated))
        {
            const std::uint32_t count = reader.read_array_count(12);
            entry.component_map.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                ComponentMapEntry component;
                component.name = read_fname(reader);
                component.object = ObjectIndex{reader.read_i32()};
                entry.component_map.push_back(component);
            }
        }

        if (!ctx.at_least(LegacyVersion::ExportFlagsAddedToExports))
        {
            return entry;
        }
        entry.export_flags = reader.read_u32();

        if (!ctx.at_least(LegacyVersion::NetObjectCountAdded))
        {
            return entry;
        }
        const std::uint32_t generations = reader.read_array_count(4);
        entry.generation_net_object_count.reserve(generations);
        for (std::uint32_t i = 0; i < generations; ++i)
        {
            entry.generation_net_object_count.push_back(reader.read_i32());
        }
        entry.package_guid = reader.read_guid();

        if (ctx.at_least(LegacyVersion::PackageFlagsAddedToExports))
        {
            entry.package_flags = reader.read_u32();
        }
        return entry;
    }

    void write_export_entry(BinaryWriter& writer, const ExportEntry& entry)
    {
        const SerializationContext& ctx = writer.context();
        writer.write_index(entry.class_index.value);
        writer.write_index(entry.super_index.value);
        writer.write_i32(entry.outer.value);
        write_fname(writer, entry.object_name);
        if (ctx.at_least(LegacyVersion::ArchetypeAddedToExports))
        {
            writer.write_i32(entry.archetype.value);
        }
        if (ctx.at_least(LegacyVersion::ObjectFlagsSizeExpandedTo64Bits))
        {
            writer.write_u64(entry.object_flags);
        }
        else
        {
            writer.write_u32(static_cast<std::uint32_t>(entry.object_flags));
        }
        writer.write_index(entry.serial_size);
        if (entry.serial_size > 0 || ctx.at_least(LegacyVersion::SerialSizeConditionRemoved))
        {
            writer.write_index(entry.serial_offset);
        }
        if (ctx.at_least(LegacyVersion::AddedComponentMapToExports) &&
            !ctx.at_least(LegacyVersion::ComponentMapDeprecated))
        {
            writer.write_i32(static_cast<std::int32_t>(entry.component_map.size()));
            for (const auto& component : entry.component_map)
            {
                write_fname(writer, component.name);
                writer.write_i32(component.object.value);
            }
        }
        if (!ctx.at_least(LegacyVersion::ExportFlagsAddedToExports))
        {
            return;
        }
        writer.write_u32(entry.export_flags);
        if (!ctx.at_least(LegacyVersion::NetObjectCountAdded))
        {
            return;
        }
        writer.write_i32(static_cast<std::int32_t>(entry.generation_net_object_count.size()));
        for (const std::int32_t count : entry.generation_net_object_count)
        {
            writer.write_i32(count);
        }
        writer.write_guid(entry.package_guid);
        if (ctx.at_least(LegacyVersion::PackageFlagsAddedToExports))
        {
            writer.write_u32(entry.package_flags);
        }
    }
}
