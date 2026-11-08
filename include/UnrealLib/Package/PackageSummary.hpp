#pragma once

/// @file
/// @brief UE3 package file summary (FPackageFileSummary).

#include <cstdint>
#include <string>
#include <vector>

#include <UnrealLib/Core/Guid.hpp>
#include <UnrealLib/Serialization/FString.hpp>
#include <UnrealLib/Serialization/SerializationContext.hpp>

namespace UnrealLib
{
    class BinaryReader;
    class BinaryWriter;

    /// @brief Package signature (PACKAGE_FILE_TAG) as read little-endian.
    inline constexpr std::uint32_t package_signature = 0x9E2A83C1u;

    /// @brief Package signature as seen when a big-endian package is read little-endian.
    inline constexpr std::uint32_t package_signature_swapped = 0xC1832A9Eu;

    /// @brief UE3 package flag bits (summary PackageFlags). Values from upstream UELib `UnrealFlags.cs`
    ///        (PackageFlags, UE3 meanings).
    namespace PackageFlag
    {
        inline constexpr std::uint32_t AllowDownload = 0x00000001u; ///< Clients may download the package.
        inline constexpr std::uint32_t ClientOptional = 0x00000002u; ///< Optional for clients.
        inline constexpr std::uint32_t ServerSideOnly = 0x00000004u; ///< Only needed on the server.
        inline constexpr std::uint32_t Cooked = 0x00000008u; ///< Package was cooked for a target platform.
        inline constexpr std::uint32_t Unsecure = 0x00000010u; ///< Not trusted (UE2 legacy meaning).
        inline constexpr std::uint32_t Encrypted = 0x00000020u; ///< Package is encrypted.
        inline constexpr std::uint32_t Need = 0x00008000u; ///< Clients must download the package.
        inline constexpr std::uint32_t ContainsMap = 0x00020000u; ///< Package contains a map/level.
        inline constexpr std::uint32_t ContainsScript = 0x00200000u; ///< Package contains UnrealScript classes.
        inline constexpr std::uint32_t ContainsDebugData = 0x00400000u; ///< Package was compiled with debug info.
        inline constexpr std::uint32_t Imports = 0x00800000u; ///< Package imports other packages (UE3 meaning per upstream).
        inline constexpr std::uint32_t Compressed = 0x02000000u; ///< Package is stored compressed.
        inline constexpr std::uint32_t FullyCompressed = 0x04000000u; ///< Package is stored fully compressed.
        inline constexpr std::uint32_t NoExportsData = 0x20000000u; ///< Export data (editor-only) was stripped.
        inline constexpr std::uint32_t Stripped = 0x40000000u; ///< Package source/text was stripped.
        inline constexpr std::uint32_t Protected = 0x80000000u; ///< Package is protected.
    }

    /// @brief Returns the names of the known flags set in @p flags, joined by '|', plus any unknown remainder in hex.
    [[nodiscard]] std::string describe_package_flags(std::uint32_t flags);

    /// @brief One texture allocation record (FTextureAllocations::FTextureType, file version >= 767): textures of one
    ///        size/format that the cooker grouped for the streaming pool.
    struct TextureAllocation
    {
        std::int32_t width = 0;                  ///< SizeX.
        std::int32_t height = 0;                 ///< SizeY.
        std::int32_t mip_count = 0;              ///< NumMips.
        std::uint32_t format = 0;                ///< EPixelFormat.
        std::uint32_t create_flags = 0;          ///< TexCreateFlags.
        std::vector<std::int32_t> export_indices; ///< Exports (0-based) of the textures of this type.

        /// @brief Memberwise comparison.
        friend bool operator==(const TextureAllocation&, const TextureAllocation&) = default;
    };

    /// @brief One generation record (FGenerationInfo).
    struct GenerationInfo
    {
        std::int32_t export_count = 0;     ///< Exports in this generation.
        std::int32_t name_count = 0;       ///< Names in this generation.
        std::int32_t net_object_count = 0; ///< Net objects (version >= 322).

        /// @brief Memberwise comparison.
        friend bool operator==(const GenerationInfo&, const GenerationInfo&) = default;
    };

    /// @brief One compressed chunk record (FCompressedChunk).
    ///
    /// On-disk order: UncompressedOffset, UncompressedSize, CompressedOffset, CompressedSize. Verified on Mirror's
    /// Edge Core.u (UncompressedOffset == NameOffset, CompressedOffset == end of summary). Upstream UELib's
    /// CompressedChunk.Deserialize reads the fields in a different order than its own Serialize; UnrealLib follows
    /// the verified on-disk order.
    struct CompressedChunkInfo
    {
        std::int32_t uncompressed_offset = 0; ///< Offset of the chunk in the decompressed package.
        std::int32_t uncompressed_size = 0;   ///< Size of the chunk once decompressed.
        std::int32_t compressed_offset = 0;   ///< Offset of the compressed chunk header in the file.
        std::int32_t compressed_size = 0;     ///< Size of the compressed chunk (header + blocks) in the file.

