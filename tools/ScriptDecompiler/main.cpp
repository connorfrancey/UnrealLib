// UnrealScriptDecompiler — decompiles the UnrealScript classes of a package to .uc files.
//
// The native function table and the type information for default properties come from the script packages (*.u)
// of the package's own directory and of every --search-path directory.
//
// Exit codes: 0 = success, 1 = some classes decompiled with placeholders (or failed), 2 = the package failed to load,
// 3 = invalid command line.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <UnrealLib/UnrealLib.hpp>

#include "../Common/PackageJson.hpp"

namespace fs = std::filesystem;

namespace
{
    using namespace UnrealLib;

    /// Loads the script packages (*.u) directly inside @p directory.
    void load_scripts(Linker& linker, const fs::path& directory, std::vector<const Package*>& out)
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(directory, ec))
        {
            if (!e.is_regular_file() || Text::to_lower_ascii(e.path().extension().string()) != ".u")
            {
                continue;
            }
            try
            {
                const Package& p = linker.load(e.path());
                if (std::find(out.begin(), out.end(), &p) == out.end()) out.push_back(&p);
            }
            catch (const Error& error)
            {
                fmt::print(stderr, "warning: {}: {}\n", e.path().filename().string(), error.what());
            }
        }
    }
}

int main(int argc, char** argv)
{
    CLI::App app{"Decompile the UnrealScript classes of an Unreal Engine 3 package (UnrealLib)",
                 "UnrealScriptDecompiler"};
    argv = app.ensure_utf8(argv);

    std::string path;
    std::string output;
    std::string profile;
    std::vector<std::string> search_paths;
    std::vector<std::string> only;
    bool to_stdout = false;
    bool quiet = false;

    app.add_option("package", path, "Script package (.u)")->required()->check(CLI::ExistingFile);
    app.add_option("-o,--output", output, "Output directory (one <Package>/Classes/<Class>.uc per class)");
    app.add_option("--profile", profile, "Game profile id or name (default: auto-detect)");
    app.add_option("-s,--search-path", search_paths, "Additional directories with packages to resolve imports")
        ->check(CLI::ExistingDirectory);
    app.add_option("-c,--class", only, "Only decompile these classes (repeatable)");
    app.add_flag("--stdout", to_stdout, "Print the sources instead of writing files");
    app.add_flag("-q,--quiet", quiet, "Only print problems and the totals");
    CLI11_PARSE(app, argc, argv);

    if (output.empty() && !to_stdout)
    {
        fmt::print(stderr, "error: give --output <directory> or --stdout\n");
        return 3;
    }

    try
    {
        LoadOptions options;
        if (!profile.empty())
        {
            options.profile = profile;
        }
        const fs::path file = Tools::utf8_path(path);
        Linker linker(options);
        linker.add_search_path(fs::absolute(file).parent_path());
        for (const auto& dir : search_paths)
        {
            linker.add_search_path(Tools::utf8_path(dir));
        }
        const Package& package = linker.load(file);

        std::vector<const Package*> scripts{&package};
        load_scripts(linker, fs::absolute(file).parent_path(), scripts);
        for (const auto& dir : search_paths)
        {
            load_scripts(linker, Tools::utf8_path(dir), scripts);
        }

        ReflectionSchema schema(linker);
        for (const Package* p : scripts) schema.add_package(*p);
        Diagnostics table_diagnostics;
        Decompiler::ScriptDecompiler decompiler(linker,
                                                Decompiler::NativeTable::from_packages(linker, scripts,
                                                                                       &table_diagnostics));
        decompiler.set_schema(&schema);

        std::size_t classes = 0, unclean = 0, failed = 0;
        const fs::path directory = Tools::utf8_path(output) / package.name() / "Classes";
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ObjectRef ref{&package, ObjectIndex::from_export(i)};
            if (package.class_name(ref.index) != "Class" || package.exports()[i].serial_size <= 0)
            {
                continue;
            }
            const std::string name = package.object_name(ref.index);
            if (!only.empty() && std::none_of(only.begin(), only.end(),
                                              [&](const std::string& c) { return Text::iequals(c, name); }))
            {
                continue;
            }
            ++classes;
            try
            {
                const Decompiler::DecompiledScript result = decompiler.decompile_class(ref);
                const bool clean = result.unknown_natives == 0 && result.placeholders == 0;
                unclean += clean ? 0 : 1;
                if (to_stdout)
                {
                    std::cout << result.source << '\n';
                }
                else
                {
                    fs::create_directories(directory);
                    std::ofstream(directory / (name + ".uc"), std::ios::binary) << result.source;
                }
                if (!quiet && !to_stdout)
                {
                    fmt::print("{}{}\n", name, clean ? "" : fmt::format("  ({} unknown natives, {} placeholders)",
                                                                        result.unknown_natives, result.placeholders));
                }
                for (const auto& d : result.diagnostics.entries())
                {
                    if (d.severity != Severity::Info && !quiet)
                    {
                        fmt::print(stderr, "  {}: [{}] {}\n", to_string(d.severity), d.code, d.message);
                    }
                }
            }
            catch (const Error& error)
            {
                ++failed;
                fmt::print(stderr, "error: {}: {}\n", name, error.what());
            }
        }
        fmt::print(stderr, "{} classes, {} with placeholders, {} failed\n", classes, unclean, failed);
        return unclean + failed == 0 ? 0 : 1;
    }
    catch (const Error& e)
    {
        fmt::print(stderr, "error ({}): {}\n", to_string(e.code()), e.what());
        return 2;
    }
}
