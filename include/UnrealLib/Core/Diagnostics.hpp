#pragma once

/// @file
/// @brief Non-fatal diagnostics (warnings, validation findings) collected while processing data.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UnrealLib
{
    /// @brief Severity of a Diagnostic.
    enum class Severity : std::uint8_t
    {
        Info,    ///< Informational note.
        Warning, ///< Suspicious but recoverable condition.
        Error,   ///< Structural problem; results relying on the affected data are unreliable.
    };

    /// @brief Returns "info", "warning" or "error".
    [[nodiscard]] std::string_view to_string(Severity severity) noexcept;

    /// @brief A single diagnostic message.
    struct Diagnostic
    {
        Severity severity = Severity::Info;  ///< Severity.
        std::string code;                    ///< Stable machine-readable identifier, e.g. "export.serial-out-of-bounds".
        std::string message;                 ///< Human-readable description.
        std::optional<std::uint64_t> offset; ///< Related byte offset, if any.
    };

    /// @brief An ordered collection of diagnostics.
    class Diagnostics
    {
    public:
        /// @brief Appends a diagnostic.
        void add(Diagnostic diagnostic);

        /// @brief Appends an informational diagnostic.
        void info(std::string_view code, std::string_view message, std::optional<std::uint64_t> offset = std::nullopt);

        /// @brief Appends a warning.
        void warning(std::string_view code, std::string_view message, std::optional<std::uint64_t> offset = std::nullopt);

        /// @brief Appends an error.
        void error(std::string_view code, std::string_view message, std::optional<std::uint64_t> offset = std::nullopt);

        /// @brief Appends all diagnostics of @p other.
        void append(const Diagnostics& other);

        /// @brief All diagnostics in insertion order.
        [[nodiscard]] std::span<const Diagnostic> entries() const noexcept { return m_entries; }

        /// @brief Number of diagnostics with the given severity.
        [[nodiscard]] std::size_t count(Severity severity) const noexcept;

        /// @brief Whether any Severity::Error diagnostic is present.
        [[nodiscard]] bool has_errors() const noexcept { return count(Severity::Error) != 0; }

        /// @brief Whether no diagnostics are present.
        [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

        /// @brief Removes all diagnostics.
        void clear() noexcept { m_entries.clear(); }

    private:
        std::vector<Diagnostic> m_entries;
    };
}
