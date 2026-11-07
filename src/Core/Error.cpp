#include <UnrealLib/Core/Error.hpp>

#include <fmt/format.h>

namespace UnrealLib
{
    namespace
    {
        std::string compose_message(std::string_view message, const std::optional<std::uint64_t>& offset)
        {
            if (offset)
            {
                return fmt::format("{} (at offset 0x{:X})", message, *offset);
            }
            return std::string(message);
        }
    }

    std::string_view to_string(ErrorCode code) noexcept
    {
        switch (code)
        {
        case ErrorCode::Unknown: return "unknown";
        case ErrorCode::OutOfBounds: return "out-of-bounds";
        case ErrorCode::InvalidData: return "invalid-data";
        case ErrorCode::LimitExceeded: return "limit-exceeded";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::Io: return "io";
        case ErrorCode::Compression: return "compression";
        case ErrorCode::InvalidArgument: return "invalid-argument";
        case ErrorCode::InvalidState: return "invalid-state";
        case ErrorCode::NotFound: return "not-found";
        }
        return "unknown";
    }

    Error::Error(ErrorCode code, std::string_view message, std::optional<std::uint64_t> offset)
        : std::runtime_error(compose_message(message, offset)), m_code(code), m_offset(offset), m_message(message)
    {
    }
}
