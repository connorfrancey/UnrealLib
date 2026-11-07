#include <UnrealLib/Core/Diagnostics.hpp>

#include <algorithm>
#include <utility>

namespace UnrealLib
{
    std::string_view to_string(Severity severity) noexcept
    {
        switch (severity)
        {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
        }
        return "unknown";
    }

    void Diagnostics::add(Diagnostic diagnostic)
    {
        m_entries.push_back(std::move(diagnostic));
    }

    void Diagnostics::info(std::string_view code, std::string_view message, std::optional<std::uint64_t> offset)
    {
        add({Severity::Info, std::string(code), std::string(message), offset});
    }

    void Diagnostics::warning(std::string_view code, std::string_view message, std::optional<std::uint64_t> offset)
    {
        add({Severity::Warning, std::string(code), std::string(message), offset});
    }

    void Diagnostics::error(std::string_view code, std::string_view message, std::optional<std::uint64_t> offset)
    {
        add({Severity::Error, std::string(code), std::string(message), offset});
    }

    void Diagnostics::append(const Diagnostics& other)
    {
        m_entries.insert(m_entries.end(), other.m_entries.begin(), other.m_entries.end());
    }

    std::size_t Diagnostics::count(Severity severity) const noexcept
    {
        return static_cast<std::size_t>(std::count_if(m_entries.begin(), m_entries.end(),
            [severity](const Diagnostic& d) { return d.severity == severity; }));
    }
}
