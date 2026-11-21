// UnrealPackageValidator — loads packages and reports structural problems.
//
// Exit codes: 0 = all packages valid, 1 = validation errors (or warnings with --strict), 2 = a package failed to load,
// 3 = invalid command line.

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <tl/expected.hpp>

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

    struct Validation
    {
        std::string profile;
        UnrealLib::Diagnostics diagnostics;
    };

    tl::expected<Validation, std::string> validate(const fs::path& file, const UnrealLib::LoadOptions& options)
    {
        try
        {
            const auto package = UnrealLib::Package::open(file, options);
            Validation result{package.profile().id, package.diagnostics()};
            result.diagnostics.append(UnrealLib::validate_package(package));
            return result;
        }
        catch (const UnrealLib::Error& error)
        {
            return tl::make_unexpected(std::string(error.what()));
        }
    }

    using JsonWriter = rapidjson::PrettyWriter<rapidjson::StringBuffer>;

    void write_string(JsonWriter& writer, const char* key, std::string_view value)
    {
        writer.Key(key);
        writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
    }

    void write_diagnostics(JsonWriter& writer, const UnrealLib::Diagnostics& diagnostics)
    {
        writer.Key("diagnostics");
        writer.StartArray();
        for (const auto& diagnostic : diagnostics.entries())
        {
            writer.StartObject();
            write_string(writer, "severity", to_string(diagnostic.severity));
            write_string(writer, "code", diagnostic.code);
            write_string(writer, "message", diagnostic.message);
            if (diagnostic.offset)
            {
                writer.Key("offset");
                writer.Uint64(*diagnostic.offset);
            }
            writer.EndObject();
        }
        writer.EndArray();
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
    rapidjson::StringBuffer report;
    JsonWriter writer(report);
    if (json)
    {
        writer.SetIndent(' ', 2);
        writer.StartObject();
        writer.Key("packages");
        writer.StartArray();
    }

    for (const auto& file : files)
    {
        const auto utf8 = file.u8string();
        const std::string_view display(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        if (json)
        {
            writer.StartObject();
            write_string(writer, "file", display);
        }

        if (const auto result = validate(file, options))
        {
            const auto& [profile_id, diagnostics] = *result;
            const bool ok = !diagnostics.has_errors() &&
                            !(strict && diagnostics.count(UnrealLib::Severity::Warning) > 0);
            ok ? ++valid : ++invalid;
            if (json)
            {
                write_string(writer, "status", ok ? "valid" : "invalid");
                write_string(writer, "profile", profile_id);
                write_diagnostics(writer, diagnostics);
            }
            else if (!quiet || !ok)
            {
                fmt::print("{}: {} ({} errors, {} warnings, profile {})\n", display, ok ? "OK" : "INVALID",
                           diagnostics.count(UnrealLib::Severity::Error),
                           diagnostics.count(UnrealLib::Severity::Warning), profile_id);
                for (const auto& d : diagnostics.entries())
                {
                    if (d.severity != UnrealLib::Severity::Info || !quiet)
                    {
                        fmt::print("  {}: [{}] {}\n", to_string(d.severity), d.code, d.message);
                    }
                }
            }
        }
        else
        {
            ++failed;
            if (json)
            {
                write_string(writer, "status", "load-failed");
                write_string(writer, "error", result.error());
            }
            else
            {
                fmt::print("{}: LOAD FAILED: {}\n", display, result.error());
            }
        }
        if (json) writer.EndObject();
    }

    if (json)
    {
        writer.EndArray();
        writer.Key("totals");
        writer.StartObject();
        writer.Key("valid");
        writer.Int(valid);
        writer.Key("invalid");
        writer.Int(invalid);
        writer.Key("load_failed");
        writer.Int(failed);
        writer.EndObject();
        writer.EndObject();
        std::cout << report.GetString() << '\n';
    }
    else
    {
        fmt::print("\n{} package(s): {} valid, {} invalid, {} failed to load\n", files.size(), valid, invalid, failed);
    }
    return failed > 0 ? 2 : (invalid > 0 ? 1 : 0);
}
