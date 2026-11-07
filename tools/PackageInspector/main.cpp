// UnrealPackageInspector — prints package metadata (summary, names, imports, exports, depends).

#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <UnrealLib/UnrealLib.hpp>

#include "../Common/PackageJson.hpp"

namespace
{
    using namespace UnrealLib;

    void print_summary(const Package& package)
    {
        const PackageSummary& s = package.summary();
        fmt::print("Package           {}\n", package.name());
        fmt::print("File size         {} bytes{}\n", package.file_size(),
                   s.is_compressed() ? fmt::format(" ({} bytes decompressed)", package.data().size()) : "");
        fmt::print("Version           {} / licensee {}\n", s.file_version, s.licensee_version);
        fmt::print("Engine / cooker   {} / {}\n", s.engine_version, s.cooker_version);
        fmt::print("Profile           {} ({})\n", package.profile().name, package.profile().id);
        for (const auto& reason : package.profile_reasons())
        {
            fmt::print("                    - {}\n", reason);
        }
        fmt::print("Byte order        {}\n", s.endian == Endian::Little ? "little-endian" : "big-endian");
        fmt::print("Flags             0x{:08X} {}\n", s.package_flags, describe_package_flags(s.package_flags));
        fmt::print("GUID              {}\n", s.guid.to_string());
        fmt::print("Header size       {}\n", s.header_size);
        fmt::print("Names             {} @ {}\n", s.name_count, s.name_offset);
        fmt::print("Imports           {} @ {}\n", s.import_count, s.import_offset);
        fmt::print("Exports           {} @ {}\n", s.export_count, s.export_offset);
        fmt::print("Depends           @ {}\n", s.depends_offset);
        for (std::size_t i = 0; i < s.generations.size(); ++i)
        {
            const auto& g = s.generations[i];
            fmt::print("Generation {}      exports {}, names {}, net objects {}\n", i, g.export_count, g.name_count,
                       g.net_object_count);
        }
        const CompressionMethod method = s.compression_flags == 0 ? CompressionMethod::None
                                                                  : compression_method_from_flags(s.compression_flags);
        fmt::print("Compression       {} (flags 0x{:X}), {} chunk(s)\n", to_string(method), s.compression_flags,
                   s.compressed_chunks.size());
        for (const auto& c : s.compressed_chunks)
        {
            fmt::print("                    [{} +{}] <- file [{} +{}]\n", c.uncompressed_offset, c.uncompressed_size,
                       c.compressed_offset, c.compressed_size);
        }
        fmt::print("Package source    0x{:08X}\n", s.package_source);
    }

    void print_names(const Package& package, std::size_t limit)
    {
        fmt::print("\nNames ({}):\n", package.names().size());
        const auto entries = package.names().entries();
        for (std::size_t i = 0; i < entries.size() && i < limit; ++i)
        {
            fmt::print("  {:6}  {:<40} 0x{:016X}\n", i, entries[i].text.text, entries[i].flags);
        }
    }

    void print_imports(const Package& package, std::size_t limit, Linker* linker)
    {
        fmt::print("\nImports ({}):\n", package.imports().size());
        std::map<std::string, std::size_t> statuses;
        for (std::size_t i = 0; i < package.imports().size(); ++i)
        {
            const ObjectIndex index = ObjectIndex::from_import(i);
            std::string resolution;
            if (linker)
            {
                const Resolution& r = linker->resolve(package, index);
                ++statuses[std::string(to_string(r.status))];
                resolution = r.status == ResolveStatus::Export
                                 ? fmt::format("  -> {} (in {})", linker->qualified_path(r.target),
                                               r.target.package->name())
                                 : fmt::format("  [{}] {}", to_string(r.status), r.detail);
            }
            if (i >= limit)
            {
                continue;
            }
            const ImportEntry& e = package.imports()[i];
            fmt::print("  {:6}  {}.{}  {}{}\n", index.value,
                       package.names().try_to_string(e.class_package).value_or("?"),
                       package.names().try_to_string(e.class_name).value_or("?"), package.object_path(index),
                       resolution);
        }
        for (const auto& [status, count] : statuses)
        {
            fmt::print("  resolution {}: {}\n", status, count);
        }
    }

