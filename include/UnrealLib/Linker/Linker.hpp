#pragma once

/// @file
/// @brief Cross-package linking: package lookup, import resolution, class hierarchy queries.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <UnrealLib/Core/Diagnostics.hpp>
#include <UnrealLib/Objects/ObjectHeader.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    /// @brief A reference to an object table entry of a specific package.
    struct ObjectRef
    {
        const Package* package = nullptr; ///< Owning package (nullptr = no object).
        ObjectIndex index;                ///< Export (> 0), import (< 0) or null (the package itself).

        /// @brief Whether this refers to something.
        [[nodiscard]] bool valid() const noexcept { return package != nullptr; }

        /// @brief Whether this refers to an export.
        [[nodiscard]] bool is_export() const noexcept { return package != nullptr && index.is_export(); }

        /// @brief The export entry. Precondition: is_export().
        [[nodiscard]] const ExportEntry& export_entry() const { return package->exports()[index.export_index()]; }

        /// @brief Memberwise comparison.
        friend bool operator==(const ObjectRef&, const ObjectRef&) = default;
    };

    /// @brief Outcome of resolving an object reference.
    enum class ResolveStatus : std::uint8_t
    {
        Null,            ///< The reference was null.
        Export,          ///< Resolved to an export (`Resolution::target`).
        Package,         ///< Resolved to a whole package (`target.index` is null).
        Intrinsic,       ///< A native-only class import (e.g. `Core.Class`, `Core.ArrayProperty`): such classes have no
                         ///< export in any package, so the import cannot and need not resolve further.
        PackageNotFound, ///< The import's package is not in the search path (or failed to load).
        ObjectNotFound,  ///< The package was found but contains no matching export.
        ClassMismatch,   ///< An export with the right path exists but its class differs from the import's class.
        Invalid,         ///< The index is out of range or the import's outer chain is malformed.
    };

    /// @brief Returns a stable name for @p status (e.g. "package-not-found").
    [[nodiscard]] std::string_view to_string(ResolveStatus status) noexcept;

    /// @brief Result of Linker::resolve().
    struct Resolution
    {
        ResolveStatus status = ResolveStatus::Null; ///< Outcome.
        ObjectRef target;                           ///< Export/package reached (Export, Package, ClassMismatch).
        std::string detail;                         ///< Explanation for unsuccessful outcomes.

        /// @brief Whether the reference is null or resolved to something definite (export, package, intrinsic).
        [[nodiscard]] bool ok() const noexcept
        {
            return status == ResolveStatus::Null || status == ResolveStatus::Export ||
                   status == ResolveStatus::Package || status == ResolveStatus::Intrinsic;
        }
    };

    /// @brief Resolves references across a set of packages.
    ///
    /// Packages are located through search paths (indexed by file stem, case-insensitively) and loaded lazily the
    /// first time an import refers to them. Loading never recurses into imports, so cyclic dependencies between
    /// packages (and cyclic object references) need no special handling. Resolutions are cached.
    ///
    /// Import resolution follows UE3 semantics: the outermost import names a package; each inner import names an
    /// export of that package whose outer is the previously matched export (top level: null outer), whose name
    /// matches (case-insensitively, including the instance number) and whose class matches the import's class.
    ///
    /// A Linker owns its packages; references (`const Package&`, ObjectRef) stay valid for its lifetime.
    /// Not thread-safe.
    class Linker
    {
    public:
        /// @param options Options used for every package the linker loads.
        explicit Linker(LoadOptions options = {});
        ~Linker();
        Linker(Linker&&) noexcept;            ///< Move-constructs.
        Linker& operator=(Linker&&) noexcept; ///< Move-assigns.
        Linker(const Linker&) = delete;
        Linker& operator=(const Linker&) = delete;

        /// @brief Indexes the package files in @p directory. Earlier search paths win on name clashes (reported as
        ///        warnings in diagnostics()).
        void add_search_path(const std::filesystem::path& directory, bool recursive = true);

        /// @brief Loads @p file (if not already loaded under the same name) and returns it.
        const Package& load(const std::filesystem::path& file);

        /// @brief Takes ownership of an already-loaded package. Throws Error (InvalidArgument) if a package with the
        ///        same name is already present.
        const Package& add(Package package);

        /// @brief Returns the package named @p name (case-insensitive), loading it from the search paths if needed.
        /// @return nullptr if no such package is known or it failed to load (see diagnostics()).
        [[nodiscard]] const Package* find_package(std::string_view name);

        /// @brief Number of packages currently loaded.
        [[nodiscard]] std::size_t loaded_count() const noexcept { return m_packages.size(); }

        /// @brief Number of package files known from search paths.
        [[nodiscard]] std::size_t indexed_count() const noexcept { return m_index.size(); }

        /// @brief Resolves @p index of @p package: exports resolve to themselves, imports to their defining export.
        [[nodiscard]] const Resolution& resolve(const Package& package, ObjectIndex index);

        /// @brief Convenience overload.
        [[nodiscard]] const Resolution& resolve(ObjectRef ref) { return resolve(*ref.package, ref.index); }

        /// @brief Fully qualified path: "Package.Outer.Name" for exports, the import path for imports.
        [[nodiscard]] std::string qualified_path(ObjectRef ref) const;

        /// @brief The class of export @p object, resolved. For class exports (null class index) the result has status
        ///        Null; for intrinsic classes, status Intrinsic.
        [[nodiscard]] const Resolution& class_of(ObjectRef object);

        /// @brief Whether the class of export @p object is, or derives from, the class @p class_path
        ///        (qualified, e.g. "Core.Component").
        /// @return std::nullopt when the class hierarchy cannot be determined (unresolved class or super).
        [[nodiscard]] std::optional<bool> is_a(ObjectRef object, std::string_view class_path);

        /// @brief Whether class export @p class_ref is, or derives from, @p class_path (walks the super chain).
        [[nodiscard]] std::optional<bool> class_is_a(ObjectRef class_ref, std::string_view class_path);

        /// @brief Computes the ObjectHeaderInput for export @p object.
        /// @return std::nullopt if the class hierarchy cannot be determined.
        [[nodiscard]] std::optional<ObjectHeaderInput> header_input(ObjectRef object);

        /// @brief Reads the UObject header of export @p object.
        /// @throws Error (NotFound) if the class hierarchy cannot be determined; SerializationError on bad data.
        [[nodiscard]] ObjectHeader read_header(ObjectRef object);

        /// @brief Load failures and search-path warnings.
        [[nodiscard]] const Diagnostics& diagnostics() const noexcept { return m_diagnostics; }

    private:
        struct PackageLookup; // per-package (outer, name) -> export index

        Resolution resolve_uncached(const Package& package, ObjectIndex index);
        Resolution resolve_import(const Package& package, std::size_t import_index);
        const PackageLookup& lookup_for(const Package& package);
        const Package& insert(std::unique_ptr<Package> package);

        LoadOptions m_options;
        std::vector<std::unique_ptr<Package>> m_packages;
        std::unordered_map<std::string, const Package*, Text::CaseInsensitiveHash, Text::CaseInsensitiveEqual> m_by_name;
        std::unordered_map<std::string, std::filesystem::path, Text::CaseInsensitiveHash, Text::CaseInsensitiveEqual>
            m_index;
        std::unordered_map<std::string, std::string, Text::CaseInsensitiveHash, Text::CaseInsensitiveEqual> m_failed;
        std::map<std::pair<const Package*, std::int32_t>, Resolution> m_cache;
        std::unordered_map<const Package*, std::unique_ptr<PackageLookup>> m_lookups;
        Diagnostics m_diagnostics;
    };
}
