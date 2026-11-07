#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <UnrealLib/Core/Diagnostics.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Core/Guid.hpp>
#include <UnrealLib/Core/Log.hpp>
#include <UnrealLib/Core/Version.hpp>

using namespace UnrealLib;

TEST(ErrorTests, MessageIncludesOffset)
{
    const SerializationError error(ErrorCode::OutOfBounds, "bad read", 0x2A);
    EXPECT_EQ(error.code(), ErrorCode::OutOfBounds);
    EXPECT_EQ(error.message(), "bad read");
    EXPECT_STREQ(error.what(), "bad read (at offset 0x2A)");
    ASSERT_TRUE(error.offset().has_value());
    EXPECT_EQ(*error.offset(), 0x2Au);
}

TEST(ErrorTests, HierarchyIsCatchableAsError)
{
    EXPECT_THROW(throw CompressionError("x"), Error);
    EXPECT_THROW(throw PackageError("x"), Error);
    EXPECT_THROW(throw IoError("x"), std::runtime_error);
    EXPECT_EQ(UnsupportedError("x").code(), ErrorCode::Unsupported);
    EXPECT_EQ(to_string(ErrorCode::LimitExceeded), "limit-exceeded");
}

TEST(DiagnosticsTests, CountsBySeverity)
{
    Diagnostics diagnostics;
    EXPECT_TRUE(diagnostics.empty());
    diagnostics.info("a", "info");
    diagnostics.warning("b", "warning", 12);
    diagnostics.error("c", "error");
    EXPECT_EQ(diagnostics.count(Severity::Warning), 1u);
    EXPECT_TRUE(diagnostics.has_errors());
    EXPECT_EQ(diagnostics.entries()[1].offset, std::optional<std::uint64_t>(12));

    Diagnostics other;
    other.error("d", "error");
    diagnostics.append(other);
    EXPECT_EQ(diagnostics.count(Severity::Error), 2u);
    EXPECT_EQ(to_string(Severity::Warning), "warning");
}

TEST(GuidTests, FormatsLikeUnreal)
{
    const Guid guid{0x06C691FD, 0x417B4677, 0xBC776381, 0x583E4EE5};
    EXPECT_EQ(guid.to_string(), "06C691FD417B4677BC776381583E4EE5");
    EXPECT_FALSE(guid.is_zero());
    EXPECT_TRUE(Guid{}.is_zero());
}

TEST(LogTests, CustomSinkAndLevelFiltering)
{
    std::vector<std::string> messages;
    set_log_sink([&](LogLevel, std::string_view message) { messages.emplace_back(message); });
    set_log_level(LogLevel::Info);
    log(LogLevel::Debug, "hidden {}", 1);
    log(LogLevel::Info, "shown {}", 2);
    set_log_level(LogLevel::Warning);
    set_log_sink({});
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0], "shown 2");
}

TEST(VersionTests, LibraryVersionIsConsistent)
{
    const std::string expected = std::to_string(library_version.major) + "." +
                                 std::to_string(library_version.minor) + "." +
                                 std::to_string(library_version.patch);
    EXPECT_EQ(library_version_string, expected);
}
