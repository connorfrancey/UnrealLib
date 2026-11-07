#pragma once

/// @file
/// @brief Text encoding conversions and case-insensitive helpers.
///
/// All text inside UnrealLib is stored as UTF-8. Two conversions are needed for Unreal data:
/// - "ANSI" strings use Windows-1252 (the same code page upstream UELib uses: UnrealEncoding.ANSI).
/// - "Unicode" strings use UTF-16LE. Unpaired surrogates are preserved using WTF-8 so that
///   decode → encode is lossless for every input.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UnrealLib::Text
{
    /// @brief Decodes Windows-1252 bytes to UTF-8. Every byte maps to a code point (undefined bytes 0x81, 0x8D,
    ///        0x8F, 0x90, 0x9D map to the identical C1 control code point), so this never fails.
    [[nodiscard]] std::string cp1252_to_utf8(std::span<const std::uint8_t> bytes);

    /// @brief Encodes UTF-8 to Windows-1252.
    /// @return std::nullopt if @p text is not valid UTF-8 or contains a code point not representable in Windows-1252.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> utf8_to_cp1252(std::string_view text);

    /// @brief Decodes UTF-16 code units to UTF-8 (WTF-8 for unpaired surrogates). Never fails.
    [[nodiscard]] std::string utf16_to_utf8(std::span<const char16_t> units);

    /// @brief Encodes UTF-8 (or WTF-8) to UTF-16 code units.
    /// @return std::nullopt if @p text is not valid UTF-8/WTF-8.
    [[nodiscard]] std::optional<std::u16string> utf8_to_utf16(std::string_view text);

    /// @brief ASCII-only lower-casing of a single character (Unreal name comparison semantics for ASCII names).
    [[nodiscard]] constexpr char ascii_to_lower(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /// @brief ASCII case-insensitive equality.
    [[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

    /// @brief Returns an ASCII-lower-cased copy of @p text.
    [[nodiscard]] std::string to_lower_ascii(std::string_view text);

    /// @brief Transparent ASCII case-insensitive hash, for use with std::unordered_map heterogeneous lookup.
    struct CaseInsensitiveHash
    {
        using is_transparent = void; ///< Enables heterogeneous lookup.
        /// @brief FNV-1a over the lower-cased characters.
        [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept;
    };

    /// @brief Transparent ASCII case-insensitive equality, companion of CaseInsensitiveHash.
    struct CaseInsensitiveEqual
    {
        using is_transparent = void; ///< Enables heterogeneous lookup.
        /// @brief See iequals().
        [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept { return iequals(a, b); }
    };
}
