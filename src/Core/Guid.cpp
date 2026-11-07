#include <UnrealLib/Core/Guid.hpp>

#include <fmt/format.h>

namespace UnrealLib
{
    std::string Guid::to_string() const
    {
        return fmt::format("{:08X}{:08X}{:08X}{:08X}", a, b, c, d);
    }
}
