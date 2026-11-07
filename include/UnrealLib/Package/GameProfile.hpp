#pragma once

/// @file
/// @brief Game profiles: everything that is specific to one game/platform build lives here, not in serializers.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <UnrealLib/Serialization/ByteOrder.hpp>
#include <UnrealLib/Serialization/Compression.hpp>

namespace UnrealLib
{
    struct PackageSummary;

    /// @brief Engine generation.
    enum class EngineGeneration : std::uint8_t
    {
        UE3, ///< Unreal Engine 3.
    };

    /// @brief Target platform of a package.
    enum class Platform : std::uint8_t
    {
        Unknown,
        PC,
        Xbox360,
        PS3,
    };

    /// @brief Returns "UE3".
    [[nodiscard]] std::string_view to_string(EngineGeneration generation) noexcept;
    /// @brief Returns "PC", "Xbox360", "PS3" or "Unknown".
    [[nodiscard]] std::string_view to_string(Platform platform) noexcept;

    /// @brief Licensee changes to asset (native object) serialization.
    struct AssetRules
    {
        /// StaticMesh ends with one extra float after ThumbnailDistance (Mirror's Edge; meaning unknown).
        bool static_mesh_trailing_float = false;
        /// Number of UV sets in skeletal mesh GPU vertices when the version would imply 1 (Mirror's Edge: 3,
        /// licensee >= 13; UE Viewer). 0 = engine default.
        std::uint8_t skeletal_uv_sets = 0;
    };

    /// @brief Describes one game build and the facts used to recognise its packages.
    ///
    /// Version numbers alone do not identify a game; detection also weighs licensee version, engine build, cooker
    /// version and byte order. Rules for higher layers (properties, bytecode, assets) will be added to the profile as
    /// those subsystems are implemented.
    struct GameProfile
    {
        std::string id;                     ///< Stable identifier, e.g. "mirrorsedge-pc".
        std::string name;                   ///< Display name, e.g. "Mirror's Edge PC".
        EngineGeneration generation = EngineGeneration::UE3; ///< Engine generation.
        Platform platform = Platform::Unknown;               ///< Platform.
        std::uint16_t min_file_version = 0; ///< Lowest accepted package file version.
        std::uint16_t max_file_version = 0; ///< Highest accepted package file version.
        std::optional<std::uint16_t> licensee_version; ///< Required licensee version, if any.
        std::optional<std::int32_t> engine_version;    ///< Expected engine build (summary EngineVersion), if known.
        std::vector<std::int32_t> cooker_versions;     ///< Cooker versions seen in this game's packages.
        Endian endian = Endian::Little;                ///< Byte order of the game's packages.
        std::vector<CompressionMethod> compression_methods; ///< Compression methods the game uses.
        /// Bytecode opcode reassignments relative to the version's default UE3 token map, as (opcode, token name)
        /// pairs; token names are those of `UnrealLib::to_string(TokenKind)` (e.g. {0x4F, "Unresolved"}).
        std::vector<std::pair<std::uint8_t, std::string>> bytecode_overrides;
        AssetRules assets;     ///< Asset serialization differences.
        bool verified = false; ///< Whether the profile has been verified against real game packages.
        std::string notes;     ///< Provenance of the values.
    };

    /// @brief Result of GameProfileRegistry::detect().
    struct ProfileDetection
    {
        const GameProfile* profile = nullptr; ///< Best match, or nullptr if no profile accepts the package.
        int score = 0;                        ///< Match score (higher is more specific).
        std::vector<std::string> reasons;     ///< Human-readable explanation of the score.
    };

    /// @brief A set of game profiles with lookup and detection.
    class GameProfileRegistry
    {
    public:
        /// @brief The built-in profiles: "mirrorsedge-pc" (verified) and "ue3-generic" (fallback, unverified).
        [[nodiscard]] static const GameProfileRegistry& builtin();

        /// @brief Adds a profile. Throws Error (InvalidArgument) if the id is already registered.
        void add(GameProfile profile);

        /// @brief Finds a profile by id or display name (case-insensitive).
        [[nodiscard]] const GameProfile* find(std::string_view id_or_name) const noexcept;

        /// @brief All profiles in registration order.
        [[nodiscard]] std::span<const GameProfile> profiles() const noexcept { return m_profiles; }

        /// @brief Scores every profile against @p summary and returns the best one.
        ///
        /// A profile is rejected when the file version is outside its range, when a required licensee version
        /// differs, or when the byte order differs. Among accepted profiles, matching licensee version, engine
        /// version and cooker version increase the score.
        [[nodiscard]] ProfileDetection detect(const PackageSummary& summary) const;

    private:
        std::vector<GameProfile> m_profiles;
    };

    /// @brief The built-in Mirror's Edge PC profile.
    [[nodiscard]] GameProfile make_mirrors_edge_pc_profile();

    /// @brief The built-in generic UE3 little-endian profile (fallback; unverified beyond Mirror's Edge).
    [[nodiscard]] GameProfile make_generic_ue3_profile();
}
