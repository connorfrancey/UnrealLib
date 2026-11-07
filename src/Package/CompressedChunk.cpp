#include <UnrealLib/Package/CompressedChunk.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>

namespace UnrealLib
{
    CompressedChunkHeader read_compressed_chunk_header(BinaryReader& reader)
    {
        const std::uint64_t start = reader.absolute_position();
        CompressedChunkHeader header;
        header.tag = reader.read_u32();
        if (header.tag != package_signature)
        {
            throw CompressionError(fmt::format("compressed chunk has bad tag 0x{:08X}", header.tag), start);
        }
        header.block_size = reader.read_i32();
        if (static_cast<std::uint32_t>(header.block_size) == package_signature)
        {
            header.block_size = default_compression_block_size;
        }
        if (header.block_size <= 0)
        {
            throw CompressionError(fmt::format("compressed chunk has invalid block size {}", header.block_size), start);
        }
        header.summary.compressed_size = reader.read_i32();
        header.summary.uncompressed_size = reader.read_i32();
        if (header.summary.compressed_size < 0 || header.summary.uncompressed_size < 0)
        {
            throw CompressionError("compressed chunk has negative sizes", start);
        }

        const auto total = static_cast<std::uint64_t>(header.summary.uncompressed_size);
        const auto block_size = static_cast<std::uint64_t>(header.block_size);
        const std::uint64_t block_count = (total + block_size - 1) / block_size;
        if (block_count * 8 > reader.remaining())
        {
            throw CompressionError(fmt::format("compressed chunk declares {} blocks, more than the data can hold",
                                               block_count),
                                   start);
        }

        header.blocks.reserve(static_cast<std::size_t>(block_count));
        std::uint64_t sum_compressed = 0;
        std::uint64_t sum_uncompressed = 0;
        for (std::uint64_t i = 0; i < block_count; ++i)
        {
            CompressedBlock block;
            block.compressed_size = reader.read_i32();
            block.uncompressed_size = reader.read_i32();
            if (block.compressed_size < 0 || block.uncompressed_size < 0 ||
                block.uncompressed_size > header.block_size)
            {
                throw CompressionError(fmt::format("compressed block {} has invalid sizes ({} -> {})", i,
                                                   block.compressed_size, block.uncompressed_size),
                                       start);
            }
            sum_compressed += static_cast<std::uint64_t>(block.compressed_size);
            sum_uncompressed += static_cast<std::uint64_t>(block.uncompressed_size);
            header.blocks.push_back(block);
        }
        if (sum_uncompressed != total ||
            sum_compressed != static_cast<std::uint64_t>(header.summary.compressed_size))
        {
            throw CompressionError(
                fmt::format("compressed block sizes ({} -> {}) do not match the chunk summary ({} -> {})",
                            sum_compressed, sum_uncompressed, header.summary.compressed_size,
                            header.summary.uncompressed_size),
                start);
        }
        return header;
    }

    void decompress_chunk(std::span<const std::byte> file, const CompressedChunkInfo& chunk, CompressionMethod method,
                          std::span<std::byte> output, const SerializationContext& context)
    {
        if (chunk.compressed_offset < 0 || chunk.compressed_size < 0 || chunk.uncompressed_size < 0)
        {
            throw CompressionError("compressed chunk record has negative values");
        }
        if (output.size() != static_cast<std::size_t>(chunk.uncompressed_size))
        {
            throw CompressionError(fmt::format("output buffer is {} bytes, chunk is {} bytes", output.size(),
                                               chunk.uncompressed_size));
        }

        BinaryReader file_reader(file, context);
        BinaryReader reader = file_reader.sub_reader(static_cast<std::size_t>(chunk.compressed_offset),
                                                     static_cast<std::size_t>(chunk.compressed_size));
        const CompressedChunkHeader header = read_compressed_chunk_header(reader);
        if (header.summary.uncompressed_size != chunk.uncompressed_size)
        {
            throw CompressionError(fmt::format("chunk header says {} uncompressed bytes, summary says {}",
                                               header.summary.uncompressed_size, chunk.uncompressed_size),
                                   reader.base_offset());
        }

        std::size_t out_position = 0;
        for (const CompressedBlock& block : header.blocks)
        {
            const std::uint64_t block_offset = reader.absolute_position();
            const auto input = reader.read_view(static_cast<std::size_t>(block.compressed_size));
            auto destination = output.subspan(out_position, static_cast<std::size_t>(block.uncompressed_size));
            // The context is added outside the handler: throwing from inside a catch block crashes clang-cl
            // AddressSanitizer builds on Windows (catch funclets), see docs/development.md.
            std::optional<std::string> failure;
            try
            {
                decompress(method, input, destination);
            }
            catch (const CompressionError& e)
            {
                failure = fmt::format("block at file offset 0x{:X}: {}", block_offset, e.message());
            }
            if (failure)
            {
                throw CompressionError(*failure, block_offset);
            }
            out_position += static_cast<std::size_t>(block.uncompressed_size);
        }
    }

    std::vector<std::byte> decompress_package(std::span<const std::byte> file, const PackageSummary& summary,
                                              const SerializationLimits& limits)
    {
        if (!summary.is_compressed())
        {
            return {file.begin(), file.end()};
        }
        const CompressionMethod method = compression_method_from_flags(summary.compression_flags);
        if (method == CompressionMethod::None)
        {
            throw CompressionError("package has compressed chunks but no compression flags");
        }

        std::vector<CompressedChunkInfo> chunks = summary.compressed_chunks;
        std::sort(chunks.begin(), chunks.end(),
                  [](const auto& a, const auto& b) { return a.uncompressed_offset < b.uncompressed_offset; });

        const std::int32_t first = chunks.front().uncompressed_offset;
        if (first < 0 || static_cast<std::size_t>(first) > file.size())
        {
            throw CompressionError(fmt::format("first chunk offset {} is outside the file", first));
        }

        std::uint64_t total = static_cast<std::uint64_t>(first);
        for (const auto& chunk : chunks)
        {
            if (chunk.uncompressed_size < 0 || static_cast<std::uint64_t>(chunk.uncompressed_offset) != total)
            {
                throw CompressionError(fmt::format("compressed chunks are not contiguous (chunk at {} expected at {})",
                                                   chunk.uncompressed_offset, total));
            }
            total += static_cast<std::uint64_t>(chunk.uncompressed_size);
        }
        if (total > limits.max_decompressed_size || total > limits.max_allocation_bytes)
        {
            throw SerializationError(ErrorCode::LimitExceeded,
                fmt::format("decompressed package size {} exceeds the configured limit", total));
        }

        std::vector<std::byte> output(static_cast<std::size_t>(total));
        std::memcpy(output.data(), file.data(), static_cast<std::size_t>(first));

        SerializationContext context = summary.context();
        context.limits = limits;
        for (const auto& chunk : chunks)
        {
            decompress_chunk(file, chunk, method,
                             std::span<std::byte>(output).subspan(static_cast<std::size_t>(chunk.uncompressed_offset),
                                                                  static_cast<std::size_t>(chunk.uncompressed_size)),
                             context);
        }
        return output;
    }
}
