#include <UnrealLib/Utilities/Text.hpp>

#include <array>

namespace UnrealLib::Text
{
    namespace
    {
        // Windows-1252 bytes 0x80..0x9F. Undefined positions map to the C1 control code point of the same value,
        // which is what MultiByteToWideChar(1252) does and keeps the mapping bijective.
        constexpr std::array<char32_t, 32> k_cp1252_high{
            0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
            0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
        };

        void append_utf8(std::string& out, char32_t cp)
        {
            if (cp < 0x80)
            {
                out.push_back(static_cast<char>(cp));
            }
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        // Decodes one UTF-8/WTF-8 sequence. Surrogate code points are accepted (WTF-8); overlong forms are rejected.
        bool next_code_point(std::string_view text, std::size_t& i, char32_t& cp)
        {
            const auto b0 = static_cast<std::uint8_t>(text[i]);
            std::size_t length = 0;
            char32_t min_value = 0;
            if (b0 < 0x80)
            {
                cp = b0;
                ++i;
                return true;
            }
            if ((b0 & 0xE0) == 0xC0)
            {
                length = 2;
                cp = b0 & 0x1Fu;
                min_value = 0x80;
            }
            else if ((b0 & 0xF0) == 0xE0)
            {
                length = 3;
                cp = b0 & 0x0Fu;
                min_value = 0x800;
            }
            else if ((b0 & 0xF8) == 0xF0)
            {
                length = 4;
                cp = b0 & 0x07u;
                min_value = 0x10000;
            }
            else
            {
                return false;
            }
            if (text.size() - i < length)
            {
                return false;
            }
            for (std::size_t k = 1; k < length; ++k)
            {
                const auto b = static_cast<std::uint8_t>(text[i + k]);
                if ((b & 0xC0) != 0x80)
                {
                    return false;
                }
                cp = (cp << 6) | (b & 0x3Fu);
            }
            if (cp < min_value || cp > 0x10FFFF)
            {
                return false;
            }
            i += length;
            return true;
        }
    }

    std::string cp1252_to_utf8(std::span<const std::uint8_t> bytes)
    {
        std::string out;
        out.reserve(bytes.size());
        for (const std::uint8_t b : bytes)
        {
            const char32_t cp = (b >= 0x80 && b <= 0x9F) ? k_cp1252_high[b - 0x80u] : char32_t{b};
            append_utf8(out, cp);
        }
        return out;
    }

    std::optional<std::vector<std::uint8_t>> utf8_to_cp1252(std::string_view text)
    {
        std::vector<std::uint8_t> out;
        out.reserve(text.size());
        std::size_t i = 0;
        while (i < text.size())
        {
            char32_t cp = 0;
            if (!next_code_point(text, i, cp))
            {
                return std::nullopt;
            }
            if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF))
            {
                out.push_back(static_cast<std::uint8_t>(cp));
                continue;
            }
            bool found = false;
            for (std::size_t k = 0; k < k_cp1252_high.size(); ++k)
            {
                if (k_cp1252_high[k] == cp)
                {
                    out.push_back(static_cast<std::uint8_t>(0x80 + k));
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return std::nullopt;
            }
        }
        return out;
    }

    std::string utf16_to_utf8(std::span<const char16_t> units)
    {
        std::string out;
        out.reserve(units.size());
        for (std::size_t i = 0; i < units.size(); ++i)
        {
            const char32_t u = units[i];
            if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units.size())
            {
                const char32_t low = units[i + 1];
                if (low >= 0xDC00 && low <= 0xDFFF)
                {
                    append_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00));
                    ++i;
                    continue;
                }
            }
            append_utf8(out, u); // BMP character or unpaired surrogate (WTF-8)
        }
        return out;
    }

    std::optional<std::u16string> utf8_to_utf16(std::string_view text)
    {
        std::u16string out;
        out.reserve(text.size());
        std::size_t i = 0;
        while (i < text.size())
        {
            char32_t cp = 0;
            if (!next_code_point(text, i, cp))
            {
                return std::nullopt;
            }
            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
            }
            else
            {
                out.push_back(static_cast<char16_t>(cp));
            }
        }
        return out;
    }

    bool iequals(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (ascii_to_lower(a[i]) != ascii_to_lower(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    std::string to_lower_ascii(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
        {
            c = ascii_to_lower(c);
        }
        return out;
    }

    std::size_t CaseInsensitiveHash::operator()(std::string_view text) const noexcept
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const char c : text)
        {
            hash ^= static_cast<std::uint8_t>(ascii_to_lower(c));
            hash *= 1099511628211ull;
        }
        return static_cast<std::size_t>(hash);
    }
}
