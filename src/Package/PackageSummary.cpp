#include <UnrealLib/Package/PackageSummary.hpp>

#include <array>
#include <utility>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

namespace UnrealLib
{
    namespace
    {
        constexpr std::array<std::pair<std::uint32_t, const char*>, 16> k_flag_names{{
            {PackageFlag::AllowDownload, "AllowDownload"},
            {PackageFlag::ClientOptional, "ClientOptional"},
            {PackageFlag::ServerSideOnly, "ServerSideOnly"},
            {PackageFlag::Cooked, "Cooked"},
            {PackageFlag::Unsecure, "Unsecure"},
            {PackageFlag::Encrypted, "Encrypted"},
            {PackageFlag::Need, "Need"},
            {PackageFlag::ContainsMap, "ContainsMap"},
            {PackageFlag::ContainsScript, "ContainsScript"},
            {PackageFlag::ContainsDebugData, "ContainsDebugData"},
            {PackageFlag::Imports, "Imports"},
            {PackageFlag::Compressed, "Compressed"},
            {PackageFlag::FullyCompressed, "FullyCompressed"},
            {PackageFlag::NoExportsData, "NoExportsData"},
            {PackageFlag::Stripped, "Stripped"},
            {PackageFlag::Protected, "Protected"},
        }};
    }

    std::string describe_package_flags(std::uint32_t flags)
    {
        std::string out;
        std::uint32_t known = 0;
        for (const auto& [bit, name] : k_flag_names)
        {
            if ((flags & bit) != 0)
            {
                if (!out.empty())
                {
                    out += '|';
                }
                out += name;
                known |= bit;
            }
        }
        if (const std::uint32_t unknown = flags & ~known; unknown != 0)
        {
            if (!out.empty())
            {
                out += '|';
            }
            out += fmt::format("0x{:08X}", unknown);
        }
        return out.empty() ? std::string("None") : out;
    }

    SerializationContext PackageSummary::context() const noexcept
    {
        SerializationContext ctx;
        ctx.file_version = file_version;
        ctx.licensee_version = licensee_version;
        ctx.endian = endian;
        return ctx;
    }

    PackageSummary read_package_summary(BinaryReader& reader)
    {
        PackageSummary s;
        const std::uint64_t start = reader.absolute_position();

        SerializationContext ctx = reader.context();
        ctx.endian = Endian::Little;
        reader.set_context(ctx);

        s.tag = reader.read_u32();
        if (s.tag == package_signature_swapped)
        {
            s.endian = Endian::Big;
        }
        else if (s.tag != package_signature)
        {
            throw SerializationError(ErrorCode::InvalidData,
                fmt::format("not an Unreal package: signature 0x{:08X}", s.tag), start);
        }
        ctx.endian = s.endian;
        reader.set_context(ctx);

        const std::uint64_t version_offset = reader.absolute_position();
        const std::int32_t legacy_version = reader.read_i32();
        if (legacy_version < 0)
        {
            throw UnsupportedError(
                fmt::format("UE4 package (legacy file version {}) is not supported", legacy_version), version_offset);
        }
        const auto version = static_cast<std::uint32_t>(legacy_version);
        s.file_version = static_cast<std::uint16_t>(version & 0xFFFFu);
        s.licensee_version = static_cast<std::uint16_t>(version >> 16);

        if (s.file_version < min_supported_file_version || s.file_version >= first_unsupported_file_version)
        {
            throw UnsupportedError(
                fmt::format("package file version {} (licensee {}) is outside the supported range [{}, {})",
                            s.file_version, s.licensee_version, min_supported_file_version,
                            first_unsupported_file_version),
                version_offset);
        }

        ctx.file_version = s.file_version;
        ctx.licensee_version = s.licensee_version;
        reader.set_context(ctx);

        // Version >= 249 is guaranteed by the range check above.
        s.header_size = reader.read_i32();
        if (ctx.at_least(LegacyVersion::AddedFolderName))
        {
            s.folder_name = reader.read_fstring();
        }
        s.package_flags = reader.read_u32();

        s.name_count = reader.read_i32();
        s.name_offset = reader.read_i32();
        s.export_count = reader.read_i32();
        s.export_offset = reader.read_i32();
        s.import_count = reader.read_i32();
        s.import_offset = reader.read_i32();

        if (ctx.at_least(LegacyVersion::AddedDependsTable))
        {
            s.depends_offset = reader.read_i32();
        }
        if (ctx.at_least(LegacyVersion::AddedImportExportGuidsTable))
        {
            s.import_export_guids_offset = reader.read_i32();
            s.import_guids_count = reader.read_i32();
            s.export_guids_count = reader.read_i32();
        }
        if (ctx.at_least(LegacyVersion::AddedThumbnailTable))
        {
            s.thumbnail_table_offset = reader.read_i32();
        }

        s.guid = reader.read_guid();

        const std::uint32_t generation_count = reader.read_array_count(12);
        s.generations.reserve(generation_count);
        for (std::uint32_t i = 0; i < generation_count; ++i)
        {
            GenerationInfo g;
            g.export_count = reader.read_i32();
            g.name_count = reader.read_i32();
            if (ctx.at_least(LegacyVersion::NetObjectCountAdded))
            {
                g.net_object_count = reader.read_i32();
            }
            s.generations.push_back(g);
        }

        if (ctx.at_least(LegacyVersion::AddedEngineVersion))
        {
            s.engine_version = reader.read_i32();
        }
        if (ctx.at_least(LegacyVersion::AddedCookerVersion))
        {
            s.cooker_version = reader.read_i32();
        }

        if (ctx.at_least(LegacyVersion::CompressionAdded))
        {
            s.compression_flags = reader.read_u32();
            const std::uint32_t chunk_count = reader.read_array_count(16);
            s.compressed_chunks.reserve(chunk_count);
            for (std::uint32_t i = 0; i < chunk_count; ++i)
            {
                CompressedChunkInfo c;
                c.uncompressed_offset = reader.read_i32();
                c.uncompressed_size = reader.read_i32();
                c.compressed_offset = reader.read_i32();
                c.compressed_size = reader.read_i32();
                s.compressed_chunks.push_back(c);
            }
        }

        if (ctx.at_least(LegacyVersion::AddedPackageSource))
        {
            s.package_source = reader.read_u32();
        }

        if (ctx.at_least(LegacyVersion::AddedAdditionalPackagesToCook))
        {
            const std::uint32_t count = reader.read_array_count(4);
            s.additional_packages_to_cook.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                s.additional_packages_to_cook.push_back(reader.read_fstring());
            }
        }

