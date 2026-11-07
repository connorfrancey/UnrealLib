#pragma once

// Normalized JSON representation of a loaded package, shared by the tools and used for differential testing.
// The schema is documented in docs/package_format.md ("Normalized JSON").

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include <UnrealLib/Core/Diagnostics.hpp>
#include <UnrealLib/Package/Package.hpp>

namespace UnrealLib::Tools
{
    struct JsonOptions
    {
        bool names = true;
        bool imports = true;
        bool exports = true;
        bool depends = false;
    };

    [[nodiscard]] nlohmann::ordered_json summary_to_json(const Package& package);
    [[nodiscard]] nlohmann::ordered_json package_to_json(const Package& package, const JsonOptions& options);
    [[nodiscard]] nlohmann::ordered_json diagnostics_to_json(const Diagnostics& diagnostics);

    // Converts a UTF-8 command-line argument to a path.
    [[nodiscard]] inline std::filesystem::path utf8_path(const std::string& text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
}
