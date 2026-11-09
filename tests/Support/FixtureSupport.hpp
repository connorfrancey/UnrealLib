#pragma once

// Shared helpers for tests that read the Mirror's Edge fixture corpus (UNREALLIB_MIRRORSEDGE_DIR).

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib::TestSupport
{
    /// Value of environment variable @p name (empty if unset).
    inline std::string env(const char* name)
    {
#ifdef _MSC_VER
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        {
            return {};
        }
        std::string result(value);
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
#endif
    }

    /// The CookedPC directory from UNREALLIB_MIRRORSEDGE_DIR.
    inline std::filesystem::path fixture_dir()
    {
        const std::string value = env("UNREALLIB_MIRRORSEDGE_DIR");
        return std::filesystem::path(std::u8string(value.begin(), value.end()));
    }

    /// Whether the fixture directory is configured and exists.
    inline bool fixtures_available()
    {
        return !fixture_dir().empty() && std::filesystem::is_directory(fixture_dir());
    }

    /// Script packages (*.u in the top directory) or, with @p recursive, every package file of the corpus.
    inline std::vector<std::filesystem::path> packages(bool recursive)
    {
        namespace fs = std::filesystem;
        std::vector<fs::path> out;
        auto consider = [&](const fs::directory_entry& e) {
            const std::string ext = Text::to_lower_ascii(e.path().extension().string());
            if (e.is_regular_file() && (recursive ? (ext == ".u" || ext == ".upk" || ext == ".me1") : ext == ".u"))
            {
                out.push_back(e.path());
            }
        };
        if (recursive)
        {
            for (const auto& e : fs::recursive_directory_iterator(fixture_dir())) consider(e);
        }
        else
        {
            for (const auto& e : fs::directory_iterator(fixture_dir())) consider(e);
        }
        return out;
    }

    /// Loads every script package (*.u in the top directory) into @p linker.
    inline std::vector<const Package*> load_script_packages(Linker& linker)
    {
        std::vector<const Package*> out;
        for (const auto& file : packages(false))
        {
            const Package* p = linker.find_package(file.stem().string());
            if (p != nullptr) out.push_back(p);
        }
        return out;
    }
}