        if (ctx.at_least(LegacyVersion::AddedTextureAllocations))
        {
            // Each type: SizeX, SizeY, NumMips, Format, TexCreateFlags, TArray<int32> ExportIndices (>= 24 bytes).
            const std::uint32_t count = reader.read_array_count(24);
            s.texture_allocations.resize(count);
            for (TextureAllocation& t : s.texture_allocations)
            {
                t.width = reader.read_i32();
                t.height = reader.read_i32();
                t.mip_count = reader.read_i32();
                t.format = reader.read_u32();
                t.create_flags = reader.read_u32();
                const std::uint32_t exports = reader.read_array_count(4);
                t.export_indices.resize(exports);
                for (std::int32_t& e : t.export_indices) e = reader.read_i32();
            }
        }
        return s;
    }

    void write_package_summary(BinaryWriter& writer, const PackageSummary& s)
    {
        if (s.file_version < min_supported_file_version || s.file_version >= first_unsupported_file_version)
        {
            throw UnsupportedError(fmt::format("cannot write a summary for file version {}", s.file_version));
        }
        const SerializationContext ctx = s.context();
        writer.set_context(ctx);

        writer.write_u32(package_signature); // written in the package's own byte order
        writer.write_u32(static_cast<std::uint32_t>(s.file_version) |
                         (static_cast<std::uint32_t>(s.licensee_version) << 16));
        writer.write_i32(s.header_size);
        if (ctx.at_least(LegacyVersion::AddedFolderName))
        {
            writer.write_fstring(s.folder_name);
        }
        writer.write_u32(s.package_flags);
        writer.write_i32(s.name_count);
        writer.write_i32(s.name_offset);
        writer.write_i32(s.export_count);
        writer.write_i32(s.export_offset);
        writer.write_i32(s.import_count);
        writer.write_i32(s.import_offset);
        if (ctx.at_least(LegacyVersion::AddedDependsTable))
        {
            writer.write_i32(s.depends_offset);
        }
        if (ctx.at_least(LegacyVersion::AddedImportExportGuidsTable))
        {
            writer.write_i32(s.import_export_guids_offset);
            writer.write_i32(s.import_guids_count);
            writer.write_i32(s.export_guids_count);
        }
        if (ctx.at_least(LegacyVersion::AddedThumbnailTable))
        {
            writer.write_i32(s.thumbnail_table_offset);
        }
        writer.write_guid(s.guid);
        writer.write_i32(static_cast<std::int32_t>(s.generations.size()));
        for (const auto& g : s.generations)
        {
            writer.write_i32(g.export_count);
            writer.write_i32(g.name_count);
            if (ctx.at_least(LegacyVersion::NetObjectCountAdded))
            {
                writer.write_i32(g.net_object_count);
            }
        }
        if (ctx.at_least(LegacyVersion::AddedEngineVersion))
        {
            writer.write_i32(s.engine_version);
        }
        if (ctx.at_least(LegacyVersion::AddedCookerVersion))
        {
            writer.write_i32(s.cooker_version);
        }
        if (ctx.at_least(LegacyVersion::CompressionAdded))
        {
            writer.write_u32(s.compression_flags);
            writer.write_i32(static_cast<std::int32_t>(s.compressed_chunks.size()));
            for (const auto& c : s.compressed_chunks)
            {
                writer.write_i32(c.uncompressed_offset);
                writer.write_i32(c.uncompressed_size);
                writer.write_i32(c.compressed_offset);
                writer.write_i32(c.compressed_size);
            }
        }
        if (ctx.at_least(LegacyVersion::AddedPackageSource))
        {
            writer.write_u32(s.package_source);
        }
        if (ctx.at_least(LegacyVersion::AddedAdditionalPackagesToCook))
        {
            writer.write_i32(static_cast<std::int32_t>(s.additional_packages_to_cook.size()));
            for (const auto& name : s.additional_packages_to_cook)
            {
                writer.write_fstring(name);
            }
        }
        if (ctx.at_least(LegacyVersion::AddedTextureAllocations))
        {
            writer.write_i32(static_cast<std::int32_t>(s.texture_allocations.size()));
            for (const TextureAllocation& t : s.texture_allocations)
            {
                writer.write_i32(t.width);
                writer.write_i32(t.height);
                writer.write_i32(t.mip_count);
                writer.write_u32(t.format);
                writer.write_u32(t.create_flags);
                writer.write_i32(static_cast<std::int32_t>(t.export_indices.size()));
                for (const std::int32_t e : t.export_indices) writer.write_i32(e);
            }
        }
    }
}
