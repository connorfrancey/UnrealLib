#include <UnrealLib/Core/Log.hpp>

#include <atomic>
#include <memory>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace UnrealLib
{
    namespace
    {
        std::atomic<LogLevel> g_level{LogLevel::Warning};

        // Function-local statics avoid static-initialization-order dependencies.
        LogSink& custom_sink()
        {
            static LogSink sink;
            return sink;
        }

        spdlog::logger& default_logger()
        {
            static const std::shared_ptr<spdlog::logger> logger = [] {
                auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
                auto result = std::make_shared<spdlog::logger>("UnrealLib", std::move(sink));
                result->set_level(spdlog::level::trace); // filtering happens in should_log()
                result->set_pattern("[%H:%M:%S.%e] [UnrealLib] [%^%l%$] %v");
                return result;
            }();
            return *logger;
        }

        spdlog::level::level_enum to_spdlog(LogLevel level) noexcept
        {
            switch (level)
            {
            case LogLevel::Trace: return spdlog::level::trace;
            case LogLevel::Debug: return spdlog::level::debug;
            case LogLevel::Info: return spdlog::level::info;
            case LogLevel::Warning: return spdlog::level::warn;
            case LogLevel::Error: return spdlog::level::err;
            case LogLevel::Off: return spdlog::level::off;
            }
            return spdlog::level::off;
        }
    }

    void set_log_level(LogLevel level) noexcept
    {
        g_level.store(level, std::memory_order_relaxed);
    }

    LogLevel log_level() noexcept
    {
        return g_level.load(std::memory_order_relaxed);
    }

    void set_log_sink(LogSink sink)
    {
        custom_sink() = std::move(sink);
    }

    void log_message(LogLevel level, std::string_view message)
    {
        if (!should_log(level))
        {
            return;
        }
        if (const auto& sink = custom_sink())
        {
            sink(level, message);
            return;
        }
        default_logger().log(to_spdlog(level), spdlog::string_view_t(message.data(), message.size()));
    }
}