        /// @brief Memberwise comparison.
        friend bool operator==(const CompressedChunkInfo&, const CompressedChunkInfo&) = default;
    };

    /// @brief FPackageFileSummary for UE3 packages (file version 249 and newer, legacy UE3 default branch).
    ///
    /// Field presence follows upstream UELib `UnrealPackage.PackageFileSummary.Deserialize` (default branch, no
    /// game-specific overrides). Fields that are absent for the package's version keep their default value.
    struct PackageSummary
    {
        std::uint32_t tag = package_signature; ///< Signature as read (little-endian interpretation).
        Endian endian = Endian::Little;        ///< Byte order of the package.
        std::uint16_t file_version = 0;        ///< Package file version (e.g. 536 for Mirror's Edge).
        std::uint16_t licensee_version = 0;    ///< Licensee version (e.g. 43 for Mirror's Edge).
        std::int32_t header_size = 0;          ///< Total header size (>= 249).
        FString folder_name;                   ///< Editor folder name (>= 269), usually "None".
        std::uint32_t package_flags = 0;       ///< See PackageFlag.
        std::int32_t name_count = 0;           ///< Name-table entries.
        std::int32_t name_offset = 0;          ///< Name-table offset (in the decompressed package).
        std::int32_t export_count = 0;         ///< Export-table entries.
        std::int32_t export_offset = 0;        ///< Export-table offset.
        std::int32_t import_count = 0;         ///< Import-table entries.
        std::int32_t import_offset = 0;        ///< Import-table offset.
        std::int32_t depends_offset = 0;       ///< Depends-table offset (>= 415).
        std::int32_t import_export_guids_offset = 0; ///< (>= 623)
        std::int32_t import_guids_count = 0;   ///< (>= 623)
        std::int32_t export_guids_count = 0;   ///< (>= 623)
        std::int32_t thumbnail_table_offset = 0; ///< (>= 584)
        Guid guid;                             ///< Package GUID.
        std::vector<GenerationInfo> generations; ///< Generation history.
        std::int32_t engine_version = 0;       ///< Engine build that saved the package (>= 245), e.g. 3716.
        std::int32_t cooker_version = 0;       ///< Cooker version (>= 277).
        std::uint32_t compression_flags = 0;   ///< See CompressionFlag (>= 334).
        std::vector<CompressedChunkInfo> compressed_chunks; ///< Fully-compressed package chunks (>= 334).
        std::uint32_t package_source = 0;      ///< CRC of the package source (>= 482).
        std::vector<FString> additional_packages_to_cook; ///< (>= 516)
        std::vector<TextureAllocation> texture_allocations; ///< Texture pool hints of cooked packages (>= 767).

        /// @brief Serialization context implied by this summary (versions + endianness, default limits).
        [[nodiscard]] SerializationContext context() const noexcept;

        /// @brief Whether the package body is stored as compressed chunks.
        [[nodiscard]] bool is_compressed() const noexcept { return !compressed_chunks.empty(); }

        /// @brief Memberwise comparison.
        friend bool operator==(const PackageSummary&, const PackageSummary&) = default;
    };

    /// @brief Smallest file version whose summary layout UnrealLib implements.
    inline constexpr std::uint16_t min_supported_file_version = to_value(LegacyVersion::AddedTotalHeaderSize);

    /// @brief File versions at or above this value are not UE3 (868 is the last UE3/UDK file version; UE4 packages
    ///        are recognised by their negative legacy version).
    inline constexpr std::uint16_t first_unsupported_file_version = 869;

    /// @brief Reads a summary starting at the reader's current position (normally 0).
    ///
    /// The reader's limits are honoured; its version/endianness are replaced as soon as the version is known.
    /// @throws SerializationError on malformed data; UnsupportedError for UE4 (negative legacy version) packages and
    ///         for file versions outside [min_supported_file_version, first_unsupported_file_version).
    [[nodiscard]] PackageSummary read_package_summary(BinaryReader& reader);

    /// @brief Writes @p summary (inverse of read_package_summary()). The writer's context is replaced by
    ///        summary.context().
    void write_package_summary(BinaryWriter& writer, const PackageSummary& summary);
}