    void print_exports(const Package& package, std::size_t limit)
    {
        fmt::print("\nExports ({}):\n", package.exports().size());
        for (std::size_t i = 0; i < package.exports().size() && i < limit; ++i)
        {
            const ObjectIndex index = ObjectIndex::from_export(i);
            const ExportEntry& e = package.exports()[i];
            const std::string cls = package.is_valid(e.class_index) ? package.class_name(index) : "<invalid>";
            fmt::print("  {:6}  {:<24} {:<50} off {:>9} size {:>8} flags 0x{:016X}\n", index.value, cls,
                       package.object_path(index), e.serial_offset, e.serial_size, e.object_flags);
        }
    }

    void print_depends(const Package& package, std::size_t limit)
    {
        fmt::print("\nDepends ({} lists):\n", package.depends().size());
        for (std::size_t i = 0; i < package.depends().size() && i < limit; ++i)
        {
            if (package.depends()[i].empty())
            {
                continue;
            }
            fmt::print("  {:6}  {}:", i + 1, package.object_path(ObjectIndex::from_export(i)));
            for (const ObjectIndex d : package.depends()[i])
            {
                fmt::print(" {}", package.is_valid(d) ? package.object_path(d) : fmt::format("<invalid {}>", d.value));
            }
            fmt::print("\n");
        }
    }
}

int main(int argc, char** argv)
{
    CLI::App app{"Inspect an Unreal Engine 3 package (UnrealLib)", "UnrealPackageInspector"};
    argv = app.ensure_utf8(argv);

    std::string path;
    std::string profile;
    std::vector<std::string> search_paths;
    bool names = false;
    bool imports = false;
    bool exports = false;
    bool depends = false;
    bool all = false;
    bool json = false;
    std::size_t limit = static_cast<std::size_t>(-1);
    bool verbose = false;

    app.add_option("package", path, "Package file (.u, .upk, ...)")->required()->check(CLI::ExistingFile);
    app.add_option("--profile", profile, "Game profile id or name (default: auto-detect)");
    app.add_option("-s,--search-path", search_paths,
                   "Directory (searched recursively) used to resolve imports; adds a resolution column")
        ->check(CLI::ExistingDirectory);
    app.add_flag("-n,--names", names, "List the name table");
    app.add_flag("-i,--imports", imports, "List the import table");
    app.add_flag("-e,--exports", exports, "List the export table");
    app.add_flag("-d,--depends", depends, "List the depends table");
    app.add_flag("-a,--all", all, "List every table");
    app.add_flag("--json", json, "Print normalized JSON instead of text");
    app.add_option("--limit", limit, "Maximum number of rows per table");
    app.add_flag("-v,--verbose", verbose, "Enable debug logging");
    CLI11_PARSE(app, argc, argv);

    if (verbose)
    {
        UnrealLib::set_log_level(UnrealLib::LogLevel::Debug);
    }
    if (all)
    {
        names = imports = exports = depends = true;
    }

    try
    {
        UnrealLib::LoadOptions options;
        if (!profile.empty())
        {
            options.profile = profile;
        }
        std::unique_ptr<UnrealLib::Linker> linker;
        UnrealLib::Package standalone;
        const UnrealLib::Package* loaded = nullptr;
        if (!search_paths.empty())
        {
            linker = std::make_unique<UnrealLib::Linker>(options);
            for (const auto& dir : search_paths)
            {
                linker->add_search_path(UnrealLib::Tools::utf8_path(dir));
            }
            loaded = &linker->load(UnrealLib::Tools::utf8_path(path));
        }
        else
        {
            standalone = UnrealLib::Package::open(UnrealLib::Tools::utf8_path(path), options);
            loaded = &standalone;
        }
        const UnrealLib::Package& package = *loaded;

        if (json)
        {
            UnrealLib::Tools::JsonOptions json_options{names, imports, exports, depends};
            auto j = UnrealLib::Tools::package_to_json(package, json_options);
            j["diagnostics"] = UnrealLib::Tools::diagnostics_to_json(package.diagnostics());
            std::cout << j.dump(2) << '\n';
            return 0;
        }

        print_summary(package);
        if (names) print_names(package, limit);
        if (imports) print_imports(package, limit, linker.get());
        if (exports) print_exports(package, limit);
        if (depends) print_depends(package, limit);
        for (const auto& d : package.diagnostics().entries())
        {
            fmt::print(stderr, "{}: [{}] {}\n", to_string(d.severity), d.code, d.message);
        }
        return 0;
    }
    catch (const UnrealLib::Error& e)
    {
        fmt::print(stderr, "error ({}): {}\n", to_string(e.code()), e.what());
        return 2;
    }
}
