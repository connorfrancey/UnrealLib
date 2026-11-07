#include <gtest/gtest.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Package/PackageSummary.hpp>

using namespace UnrealLib;

namespace
{
    PackageSummary summary(std::uint16_t version, std::uint16_t licensee, std::int32_t engine, std::int32_t cooker)
    {
        PackageSummary s;
        s.file_version = version;
        s.licensee_version = licensee;
        s.engine_version = engine;
        s.cooker_version = cooker;
        return s;
    }
}

TEST(GameProfileTests, BuiltinRegistryContainsMirrorsEdge)
{
    const auto& registry = GameProfileRegistry::builtin();
    const GameProfile* me = registry.find("Mirror's Edge PC");
    ASSERT_NE(me, nullptr);
    EXPECT_EQ(me->id, "mirrorsedge-pc");
    EXPECT_EQ(registry.find("MIRRORSEDGE-PC"), me);
    EXPECT_TRUE(me->verified);
    EXPECT_EQ(me->min_file_version, 536);
    EXPECT_EQ(me->licensee_version, std::optional<std::uint16_t>(43));
    EXPECT_EQ(me->engine_version, std::optional<std::int32_t>(3716));
    EXPECT_EQ(registry.find("unknown"), nullptr);
}

TEST(GameProfileTests, DetectsMirrorsEdge)
{
    const auto detection = GameProfileRegistry::builtin().detect(summary(536, 43, 3716, 60));
    ASSERT_NE(detection.profile, nullptr);
    EXPECT_EQ(detection.profile->id, "mirrorsedge-pc");
    EXPECT_FALSE(detection.reasons.empty());
}

TEST(GameProfileTests, LicenseeMismatchFallsBackToGeneric)
{
    // e.g. a modded Mirror's Edge package saved with licensee 0
    const auto detection = GameProfileRegistry::builtin().detect(summary(536, 0, 3716, 0));
    ASSERT_NE(detection.profile, nullptr);
    EXPECT_EQ(detection.profile->id, "ue3-generic");
}

TEST(GameProfileTests, VersionAloneDoesNotSelectMirrorsEdge)
{
    // Another game sharing version 536 with a different licensee must not be classified as Mirror's Edge.
    const auto detection = GameProfileRegistry::builtin().detect(summary(536, 7, 4000, 10));
    ASSERT_NE(detection.profile, nullptr);
    EXPECT_NE(detection.profile->id, "mirrorsedge-pc");
}

TEST(GameProfileTests, BigEndianHasNoBuiltinProfile)
{
    PackageSummary s = summary(536, 43, 3716, 60);
    s.endian = Endian::Big;
    EXPECT_EQ(GameProfileRegistry::builtin().detect(s).profile, nullptr);
}

TEST(GameProfileTests, CustomRegistryAndDuplicateIds)
{
    GameProfileRegistry registry;
    GameProfile p = make_generic_ue3_profile();
    registry.add(p);
    EXPECT_THROW(registry.add(p), Error);

    GameProfile special;
    special.id = "special";
    special.name = "Special Game";
    special.min_file_version = 536;
    special.max_file_version = 536;
    special.licensee_version = std::uint16_t{7};
    registry.add(special);
    const auto detection = registry.detect(summary(536, 7, 0, 0));
    ASSERT_NE(detection.profile, nullptr);
    EXPECT_EQ(detection.profile->id, "special");
}
