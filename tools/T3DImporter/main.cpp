// UnrealT3DImporter: applies an edited T3D level export to its package and writes the modified package.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <UnrealLib/UnrealLib.hpp>

#include "../Common/PackageJson.hpp"

using namespace UnrealLib;
namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    CLI::App app{"Apply an edited T3D level (from UnrealPackageExtractor -t levels) to its package and write the "
                 "modified package (UnrealLib)",
                 "UnrealT3DImporter"};
    argv = app.ensure_utf8(argv);

    std::string package_path;
    std::string t3d_path;
    std::string output;
    std::string profile;
    std::string compression = "none";
    std::vector<std::string> search_paths;
    bool dry_run = false;

    app.add_option("package", package_path, "The level package the T3D was exported from")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("t3d", t3d_path, "The edited T3D file")->required()->check(CLI::ExistingFile);
    app.add_option("-o,--output", output, "Output package file (must differ from the input)");
    app.add_option("--profile", profile, "Game profile id or name (default: auto-detect)");
    app.add_option("-s,--search-path", search_paths,
                   "Directories with the game's packages (default: the nearest directory at or above the package "
                   "with Core.u)")
        ->check(CLI::ExistingDirectory);
    app.add_option("--compression", compression, "Compression of the written package: none, zlib or lzo")
        ->check(CLI::IsMember({"none", "zlib", "lzo"}));
    app.add_flag("-n,--dry-run", dry_run, "Report what would change without writing");
    CLI11_PARSE(app, argc, argv);
    if (!dry_run && output.empty())
    {
        fmt::print(stderr, "error: --output is required unless --dry-run is given\n");
        return 2;
    }

    try
    {
        const fs::path file = Tools::utf8_path(package_path);
        if (!dry_run && fs::exists(Tools::utf8_path(output)) &&
            fs::equivalent(file, Tools::utf8_path(output)))
        {
            fmt::print(stderr, "error: the output must not overwrite the input package\n");
            return 2;
        }
        LoadOptions options;
        if (!profile.empty()) options.profile = profile;
        Linker linker(options);
        if (search_paths.empty())
        {
            fs::path dir = fs::absolute(file).parent_path();
            linker.add_search_path(dir);
            for (int up = 0; up < 4 && !linker.find_package_file("Core") && dir.has_parent_path() &&
                             dir.parent_path() != dir;
                 ++up)
            {
                dir = dir.parent_path();
                if (fs::exists(dir / "Core.u")) linker.add_search_path(dir);
            }
            if (!linker.find_package_file("Core"))
            {
                fmt::print(stderr, "warning: Core.u not found; pass --search-path <CookedPC> so property types resolve\n");
            }
        }
        for (const auto& dir : search_paths) linker.add_search_path(Tools::utf8_path(dir));
        const Package& package = linker.load(file);
        ReflectionSchema schema(linker);
        for (const char* script : {"Core", "Engine", "GameFramework", "TdGame"})
        {
            if (const Package* p = linker.find_package(script)) schema.add_package(*p);
        }

        std::ifstream in(Tools::utf8_path(t3d_path), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        const T3D::T3DDocument document = T3D::parse(text);

        PackageModel model = PackageModel::from_package(package);
        T3D::ImportOptions import_options;
        import_options.schema = &schema;
        Diagnostics diagnostics;
        const T3D::ImportResult result =
            T3D::import_document(model, linker, package, document, import_options, &diagnostics);
        for (const Diagnostic& d : diagnostics.entries())
        {
            fmt::print(stderr, "{}: {}: {}\n", to_string(d.severity), d.code, d.message);
        }
        fmt::print("{} objects matched, {} modified, {} brushes modified, {} skipped\n", result.objects,
                   result.modified, result.brushes_modified, result.skipped);
        if (dry_run) return result.skipped == 0 ? 0 : 1;

        WriteOptions write_options;
        if (compression == "zlib") write_options.compression = CompressionMethod::Zlib;
        else if (compression == "lzo") write_options.compression = CompressionMethod::Lzo;
        save_package(model, Tools::utf8_path(output), write_options);
        fmt::print("Wrote {}\n", output);
        return result.skipped == 0 ? 0 : 1;
    }
    catch (const Error& e)
    {
        fmt::print(stderr, "error: {}\n", e.what());
        return 1;
    }
}
