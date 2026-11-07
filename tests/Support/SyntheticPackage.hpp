#pragma once

// Test support: builds small, fully valid UE3 packages in memory using UnrealLib's own writers.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <UnrealLib/Package/ObjectTables.hpp>
#include <UnrealLib/Package/PackageSummary.hpp>
#include <UnrealLib/Serialization/Compression.hpp>

namespace UnrealLib::Testing
{
    struct SyntheticExport
    {
        ExportEntry entry;               // serial_offset/serial_size are filled in by the builder
        std::vector<std::byte> payload;  // export data
        std::vector<ObjectIndex> depends;
    };

    struct SyntheticPackageSpec
    {
        std::uint16_t file_version = 536;
        std::uint16_t licensee_version = 43;
        std::int32_t engine_version = 3716;
        std::int32_t cooker_version = 60;
        std::uint32_t package_flags = PackageFlag::Cooked;
        std::vector<std::string> names;
        std::vector<ImportEntry> imports;
        std::vector<SyntheticExport> exports;

        // Compression of the package body (everything after the summary).
        CompressionMethod compression = CompressionMethod::None;
        std::int32_t block_size = 0x20000; // uncompressed bytes per block
    };

    struct SyntheticPackage
    {
        std::vector<std::byte> file;         // bytes as they would be on disk
        std::vector<std::byte> uncompressed; // decompressed image (== file when not compressed)
        PackageSummary summary;              // summary as written to the file
    };

    // A small package: names None/Core/Class/Package/Object/TestPkg/MyObject, two imports
    // (Core [Package], Core.Object [Class]) and three exports with payloads and depends.
    [[nodiscard]] SyntheticPackageSpec make_basic_spec();

    [[nodiscard]] SyntheticPackage build_synthetic_package(const SyntheticPackageSpec& spec);

    // Returns a reference to `text` in spec.names, appending it if needed.
    FName intern(SyntheticPackageSpec& spec, const std::string& text, std::int32_t number = 0);

    // Appends an import and returns its index.
    ObjectIndex add_import(SyntheticPackageSpec& spec, const std::string& class_package, const std::string& class_name,
                           ObjectIndex outer, const std::string& name);

    // Appends an export and returns its index.
    ObjectIndex add_export(SyntheticPackageSpec& spec, ObjectIndex class_index, ObjectIndex super_index,
                           ObjectIndex outer, const std::string& name, std::uint64_t flags = 0,
                           std::vector<std::byte> payload = {});

    [[nodiscard]] inline std::vector<std::byte> bytes_of(std::initializer_list<int> values)
    {
        std::vector<std::byte> out;
        out.reserve(values.size());
        for (const int v : values)
        {
            out.push_back(static_cast<std::byte>(v));
        }
        return out;
    }
}
