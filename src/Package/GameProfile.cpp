#include <UnrealLib/Package/GameProfile.hpp>

#include <algorithm>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/PackageSummary.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    std::string_view to_string(EngineGeneration generation) noexcept
    {
        switch (generation)
        {
        case EngineGeneration::UE3: return "UE3";
        }
        return "Unknown";
    }

    std::string_view to_string(Platform platform) noexcept
    {
        switch (platform)
        {
        case Platform::PC: return "PC";
        case Platform::Xbox360: return "Xbox360";
        case Platform::PS3: return "PS3";
        case Platform::Unknown: break;
        }
        return "Unknown";
    }

    GameProfile make_mirrors_edge_pc_profile()
    {
        GameProfile p;
        p.id = "mirrorsedge-pc";
        p.name = "Mirror's Edge PC";
        p.generation = EngineGeneration::UE3;
        p.platform = Platform::PC;
        p.min_file_version = 536;
        p.max_file_version = 536;
        p.licensee_version = std::uint16_t{43};
        p.engine_version = 3716;
        p.cooker_versions = {60, 0};
        p.endian = Endian::Little;
        p.compression_methods = {CompressionMethod::None, CompressionMethod::Lzo};
        // Upstream DefaultEngineBranch.BuildTokenMap: `case MirrorsEdge: tokenMap[0x4F] = typeof(UnresolvedToken)`.
        p.bytecode_overrides = {{std::uint8_t{0x4F}, "Unresolved"}};
        p.verified = true;
        p.notes =
            "Upstream UELib: GameBuild [Build(536, 43)] MirrorsEdge. Verified against the Steam release: 2425/2426 "
            "packages are 536/43, engine 3716, cooker 60 (0 for editor-saved packages), compression none or LZO.";
        return p;
    }

    GameProfile make_generic_ue3_profile()
    {
        GameProfile p;
        p.id = "ue3-generic";
        p.name = "Generic UE3";
        p.generation = EngineGeneration::UE3;
        p.platform = Platform::Unknown;
        p.min_file_version = min_supported_file_version;
        p.max_file_version = static_cast<std::uint16_t>(first_unsupported_file_version - 1);
        p.endian = Endian::Little;
        p.compression_methods = {CompressionMethod::None, CompressionMethod::Zlib, CompressionMethod::Lzo};
        p.verified = false;
        p.notes = "Fallback using the upstream UELib default (non game-specific) serialization path.";
        return p;
    }

    const GameProfileRegistry& GameProfileRegistry::builtin()
    {
        static const GameProfileRegistry registry = [] {
            GameProfileRegistry r;
            r.add(make_mirrors_edge_pc_profile());
            r.add(make_generic_ue3_profile());
            return r;
        }();
        return registry;
    }

    void GameProfileRegistry::add(GameProfile profile)
    {
        if (find(profile.id) != nullptr)
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("game profile '{}' is already registered", profile.id));
        }
        m_profiles.push_back(std::move(profile));
    }

    const GameProfile* GameProfileRegistry::find(std::string_view id_or_name) const noexcept
    {
        const auto it = std::find_if(m_profiles.begin(), m_profiles.end(), [&](const GameProfile& p) {
            return Text::iequals(p.id, id_or_name) || Text::iequals(p.name, id_or_name);
        });
        return it == m_profiles.end() ? nullptr : &*it;
    }

    ProfileDetection GameProfileRegistry::detect(const PackageSummary& summary) const
    {
        ProfileDetection best;
        for (const GameProfile& profile : m_profiles)
        {
            ProfileDetection candidate;
            candidate.profile = &profile;

            if (summary.file_version < profile.min_file_version || summary.file_version > profile.max_file_version)
            {
                continue;
            }
            if (summary.endian != profile.endian)
            {
                continue;
            }
            candidate.score += 1;
            candidate.reasons.push_back(fmt::format("{}: file version {} in [{}, {}]", profile.id,
                                                    summary.file_version, profile.min_file_version,
                                                    profile.max_file_version));

            if (profile.licensee_version)
            {
                if (summary.licensee_version != *profile.licensee_version)
                {
                    continue;
                }
                candidate.score += 10;
                candidate.reasons.push_back(fmt::format("{}: licensee version {}", profile.id,
                                                        summary.licensee_version));
            }
            if (profile.engine_version && summary.engine_version == *profile.engine_version)
            {
                candidate.score += 5;
                candidate.reasons.push_back(fmt::format("{}: engine version {}", profile.id, summary.engine_version));
            }
            if (std::find(profile.cooker_versions.begin(), profile.cooker_versions.end(), summary.cooker_version) !=
                profile.cooker_versions.end())
            {
                candidate.score += 1;
                candidate.reasons.push_back(fmt::format("{}: cooker version {}", profile.id, summary.cooker_version));
            }

            if (candidate.score > best.score)
            {
                best = std::move(candidate);
            }
        }
        return best;
    }
}
