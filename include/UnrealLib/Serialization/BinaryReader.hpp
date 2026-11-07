#pragma once

/// @file
/// @brief Bounds-checked reader over an in-memory byte range.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <UnrealLib/Core/Guid.hpp>
#include <UnrealLib/Serialization/FString.hpp>
#include <UnrealLib/Serialization/SerializationContext.hpp>

namespace UnrealLib
{
    /// @brief Reads primitive and Unreal-specific values from a non-owning byte range.
    ///
    /// Every read is bounds-checked and throws SerializationError (ErrorCode::OutOfBounds / InvalidData /
    /// LimitExceeded) on failure; the reported offset is absolute (base_offset + position). Values are decoded field
    /// by field with explicit byte order — memory is never reinterpreted as structures.
    ///
    /// The reader does not own the data; the caller must keep it alive.
    class BinaryReader
    {
    public:
        /// @param data        Bytes to read.
        /// @param context     Version/limit information used by version-dependent primitives.
        /// @param base_offset Absolute offset of `data[0]` in the original input, used in error messages.
        explicit BinaryReader(std::span<const std::byte> data, SerializationContext context = {},
                              std::uint64_t base_offset = 0) noexcept;

        /// @brief Deleted: a reader over a temporary buffer would dangle as soon as the full expression ends.
        ///        Keep the buffer in a named variable (or MemoryArchive) and pass it as a span.
        BinaryReader(std::vector<std::byte>&& data, SerializationContext context = {},
                     std::uint64_t base_offset = 0) = delete;

        /// @name Position
        /// @{
        [[nodiscard]] std::size_t size() const noexcept { return m_data.size(); }        ///< Total size.
        [[nodiscard]] std::size_t position() const noexcept { return m_position; }       ///< Current position.
        [[nodiscard]] std::size_t remaining() const noexcept { return m_data.size() - m_position; } ///< Bytes left.
        [[nodiscard]] bool at_end() const noexcept { return m_position == m_data.size(); } ///< No bytes left.
        [[nodiscard]] std::uint64_t base_offset() const noexcept { return m_base_offset; } ///< Absolute offset of byte 0.
        [[nodiscard]] std::uint64_t absolute_position() const noexcept { return m_base_offset + m_position; } ///< base + position.

        /// @brief Moves to @p position (0..size() inclusive). Throws SerializationError if out of range.
        void seek(std::size_t position);

        /// @brief Advances by @p count bytes. Throws SerializationError if fewer bytes remain.
        void skip(std::size_t count);
        /// @}

        /// @name Context
        /// @{
        [[nodiscard]] const SerializationContext& context() const noexcept { return m_context; } ///< Current context.
        void set_context(const SerializationContext& context) noexcept { m_context = context; } ///< Replaces the context.
        /// @}

        /// @name Fixed-size primitives (byte order from the context)
        /// @{

        /// @brief Reads an unsigned 8-bit integer.
        std::uint8_t read_u8();
        /// @brief Reads an unsigned 16-bit integer.
        std::uint16_t read_u16();
        /// @brief Reads an unsigned 32-bit integer.
        std::uint32_t read_u32();
        /// @brief Reads an unsigned 64-bit integer.
        std::uint64_t read_u64();
        /// @brief Reads a signed 8-bit integer.
        std::int8_t read_i8();
        /// @brief Reads a signed 16-bit integer.
        std::int16_t read_i16();
        /// @brief Reads a signed 32-bit integer.
        std::int32_t read_i32();
        /// @brief Reads a signed 64-bit integer.
        std::int64_t read_i64();
        /// @brief Reads an IEEE-754 single-precision float.
        float read_f32();
        /// @brief Reads an IEEE-754 double-precision float.
        double read_f64();
        /// @brief Reads a 32-bit UBOOL; any non-zero value is true.
        bool read_bool32();
        /// @brief Reads an FGuid (four uint32 values).
        Guid read_guid();
        /// @}

        /// @name Raw bytes
        /// @{

        /// @brief Copies `out.size()` bytes into @p out.
        void read_bytes(std::span<std::byte> out);
        /// @brief Returns a view of the next @p count bytes and advances past them (no copy).
        [[nodiscard]] std::span<const std::byte> read_view(std::size_t count);
        /// @brief Copies the next @p count bytes into a new vector (subject to max_allocation_bytes).
        [[nodiscard]] std::vector<std::byte> read_byte_vector(std::size_t count);
        /// @brief Returns a view of the next @p count bytes without advancing.
        [[nodiscard]] std::span<const std::byte> peek_view(std::size_t count) const;
        /// @}

        /// @name Unreal primitives
        /// @{

        /// @brief Reads a variable-length CompactIndex (1–5 bytes) regardless of version.
        std::int32_t read_compact_index();

        /// @brief Reads an "index": CompactIndex before version 178, int32 from version 178 on.
        std::int32_t read_index();

        /// @brief Reads a length-prefixed FString (see FString for the wire format).
        FString read_fstring();

        /// @brief Reads an FString and returns only its UTF-8 text.
        std::string read_string();

        /// @brief Reads a NUL-terminated Windows-1252 string (pre-version-64 names). The terminator is consumed.
        std::string read_ansi_null_string();

        /// @brief Reads an int32 element count and validates it.
        ///
        /// Throws if the count is negative, above SerializationLimits::max_array_count, or if
        /// `count * min_element_size` exceeds the remaining bytes.
        /// @param min_element_size Lower bound of the serialized size of one element.
        std::uint32_t read_array_count(std::size_t min_element_size = 1);
        /// @}

        /// @brief Creates a reader over `[offset, offset + count)` of this reader's data, sharing the context.
        [[nodiscard]] BinaryReader sub_reader(std::size_t offset, std::size_t count) const;

        /// @brief The underlying data.
        [[nodiscard]] std::span<const std::byte> data() const noexcept { return m_data; }

    private:
        const std::uint8_t* require(std::size_t count);
        [[noreturn]] void fail_out_of_bounds(std::size_t requested) const;

        std::span<const std::byte> m_data;
        std::size_t m_position = 0;
        std::uint64_t m_base_offset = 0;
        SerializationContext m_context;
    };
}
