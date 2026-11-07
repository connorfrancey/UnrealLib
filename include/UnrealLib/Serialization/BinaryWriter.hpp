#pragma once

/// @file
/// @brief Growable binary writer, the counterpart of BinaryReader.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <UnrealLib/Core/Guid.hpp>
#include <UnrealLib/Serialization/FString.hpp>
#include <UnrealLib/Serialization/SerializationContext.hpp>

namespace UnrealLib
{
    /// @brief Writes primitive and Unreal-specific values into an owned, growable buffer.
    ///
    /// Writes happen at the current position: they overwrite existing bytes and extend the buffer as needed.
    /// seek() may move anywhere in `[0, size()]`, which allows back-patching offsets once they are known.
    class BinaryWriter
    {
    public:
        /// @param context Version information used by version-dependent primitives.
        explicit BinaryWriter(SerializationContext context = {}) noexcept;

        /// @name Position
        /// @{
        [[nodiscard]] std::size_t size() const noexcept { return m_buffer.size(); }   ///< Bytes written.
        [[nodiscard]] std::size_t position() const noexcept { return m_position; }    ///< Current position.
        /// @brief Moves to @p position (0..size() inclusive). Throws SerializationError otherwise.
        void seek(std::size_t position);
        /// @}

        /// @name Context
        /// @{
        [[nodiscard]] const SerializationContext& context() const noexcept { return m_context; } ///< Current context.
        void set_context(const SerializationContext& context) noexcept { m_context = context; } ///< Replaces the context.
        /// @}

        /// @name Fixed-size primitives
        /// @{

        /// @brief Writes an unsigned 8-bit integer.
        void write_u8(std::uint8_t value);
        /// @brief Writes an unsigned 16-bit integer.
        void write_u16(std::uint16_t value);
        /// @brief Writes an unsigned 32-bit integer.
        void write_u32(std::uint32_t value);
        /// @brief Writes an unsigned 64-bit integer.
        void write_u64(std::uint64_t value);
        /// @brief Writes a signed 8-bit integer.
        void write_i8(std::int8_t value);
        /// @brief Writes a signed 16-bit integer.
        void write_i16(std::int16_t value);
        /// @brief Writes a signed 32-bit integer.
        void write_i32(std::int32_t value);
        /// @brief Writes a signed 64-bit integer.
        void write_i64(std::int64_t value);
        /// @brief Writes an IEEE-754 single-precision float.
        void write_f32(float value);
        /// @brief Writes an IEEE-754 double-precision float.
        void write_f64(double value);
        /// @brief Writes a 32-bit UBOOL (0 or 1).
        void write_bool32(bool value);
        /// @brief Writes an FGuid.
        void write_guid(const Guid& value);
        /// @brief Writes raw bytes.
        void write_bytes(std::span<const std::byte> bytes);
        /// @}

        /// @name Unreal primitives
        /// @{

        /// @brief Writes a CompactIndex regardless of version.
        void write_compact_index(std::int32_t value);
        /// @brief Writes an "index": CompactIndex before version 178, int32 from version 178 on.
        void write_index(std::int32_t value);
        /// @brief Writes an FString, honouring its encoding and terminator flag.
        ///
        /// If the string is marked StringEncoding::Ansi but contains characters not representable in Windows-1252,
        /// it is written as UTF-16 instead. Throws SerializationError if the text is not valid UTF-8.
        void write_fstring(const FString& value);
        /// @brief Writes a NUL-terminated Windows-1252 string (pre-version-64 names).
        void write_ansi_null_string(std::string_view text);
        /// @}

        /// @brief Overwrites 4 bytes at @p offset with @p value without moving the position.
        void patch_u32(std::size_t offset, std::uint32_t value);

        /// @brief Overwrites 4 bytes at @p offset with @p value without moving the position.
        void patch_i32(std::size_t offset, std::int32_t value);

        /// @brief The written bytes.
        [[nodiscard]] std::span<const std::byte> data() const noexcept { return m_buffer; }

        /// @brief Moves the buffer out of the writer, leaving it empty.
        [[nodiscard]] std::vector<std::byte> take() noexcept;

    private:
        std::uint8_t* reserve_at_position(std::size_t count);

        std::vector<std::byte> m_buffer;
        std::size_t m_position = 0;
        SerializationContext m_context;
    };
}
