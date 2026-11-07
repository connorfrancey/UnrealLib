#pragma once

/// @file
/// @brief Package loading: summary, decompression, name/import/export/depends tables.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <UnrealLib/Core/Diagnostics.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Package/ObjectTables.hpp>
#include <UnrealLib/Package/PackageSummary.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/SerializationLimits.hpp>

namespace UnrealLib
{
    /// @brief Options controlling Package::load().
    struct LoadOptions
    {
        /// Profile id or display name (e.g. "mirrorsedge-pc" or "Mirror's Edge PC"). Empty: auto-detect.
        std::optional<std::string> profile;

        /// Registry used for profile lookup/detection; nullptr selects GameProfileRegistry::builtin().
        const GameProfileRegistry* registry = nullptr;

        /// Allocation and size limits applied to every read.
        SerializationLimits limits{};

        /// Whether to read the depends table (version >= 415).
        bool read_depends = true;
    };

    /// @brief A loaded Unreal package (header-level view).
    ///
    /// Loading performs, in order: summary parsing, profile selection, decompression of compressed chunks, and
    /// parsing of the name, import, export and depends tables. Object construction / deserialization (milestone M3)
    /// is not implemented yet; export payloads are available as raw bytes through export_data().
    ///
    /// A Package is move-only. All accessors other than is_loaded() throw Error (InvalidState) before a successful
    /// load.
    class Package
    {
    public:
        Package() = default;
        Package(Package&&) noexcept = default;            ///< Move-constructs (the source becomes unspecified).
        Package& operator=(Package&&) noexcept = default; ///< Move-assigns.
        Package(const Package&) = delete;
        Package& operator=(const Package&) = delete;

        /// @brief Convenience: constructs a package and loads @p path.
        [[nodiscard]] static Package open(const std::filesystem::path& path, const LoadOptions& options = {});

        /// @brief Loads a package file. On failure the package is left unloaded and the exception propagates.
        void load(const std::filesystem::path& path, const LoadOptions& options = {});

        /// @brief Loads a package from an in-memory copy of the file.
        /// @param bytes Complete package file contents.
        /// @param name  Package name (normally the file stem, e.g. "Core").
        /// @param options Profile selection, limits and table options.
        void load_from_memory(std::vector<std::byte> bytes, std::string name, const LoadOptions& options = {});

        /// @brief Whether a package has been loaded successfully.
        [[nodiscard]] bool is_loaded() const noexcept { return m_loaded; }

        /// @brief Package name (file stem).
        [[nodiscard]] const std::string& name() const;

        /// @brief The file summary as read from disk.
        [[nodiscard]] const PackageSummary& summary() const;

        /// @brief The selected game profile.
        [[nodiscard]] const GameProfile& profile() const;

        /// @brief Why the profile was chosen (detection reasons, or a note that it was selected explicitly).
        [[nodiscard]] std::span<const std::string> profile_reasons() const;

        /// @brief The name table.
        [[nodiscard]] const NameTable& names() const;

        /// @brief The import table.
        [[nodiscard]] std::span<const ImportEntry> imports() const;

        /// @brief The export table.
        [[nodiscard]] std::span<const ExportEntry> exports() const;

        /// @brief The depends table (one list of object indices per export; empty if absent or not read).
        [[nodiscard]] const std::vector<std::vector<ObjectIndex>>& depends() const;

        /// @brief Size of the file on disk.
        [[nodiscard]] std::uint64_t file_size() const;

        /// @brief The decompressed package image (identical to the file for uncompressed packages).
        [[nodiscard]] std::span<const std::byte> data() const;

        /// @brief Non-fatal findings produced while loading.
        [[nodiscard]] const Diagnostics& diagnostics() const;

        /// @brief Serialization context for this package (versions, byte order, configured limits).
        [[nodiscard]] SerializationContext context() const;

        /// @brief A reader over data() configured with context().
        [[nodiscard]] BinaryReader reader() const;

        /// @name Object references
        /// @{

        /// @brief Whether @p index is null or refers to an existing import/export.
        /// @param index Object index to check.
        [[nodiscard]] bool is_valid(ObjectIndex index) const;

        /// @brief Object name of @p index ("None" for null). Throws Error (OutOfBounds) for invalid indices.
        [[nodiscard]] std::string object_name(ObjectIndex index) const;

        /// @brief Class name of @p index: the import's class, the export's class object name, or "Class" for exports
        ///        with a null class. Throws Error (OutOfBounds) for invalid indices.
        [[nodiscard]] std::string class_name(ObjectIndex index) const;

        /// @brief Outer of @p index (null for top-level objects).
        [[nodiscard]] ObjectIndex outer(ObjectIndex index) const;

        /// @brief Dotted path of @p index from its outermost object, e.g. "Engine.Actor.Tick".
        ///
        /// Cycles in the outer chain are detected; the path is then truncated and prefixed with "<cycle>".
        [[nodiscard]] std::string object_path(ObjectIndex index) const;

        /// @brief Finds the first export whose path (preferred) or name matches @p name_or_path case-insensitively.
        [[nodiscard]] std::optional<ObjectIndex> find_export(std::string_view name_or_path) const;

        /// @brief Finds the first import whose path (preferred) or name matches @p name_or_path case-insensitively.
        [[nodiscard]] std::optional<ObjectIndex> find_import(std::string_view name_or_path) const;

        /// @brief The serialized bytes of export @p export_index (bounds-checked against data()).
        [[nodiscard]] std::span<const std::byte> export_data(std::size_t export_index) const;
        /// @}

    private:
        void require_loaded() const;
        void parse_tables(const LoadOptions& options);
        [[nodiscard]] const FName& object_fname(ObjectIndex index) const;

        bool m_loaded = false;
        std::string m_name;
        PackageSummary m_summary;
        GameProfile m_profile;
        std::vector<std::string> m_profile_reasons;
        SerializationLimits m_limits;
        std::uint64_t m_file_size = 0;
        std::vector<std::byte> m_data;
        NameTable m_names;
        std::vector<ImportEntry> m_imports;
        std::vector<ExportEntry> m_exports;
        std::vector<std::vector<ObjectIndex>> m_depends;
        Diagnostics m_diagnostics;
    };
}
