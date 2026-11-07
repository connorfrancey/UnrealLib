// UnrealPackageValidator — loads packages and reports structural problems.
//
// Exit codes: 0 = all packages valid, 1 = validation errors (or warnings with --strict), 2 = a package failed to load,
// 3 = invalid command line.

#include <filesystem>
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
    bool is_package_file(const fs::path& path)
    {
        const std::string ext = UnrealLib::Text::to_lower_ascii(path.extension().string());
        return ext == ".u" || ext == ".upk" || ext == ".umap" || ext == ".me1" || ext == ".ut3" || ext == ".udk";
    }

    std::vector<fs::path> collect(const std::vector<std::string>& inputs, bool recursive)
    {
        std::vector<fs::path> files;
        for (const auto& input : inputs)
        {
            const fs::path path = UnrealLib::Tools::utf8_path(input);
            if (fs::is_directory(path))
            {
                if (recursive)
                {
                    for (const auto& e : fs::recursive_directory_iterator(path))
                    {
                        if (e.is_regular_file() && is_package_file(e.path())) files.push_back(e.path());
                    }
                }
                else
                {
                    for (const auto& e : fs::directory_iterator(path))
                    {
                        if (e.is_regular_file() && is_package_file(e.path())) files.push_back(e.path());
                    }
                }
            }
            else
            {
                files.push_back(path);
            }
        }
        return files;
    }
}

int main(int argc, char** argv)
{
    CLI::App app{"Validate the structure of Unreal Engine 3 packages (UnrealLib)", "UnrealPackageValidator"};
    argv = app.ensure_utf8(argv);

    std::vector<std::string> inputs;
    std::string profile;
    bool recursive = false;
    bool strict = false;
    bool json = false;
    bool quiet = false;

    app.add_option("packages", inputs, "Package files or directories")->required();
    app.add_option("--profile", profile, "Game profile id or name (default: auto-detect)");
    app.add_flag("-r,--recursive", recursive, "Recurse into directories");
    app.add_flag("--strict", strict, "Treat warnings as failures");
    app.add_flag("--json", json, "Print a JSON report");
    app.add_flag("-q,--quiet", quiet, "Only print failing packages and the totals");
    try
    {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError& e)
    {
        const int code = app.exit(e);
        return code == 0 ? 0 : 3;
    }

    UnrealLib::LoadOptions options;
    if (!profile.empty())
    {
        options.profile = profile;
    }

    const auto files = collect(inputs, recursive);
    int valid = 0;
    int invalid = 0;
    int failed = 0;
    nlohmann::ordered_json report = nlohmann::ordered_json::array();

    for (const auto& file : files)
    {
        const std::string display = file.string();
        nlohmann::ordered_json entry;
        entry["file"] = display;
        try
        {
            const auto package = UnrealLib::Package::open(file, options);
            UnrealLib::Diagnostics diagnostics = package.diagnostics();
            diagnostics.append(UnrealLib::validate_package(package));

            const bool ok = !diagnostics.has_errors() &&
                            !(strict && diagnostics.count(UnrealLib::Severity::Warning) > 0);
            ok ? ++valid : ++invalid;
            entry["status"] = ok ? "valid" : "invalid";
            entry["profile"] = package.profile().id;
            entry["diagnostics"] = UnrealLib::Tools::diagnostics_to_json(diagnostics);

            if (!json && (!quiet || !ok))
            {
                fmt::print("{}: {} ({} errors, {} warnings, profile {})\n", display, ok ? "OK" : "INVALID",
                           diagnostics.count(UnrealLib::Severity::Error),
                           diagnostics.count(UnrealLib::Severity::Warning), package.profile().id);
                for (const auto& d : diagnostics.entries())
                {
                    if (d.severity != UnrealLib::Severity::Info || !quiet)
                    {
                        fmt::print("  {}: [{}] {}\n", to_string(d.severity), d.code, d.message);
                    }
                }
            }
        }
        catch (const UnrealLib::Error& e)
        {
            ++failed;
            entry["status"] = "load-failed";
            entry["error"] = e.what();
            if (!json)
            {
                fmt::print("{}: LOAD FAILED: {}\n", display, e.what());
            }
        }
        report.push_back(std::move(entry));
    }

    if (json)
    {
        nlohmann::ordered_json out;
        out["packages"] = std::move(report);
        out["totals"] = {{"valid", valid}, {"invalid", invalid}, {"load_failed", failed}};
        std::cout << out.dump(2) << '\n';
    }
    else
    {
        fmt::print("\n{} package(s): {} valid, {} invalid, {} failed to load\n", files.size(), valid, invalid, failed);
    }
    return failed > 0 ? 2 : (invalid > 0 ? 1 : 0);
}
