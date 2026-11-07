#include "PackageJson.hpp"

#include <fmt/format.h>

namespace UnrealLib::Tools
{
    namespace
    {
        std::string hex32(std::uint32_t value)
        {
            return fmt::format("0x{:08X}", value);
        }

        std::string hex64(std::uint64_t value)
        {
            return fmt::format("0x{:016X}", value);
        }

        std::string name_text(const Package& package, const FName& name)
        {
            return package.names().try_to_string(name).value_or(fmt::format("<name#{}>", name.index));
        }

        nlohmann::ordered_json reference(const Package& package, ObjectIndex index)
        {
            if (index.is_null())
            {
                return nullptr;
            }
            if (!package.is_valid(index))
            {
                return fmt::format("<invalid {}>", index.value);
            }
            return package.object_path(index);
        }
    }

    nlohmann::ordered_json summary_to_json(const Package& package)
    {
        const PackageSummary& s = package.summary();
        nlohmann::ordered_json j;
        j["name"] = package.name();
        j["file_size"] = package.file_size();
        j["file_version"] = s.file_version;
        j["licensee_version"] = s.licensee_version;
        j["engine_version"] = s.engine_version;
        j["cooker_version"] = s.cooker_version;
        j["endian"] = s.endian == Endian::Little ? "little" : "big";
        j["header_size"] = s.header_size;
        j["folder_name"] = s.folder_name.text;
        j["package_flags"] = hex32(s.package_flags);
        j["package_flags_text"] = describe_package_flags(s.package_flags);
        j["guid"] = s.guid.to_string();
        j["names"] = {{"count", s.name_count}, {"offset", s.name_offset}};
        j["imports"] = {{"count", s.import_count}, {"offset", s.import_offset}};
        j["exports"] = {{"count", s.export_count}, {"offset", s.export_offset}};
        j["depends_offset"] = s.depends_offset;
        auto generations = nlohmann::ordered_json::array();
        for (const auto& g : s.generations)
        {
            generations.push_back(
                {{"exports", g.export_count}, {"names", g.name_count}, {"net_objects", g.net_object_count}});
        }
        j["generations"] = std::move(generations);
        j["compression_flags"] = hex32(s.compression_flags);
        auto chunks = nlohmann::ordered_json::array();
        for (const auto& c : s.compressed_chunks)
        {
            chunks.push_back({{"uncompressed_offset", c.uncompressed_offset},
                              {"uncompressed_size", c.uncompressed_size},
                              {"compressed_offset", c.compressed_offset},
                              {"compressed_size", c.compressed_size}});
        }
        j["compressed_chunks"] = std::move(chunks);
        j["package_source"] = hex32(s.package_source);
        auto cook = nlohmann::ordered_json::array();
        for (const auto& p : s.additional_packages_to_cook)
        {
            cook.push_back(p.text);
        }
        j["additional_packages_to_cook"] = std::move(cook);
        j["profile"] = package.profile().id;
        return j;
    }

    nlohmann::ordered_json package_to_json(const Package& package, const JsonOptions& options)
    {
        nlohmann::ordered_json j;
        j["summary"] = summary_to_json(package);

        if (options.names)
        {
            auto names = nlohmann::ordered_json::array();
            for (const auto& n : package.names().entries())
            {
                names.push_back({{"name", n.text.text}, {"flags", hex64(n.flags)}});
            }
            j["names"] = std::move(names);
        }

        if (options.imports)
        {
            auto imports = nlohmann::ordered_json::array();
            for (std::size_t i = 0; i < package.imports().size(); ++i)
            {
                const ImportEntry& e = package.imports()[i];
                imports.push_back({{"index", ObjectIndex::from_import(i).value},
                                   {"path", package.object_path(ObjectIndex::from_import(i))},
                                   {"class_package", name_text(package, e.class_package)},
                                   {"class", name_text(package, e.class_name)},
                                   {"outer", reference(package, e.outer)}});
            }
            j["imports"] = std::move(imports);
        }

        if (options.exports)
        {
            auto exports = nlohmann::ordered_json::array();
            for (std::size_t i = 0; i < package.exports().size(); ++i)
            {
                const ExportEntry& e = package.exports()[i];
                const ObjectIndex self = ObjectIndex::from_export(i);
                nlohmann::ordered_json x;
                x["index"] = self.value;
                x["path"] = package.object_path(self);
                x["class"] = package.is_valid(e.class_index) ? package.class_name(self) : "<invalid>";
                x["super"] = reference(package, e.super_index);
                x["outer"] = reference(package, e.outer);
                x["archetype"] = reference(package, e.archetype);
                x["object_flags"] = hex64(e.object_flags);
                x["export_flags"] = hex32(e.export_flags);
                x["serial_offset"] = e.serial_offset;
                x["serial_size"] = e.serial_size;
                x["package_flags"] = hex32(e.package_flags);
                if (!e.component_map.empty())
                {
                    auto components = nlohmann::ordered_json::array();
                    for (const auto& c : e.component_map)
                    {
                        components.push_back({{"name", name_text(package, c.name)},
                                              {"object", reference(package, c.object)}});
                    }
                    x["component_map"] = std::move(components);
                }
                if (options.depends && i < package.depends().size())
                {
                    auto depends = nlohmann::ordered_json::array();
                    for (const ObjectIndex d : package.depends()[i])
                    {
                        depends.push_back(reference(package, d));
                    }
                    x["depends"] = std::move(depends);
                }
                exports.push_back(std::move(x));
            }
            j["exports"] = std::move(exports);
        }
        return j;
    }

    nlohmann::ordered_json diagnostics_to_json(const Diagnostics& diagnostics)
    {
        auto out = nlohmann::ordered_json::array();
        for (const auto& d : diagnostics.entries())
        {
            nlohmann::ordered_json j;
            j["severity"] = std::string(to_string(d.severity));
            j["code"] = d.code;
            j["message"] = d.message;
            if (d.offset)
            {
                j["offset"] = *d.offset;
            }
            out.push_back(std::move(j));
        }
        return out;
    }
}
