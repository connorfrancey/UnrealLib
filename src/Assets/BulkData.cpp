#include <UnrealLib/Assets/BulkData.hpp>

#include <fstream>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/CompressedChunk.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

namespace UnrealLib
{
    std::string BulkData::describe_flags() const
    {
        static constexpr std::pair<std::uint32_t, const char*> names[] = {
            {BulkDataFlag::StoreInSeparateFile, "StoreInSeparateFile"},
            {BulkDataFlag::CompressedZlib, "CompressedZlib"},
            {BulkDataFlag::ForceSingleElementSerialization, "ForceSingleElementSerialization"},
            {BulkDataFlag::SingleUse, "SingleUse"},
            {BulkDataFlag::CompressedLzo, "CompressedLzo"},
            {BulkDataFlag::Unused, "Unused"},
            {BulkDataFlag::StoreOnlyPayload, "StoreOnlyPayload"},
            {BulkDataFlag::CompressedLzx, "CompressedLzx"},
        };
        std::string out;
        std::uint32_t rest = flags;
        for (const auto& [bit, name] : names)
        {
            if ((flags & bit) != 0)
            {
                out += out.empty() ? "" : "|";
                out += name;
                rest &= ~bit;
            }
        }
        if (rest != 0)
        {
            out += fmt::format("{}0x{:X}", out.empty() ? "" : "|", rest);
        }
        return out.empty() ? "None" : out;
    }

    BulkData read_bulk_data(BinaryReader& reader)
    {
        BulkData record;
        const std::uint64_t start = reader.absolute_position();
        record.flags = reader.read_u32();
        record.element_count = reader.read_i32();
        record.size_on_disk = reader.read_i32();
        record.offset_in_file = reader.read_i32();
        if (record.element_count < 0)
        {
            throw SerializationError(fmt::format("bulk data has negative element count {}", record.element_count),
                                     start);
        }
        if (!record.is_inline())
        {
            return record;
        }
        if (record.size_on_disk < 0)
        {
            throw SerializationError(fmt::format("bulk data has negative size {}", record.size_on_disk), start);
        }
        record.payload_offset = reader.absolute_position();
        (void)reader.read_view(static_cast<std::size_t>(record.size_on_disk));
        return record;
    }

    void write_bulk_data(BinaryWriter& writer, BulkData& record, std::span<const std::byte> payload,
                         std::uint64_t base_offset)
    {
        if (record.is_inline())
        {
            record.size_on_disk = static_cast<std::int32_t>(payload.size());
            record.payload_offset = base_offset + writer.position() + 16;
            record.offset_in_file = static_cast<std::int32_t>(record.payload_offset);
        }
        writer.write_u32(record.flags);
        writer.write_i32(record.element_count);
        writer.write_i32(record.size_on_disk);
        writer.write_i32(record.offset_in_file);
        if (record.is_inline())
        {
            writer.write_bytes(payload);
        }
    }

    std::span<const std::byte> bulk_data_stored_bytes(const Package& package, const BulkData& record)
    {
        if (!record.is_inline())
        {
            throw UnsupportedError(fmt::format("bulk data payload is not stored inline (flags {})",
                                               record.describe_flags()));
        }
        const auto data = package.data();
        const std::uint64_t offset = record.payload_offset;
        const auto size = static_cast<std::uint64_t>(static_cast<std::uint32_t>(record.size_on_disk));
        if (record.size_on_disk < 0 || offset + size > data.size())
        {
            throw SerializationError(
                fmt::format("bulk data [{} +{}] is outside the package ({} bytes)", offset,
                            record.size_on_disk, data.size()),
                offset);
        }
        return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    }

    namespace
    {
        std::vector<std::byte> decode_stored(std::span<const std::byte> stored, const BulkData& record,
                                             std::uint64_t total, const SerializationContext& context)
        {
            if (!record.is_compressed())
            {
                if (stored.size() != total)
                {
                    throw SerializationError(fmt::format("bulk data stores {} bytes, expected {}", stored.size(), total));
                }
                return {stored.begin(), stored.end()};
            }
            CompressionMethod method = CompressionMethod::Lzx;
            if ((record.flags & BulkDataFlag::CompressedZlib) != 0) method = CompressionMethod::Zlib;
            else if ((record.flags & BulkDataFlag::CompressedLzo) != 0) method = CompressionMethod::Lzo;
            std::vector<std::byte> output(static_cast<std::size_t>(total));
            CompressedChunkInfo chunk;
            chunk.compressed_size = static_cast<std::int32_t>(stored.size());
            chunk.uncompressed_size = static_cast<std::int32_t>(total);
            decompress_chunk(stored, chunk, method, output, context);
            return output;
        }
    }

    std::vector<std::byte> load_bulk_data(const Package& package, const BulkData& record, std::size_t element_size)
    {
        if (!record.has_payload())
        {
            return {};
        }
        const SerializationContext context = package.context();
        const std::uint64_t total = static_cast<std::uint64_t>(record.element_count) * element_size;
        if (total > context.limits.max_allocation_bytes)
        {
            throw Error(ErrorCode::LimitExceeded,
                        fmt::format("bulk data of {} bytes exceeds the allocation limit", total));
        }
        const auto stored = bulk_data_stored_bytes(package, record);
        if (!record.is_compressed())
        {
            if (stored.size() != total)
            {
                throw SerializationError(fmt::format("bulk data stores {} bytes for {} elements of {} bytes",
                                                     stored.size(), record.element_count, element_size),
                                         record.payload_offset);
            }
            return {stored.begin(), stored.end()};
        }

        return decode_stored(stored, record, total, context);
    }

    std::vector<std::byte> load_bulk_data_from_file(const std::filesystem::path& file, const BulkData& record,
                                                    const SerializationContext& context, std::size_t element_size)
    {
        if (!record.has_payload())
        {
            return {};
        }
        if (record.offset_in_file < 0 || record.size_on_disk < 0)
        {
            throw SerializationError(fmt::format("bulk data record has offset {} and size {}", record.offset_in_file,
                                                 record.size_on_disk));
        }
        const std::uint64_t total = static_cast<std::uint64_t>(record.element_count) * element_size;
        if (total > context.limits.max_allocation_bytes ||
            static_cast<std::uint64_t>(record.size_on_disk) > context.limits.max_allocation_bytes)
        {
            throw Error(ErrorCode::LimitExceeded, fmt::format("bulk data of {} bytes exceeds the allocation limit", total));
        }
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            throw IoError(fmt::format("cannot open '{}'", file.string()));
        }
        in.seekg(0, std::ios::end);
        const auto file_size = static_cast<std::uint64_t>(in.tellg());
        const auto offset = static_cast<std::uint64_t>(record.offset_in_file);
        const auto size = static_cast<std::uint64_t>(record.size_on_disk);
        if (offset + size > file_size)
        {
            throw SerializationError(fmt::format("bulk data [{} +{}] is outside '{}' ({} bytes)", offset, size,
                                                 file.filename().string(), file_size));
        }
        std::vector<std::byte> stored(static_cast<std::size_t>(size));
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
        if (!in)
        {
            throw IoError(fmt::format("cannot read '{}'", file.string()));
        }
        return decode_stored(stored, record, total, context);
    }
}
