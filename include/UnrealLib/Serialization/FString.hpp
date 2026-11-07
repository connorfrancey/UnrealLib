#pragma once

/// @file
/// @brief Unreal FString with enough metadata to re-serialize it byte-for-byte.

#include <cstdint>
#include <string>
#include <utility>

namespace UnrealLib
{
    /// @brief On-disk character encoding of an FString.
    enum class StringEncoding : std::uint8_t
    {
        Ansi,  ///< Positive length prefix; Windows-1252 bytes.
        Utf16, ///< Negative length prefix; UTF-16LE (or BE in big-endian archives) code units.
    };

    /// @brief A serialized Unreal string.
    ///
    /// Wire format: `index length` (CompactIndex before version 178, int32 afterwards), followed by `|length|`
    /// characters. A positive length means Windows-1252 bytes, a negative length UTF-16 code units, zero means an
    /// empty string with no payload. The final character is normally a NUL terminator.
    struct FString
    {
        std::string text;                         ///< Content as UTF-8 (WTF-8 for unpaired UTF-16 surrogates), without terminator.
        StringEncoding encoding = StringEncoding::Ansi; ///< Encoding used on disk.
        bool null_terminated = true;              ///< Whether the serialized payload ended with a NUL character.

        FString() = default;

        /// @brief Creates an ANSI, NUL-terminated string (the common case). The writer automatically switches to
        ///        UTF-16 if @p value cannot be represented in Windows-1252.
        FString(std::string value) : text(std::move(value)) {} // NOLINT(google-explicit-constructor)

        /// @brief Creates an ANSI, NUL-terminated string from a C string.
        FString(const char* value) : text(value) {} // NOLINT(google-explicit-constructor)

        /// @brief Creates a string with explicit on-disk properties.
        FString(std::string value, StringEncoding enc, bool terminated)
            : text(std::move(value)), encoding(enc), null_terminated(terminated)
        {
        }

        /// @brief Memberwise comparison.
        friend bool operator==(const FString&, const FString&) = default;
    };
}
