#pragma once

/// @file
/// @brief Package writing: an editable in-memory package model and its serializer.
///
/// A PackageModel holds everything a package file contains (summary, names, imports, exports, depends, export data)
/// in editable form. write_package() lays the file out again from scratch — summary, name table, import table,
/// export table, depends table, preserved header tail, export data — and recomputes every count, offset and size.
/// An unmodified model of an uncompressed package is written back byte-identically (verified on every uncompressed
/// Mirror's Edge package).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Package/ObjectTables.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Package/PackageSummary.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/Compression.hpp>

namespace UnrealLib
{
    /// @brief Editable contents of a package.
    ///
    /// Indices follow the usual convention: ObjectIndex::from_export(i) is `exports[i]`, from_import(i) is
    /// `imports[i]`. Export payloads are opaque bytes; edit them with the property helpers or replace them whole.
    struct PackageModel
    {
        std::string name;                        ///< Package name (file stem).
        PackageSummary summary;                  ///< Summary; counts, offsets and compression are recomputed.
        std::vector<NameEntry> names;            ///< Name table.
        std::vector<ImportEntry> imports;        ///< Import table.
        std::vector<ExportEntry> exports;        ///< Export table; serial size/offset are recomputed.
        std::vector<std::vector<ObjectIndex>> depends; ///< One list per export (file version >= 415).
        std::vector<std::vector<std::byte>> export_data; ///< One payload per export.
        /// Bytes between the depends table and the first export's data, kept verbatim (empty for Mirror's Edge;
        /// thumbnail/GUID tables of later versions would live here and are not re-laid out).
        std::vector<std::byte> header_tail;

        /// @brief Copies a loaded package. Compressed packages yield their decompressed contents.
        /// @throws Error (InvalidArgument) if the package's depends table was not loaded (LoadOptions::read_depends).
        [[nodiscard]] static PackageModel from_package(const Package& package);

        /// @brief Index of name @p text, adding it if missing (case-insensitive match, like Unreal).
        std::int32_t add_name(std::string_view text, std::uint64_t flags = 0x0007001000000000ull);

        /// @brief An FName for @p text (adds the base name; `Name_N` suffixes become the instance number).
        [[nodiscard]] FName make_name(std::string_view text);

        /// @brief Display text of @p name ("Base" or "Base_<number-1>").
        [[nodiscard]] std::string name_text(const FName& name) const;

        /// @brief Adds an import; returns its index.
        ObjectIndex add_import(std::string_view class_package, std::string_view class_name, ObjectIndex outer,
                               std::string_view object_name);

        /// @brief Finds an import by class and outer/name (case-insensitive), if present.
        [[nodiscard]] std::optional<ObjectIndex> find_import(std::string_view class_name, ObjectIndex outer,
                                                             std::string_view object_name) const;

        /// @brief Adds an export with payload @p data (and an empty depends list); returns its index.
        ObjectIndex add_export(ExportEntry entry, std::vector<std::byte> data);

        /// @brief Finds an export by path ("Outer.Name", case-insensitive).
        [[nodiscard]] std::optional<ObjectIndex> find_export(std::string_view path) const;

        /// @brief Path of export or import @p index ("Outer.Name").
        [[nodiscard]] std::string object_path(ObjectIndex index) const;

        /// @brief Renames export or import @p index (adds the name if needed).
        void rename(ObjectIndex index, std::string_view new_name);

        /// @brief Checks internal consistency (index ranges, one payload and depends list per export).
        /// @throws Error (InvalidState) describing the first problem.
        void validate() const;
    };

    /// @brief Output options.
    struct WriteOptions
    {
        /// Compression of the package body (everything after the summary). None writes an uncompressed package.
        CompressionMethod compression = CompressionMethod::None;
        /// Uncompressed bytes per compressed chunk (UE3 cooked packages: 1 MiB) and per block inside a chunk.
        std::int32_t chunk_size = 0x100000;
        std::int32_t block_size = 0x20000; ///< See chunk_size.
    };

    /// @brief Serializes @p model as a package file.
    /// @throws Error (InvalidState) if the model is inconsistent; UnsupportedError for unsupported compression.
    [[nodiscard]] std::vector<std::byte> write_package(const PackageModel& model, const WriteOptions& options = {});

    /// @brief Writes @p model to @p path (see write_package()). @throws IoError on failure.
    void save_package(const PackageModel& model, const std::filesystem::path& path, const WriteOptions& options = {});

    /// @brief Replaces the tagged properties of export @p index, keeping the bytes before them (the UObject header)
    ///        and after them (native data) unchanged.
    /// @param model             The package being edited.
    /// @param index             The export whose properties are replaced.
    /// @param properties_offset Offset of the first tag in the export data (ObjectProperties::header.properties_offset).
    /// @param properties_end    Offset just past the "None" tag (ObjectProperties::end_offset).
    /// @param properties        The new property list (written with write_properties()).
    /// @throws Error (InvalidArgument) if the offsets are out of range.
    void replace_properties(PackageModel& model, ObjectIndex index, std::size_t properties_offset,
                            std::size_t properties_end, const std::vector<Property>& properties);
}
