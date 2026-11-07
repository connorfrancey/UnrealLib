#pragma once

/// @file
/// @brief UE3 bulk data (FUntypedBulkData): the container of texture mips, sound data and other large payloads.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace UnrealLib
{
    struct SerializationContext;
    class BinaryReader;
    class BinaryWriter;
    class Package;

    /// @brief Bulk data flags (upstream UELib `BulkDataFlags`).
    namespace BulkDataFlag
    {
        inline constexpr std::uint32_t StoreInSeparateFile = 0x01; ///< Payload lives in another file.
        inline constexpr std::uint32_t CompressedZlib = 0x02;     ///< Payload is a zlib compressed chunk.
        inline constexpr std::uint32_t ForceSingleElementSerialization = 0x04; ///< Serialized element by element.
        inline constexpr std::uint32_t SingleUse = 0x08;          ///< Freed after first use.
        inline constexpr std::uint32_t CompressedLzo = 0x10;      ///< Payload is an LZO compressed chunk.
        inline constexpr std::uint32_t Unused = 0x20;             ///< No payload (stripped by the cooker).
        inline constexpr std::uint32_t StoreOnlyPayload = 0x40;   ///< Payload stored elsewhere in this file.
        inline constexpr std::uint32_t CompressedLzx = 0x80;      ///< Payload is an LZX compressed chunk.
        inline constexpr std::uint32_t Compressed = CompressedZlib | CompressedLzo | CompressedLzx; ///< Any method.
    }

    /// @brief One FUntypedBulkData record.
    ///
    /// Layout (file version >= 266): uint32 flags, int32 element count, int32 size on disk, int32 offset in file,
    /// then the payload itself unless it is stored in a separate file (`StoreInSeparateFile`) or elsewhere
    /// (`StoreOnlyPayload`). `offset_in_file` is absolute in the (decompressed) package image.
    ///
    /// Inline payloads are read at the position that follows the record, like UE3 does, not at `offset_in_file`: in
    /// 30 Mirror's Edge packages every stored offset is off by a per-package constant (138 or 216 bytes), while the
    /// payload does follow its record.
    struct BulkData
    {
        std::uint32_t flags = 0;          ///< BulkDataFlag bits.
        std::int32_t element_count = 0;   ///< Number of elements (bytes, for byte bulk data).
        std::int32_t size_on_disk = 0;    ///< Stored payload size (compressed size when compressed).
        std::int32_t offset_in_file = 0;  ///< Payload offset as stored.
        std::uint64_t payload_offset = 0; ///< Where the inline payload actually is (absolute; 0 if not inline).

        /// @brief Whether the stored offset disagrees with the actual payload position (inline payloads only).
        [[nodiscard]] bool offset_mismatch() const noexcept
        {
            return is_inline() && static_cast<std::uint64_t>(static_cast<std::uint32_t>(offset_in_file)) !=
                                      payload_offset;
        }

        /// @brief Whether the payload is stored inline, in this package.
        [[nodiscard]] bool is_inline() const noexcept
        {
            return (flags & (BulkDataFlag::StoreInSeparateFile | BulkDataFlag::StoreOnlyPayload)) == 0;
        }

        /// @brief Whether there is a payload at all (not unused and not empty).
        [[nodiscard]] bool has_payload() const noexcept
        {
            return (flags & BulkDataFlag::Unused) == 0 && element_count > 0;
        }

        /// @brief Whether the payload is compressed.
        [[nodiscard]] bool is_compressed() const noexcept { return (flags & BulkDataFlag::Compressed) != 0; }

        /// @brief Readable flag list ("StoreInSeparateFile|CompressedLzo").
        [[nodiscard]] std::string describe_flags() const;
    };

    /// @brief Reads a bulk data record and skips its inline payload, which must fit in the reader.
    /// @throws SerializationError on malformed records.
    [[nodiscard]] BulkData read_bulk_data(BinaryReader& reader);

    /// @brief Writes a bulk data record with its inline payload (@p payload must already be in stored form,
    ///        i.e. compressed if the flags say so). The size and offset are recomputed from @p payload and
    ///        `base_offset + writer.position()`, which must be the record's final position in the file.
    ///        Records that are not inline are written without payload, unchanged.
    void write_bulk_data(BinaryWriter& writer, BulkData& record, std::span<const std::byte> payload,
                         std::uint64_t base_offset = 0);

    /// @brief The stored bytes of an inline payload (still compressed if the record is compressed).
    /// @throws SerializationError if the payload is outside the package; UnsupportedError if not inline.
    [[nodiscard]] std::span<const std::byte> bulk_data_stored_bytes(const Package& package, const BulkData& record);

    /// @brief Loads the payload of an inline record (decompressing zlib/LZO chunks): exactly
    ///        `element_count * element_size` bytes.
    /// @throws UnsupportedError for separate-file payloads and LZX; CompressionError / SerializationError on bad
    ///         data; LimitExceeded if larger than the package's allocation limit.
    [[nodiscard]] std::vector<std::byte> load_bulk_data(const Package& package, const BulkData& record,
                                                        std::size_t element_size = 1);

    /// @brief Loads the payload of a `StoreInSeparateFile` record from @p file, where it is stored at
    ///        `offset_in_file` of the file *as stored on disk* (Mirror's Edge: the package named by the object's
    ///        outermost package; verified on every streamed texture mip of the game).
    /// @throws IoError if the file cannot be read; otherwise as load_bulk_data().
    [[nodiscard]] std::vector<std::byte> load_bulk_data_from_file(const std::filesystem::path& file,
                                                                  const BulkData& record,
                                                                  const SerializationContext& context,
                                                                  std::size_t element_size = 1);
}
