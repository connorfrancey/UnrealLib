#pragma once

/// @file
/// @brief Minimal logging facade. The default backend is spdlog (stderr); consumers may install their own sink.

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace UnrealLib
{
    /// @brief Log severity levels, ordered from most to least verbose.
    enum class LogLevel : std::uint8_t
    {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Off,
    };

    /// @brief A user-supplied log sink. Receives already-formatted messages.
    using LogSink = std::function<void(LogLevel level, std::string_view message)>;

    /// @brief Sets the minimum level that is emitted. Default: LogLevel::Warning.
    void set_log_level(LogLevel level) noexcept;

    /// @brief Returns the current minimum log level.
    [[nodiscard]] LogLevel log_level() noexcept;

    /// @brief Replaces the log backend. Pass an empty function to restore the default spdlog backend.
    /// @note Not thread-safe with respect to concurrent logging; install the sink during start-up.
    void set_log_sink(LogSink sink);

    /// @brief Whether a message at @p level would be emitted.
    [[nodiscard]] inline bool should_log(LogLevel level) noexcept
    {
        return level != LogLevel::Off && level >= log_level();
    }

    /// @brief Emits an already-formatted message (no level filtering beyond should_log()).
    void log_message(LogLevel level, std::string_view message);

    /// @brief Formats and emits a message at @p level if enabled.
    template <typename... Args>
    void log(LogLevel level, fmt::format_string<Args...> format, Args&&... args)
    {
        if (should_log(level))
        {
            log_message(level, fmt::format(format, std::forward<Args>(args)...));
        }
    }
}
