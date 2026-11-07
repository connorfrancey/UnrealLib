#pragma once

/// @file
/// @brief Decompression of fully-compressed UE3 packages (summary CompressedChunks).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <UnrealLib/Package/PackageSummary.hpp>
#include <UnrealLib/Serialization/Compression.hpp>

namespace UnrealLib
{
    class BinaryReader;

    /// @brief Default block size used when the chunk header stores the package signature in its block-size field
    ///        (upstream UELib CompressedChunkHeader.Deserialize).
    inline constexpr std::int32_t default_compression_block_size = 0x20000;

    /// @brief Sizes of one compressed block (FCompressedChunkInfo).
    struct CompressedBlock
    {
        std::int32_t compressed_size = 0;   ///< Compressed bytes.
        std::int32_t uncompressed_size = 0; ///< Decompressed bytes.
    };

    /// @brief The header at the start of every compressed chunk (FCompressedChunkHeader / FBulkData-style layout).
    ///
    /// Layout: uint32 tag (package signature), int32 block_size, summary block, then
    /// ceil(summary.uncompressed_size / block_size) blocks, followed by the compressed block payloads.
    struct CompressedChunkHeader
    {
        std::uint32_t tag = 0;            ///< Must be the package signature.
        std::int32_t block_size = 0;      ///< Maximum uncompressed size per block (after signature substitution).
        CompressedBlock summary;          ///< Totals.
        std::vector<CompressedBlock> blocks; ///< Per-block sizes.
    };

    /// @brief Reads and validates a compressed chunk header (block sizes must sum to the summary totals).
    [[nodiscard]] CompressedChunkHeader read_compressed_chunk_header(BinaryReader& reader);

    /// @brief Decompresses one chunk.
    /// @param file   The complete package file.
    /// @param chunk  Chunk record from the summary.
    /// @param method Compression method (from the summary's compression flags).
    /// @param output Destination; must be exactly `chunk.uncompressed_size` bytes.
    /// @param context Byte order and limits.
    void decompress_chunk(std::span<const std::byte> file, const CompressedChunkInfo& chunk, CompressionMethod method,
                          std::span<std::byte> output, const SerializationContext& context);

    /// @brief Produces the decompressed package image.
    ///
    /// The bytes before the first chunk's uncompressed offset (the summary) are copied from the file verbatim; each
    /// chunk is decompressed to its uncompressed offset. Chunks must be non-overlapping and contiguous.
    /// @throws CompressionError / SerializationError / UnsupportedError on failure.
    [[nodiscard]] std::vector<std::byte> decompress_package(std::span<const std::byte> file,
                                                            const PackageSummary& summary,
                                                            const SerializationLimits& limits);
}
