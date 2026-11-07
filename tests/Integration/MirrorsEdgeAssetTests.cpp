// Asset deserialization against real Mirror's Edge packages.
// Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set; the full corpus needs UNREALLIB_FIXTURE_SWEEP=1.
//
// Every asset export of a supported class is read; its native data must be consumed exactly. Payloads stored in the
// package are loaded (decompressed) and decoded: the largest stored mip of every texture, every sound's container.
//
// UNREALLIB_ASSET_DUMP=<directory> writes a sample of decoded assets (game-derived: never commit them).

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <UnrealLib/Assets/Sound.hpp>
#include <UnrealLib/Assets/Texture.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>

#include "Support/FixtureSupport.hpp"

using namespace UnrealLib;
using namespace UnrealLib::Assets;
using namespace UnrealLib::TestSupport;
namespace fs = std::filesystem;

namespace
{
    struct AssetTally
    {
        std::map<std::string, std::size_t> read;      // class -> exports read
        std::map<std::string, std::size_t> failures;  // "class: message" -> count
        std::map<std::string, std::size_t> formats;   // pixel format -> textures
        std::map<std::string, std::size_t> mip_flags; // bulk flags of mips -> count
        std::map<std::string, std::size_t> containers; // audio container -> sounds
        std::map<std::string, std::size_t> sound_flags;
        std::size_t textures_decoded = 0;
        std::size_t textures_without_payload = 0;
        std::size_t texture_decode_failures = 0;
        std::size_t sounds_extracted = 0;
        std::size_t sounds_without_payload = 0;
        std::size_t dumped = 0;
        std::size_t total_failures = 0;
        std::size_t offset_mismatches = 0;
        std::size_t separate_mips_decoded = 0;

        void fail(const std::string& cls, const std::string& path, const std::string& message)
        {
            ++total_failures;
            const std::string key = cls + ": " + message.substr(0, 90);
            if (failures[key]++ == 0)
            {
                std::cout << "    first failure [" << key << "] at " << path << '\n';
            }
        }
    };

    std::string flags_key(const BulkData& b)
    {
        return b.describe_flags() + (b.element_count == 0 ? " (empty)" : "");
    }

    void check_assets(Linker& linker, const Package& package, AssetTally& tally)
    {
        const std::string dump = env("UNREALLIB_ASSET_DUMP");
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ObjectRef ref{&package, ObjectIndex::from_export(i)};
            if (package.exports()[i].serial_size <= 0 ||
                (package.exports()[i].object_flags & ObjectFlag::ClassDefaultObject) != 0)
            {
                continue;
            }
            const std::string cls = package.class_name(ref.index);
            try
            {
                if (is_texture2d_class(cls))
                {
                    const Texture2D texture = read_texture2d(linker, ref);
                    ++tally.read[cls];
                    ++tally.formats[texture.format_name];
                    for (const auto& mip : texture.mips)
                    {
                        ++tally.mip_flags[flags_key(mip.data)];
                        tally.offset_mismatches += mip.data.offset_mismatch() ? 1 : 0;
                    }
                    const auto first = texture.first_mip_with_payload();
                    if (!first)
                    {
                        ++tally.textures_without_payload;
                        continue;
                    }
                    if (!can_decode(texture.format))
                    {
                        continue;
                    }
                    try
                    {
                        const Image image = decode_mip(package, texture, *first, &linker);
                        tally.separate_mips_decoded += texture.mips[*first].data.is_inline() ? 0 : 1;
                        ++tally.textures_decoded;
                        if (!dump.empty() && tally.dumped < 200 && tally.textures_decoded % 97 == 0)
                        {
                            ++tally.dumped;
                            image.save_png(fs::path(dump) / "textures" / (package.name() + "." + texture.name + ".png"));
                        }
                    }
                    catch (const Error& e)
                    {
                        ++tally.texture_decode_failures;
                        tally.fail(cls + " decode", linker.qualified_path(ref), e.what());
                    }
                }
                else if (cls == "TextureCube")
                {
                    (void)read_texture_cube(linker, ref);
                    ++tally.read[cls];
                }
                else if (cls == "SoundNodeWave")
                {
                    const SoundWave sound = read_sound_wave(linker, ref);
                    ++tally.read[cls];
                    for (const BulkData* b : {&sound.raw_data, &sound.compressed_pc, &sound.compressed_xbox360,
                                              &sound.compressed_ps3})
                    {
                        ++tally.sound_flags[flags_key(*b)];
                    }
                    const auto extracted = extract_sound(package, sound);
                    if (!extracted)
                    {
                        ++tally.sounds_without_payload;
                        continue;
                    }
                    ++tally.sounds_extracted;
                    ++tally.containers[std::string(file_extension(extracted->container))];
                }
            }
            catch (const Error& e)
            {
                tally.fail(cls, linker.qualified_path(ref), e.what());
            }
        }
    }

    void print_map(const char* title, const std::map<std::string, std::size_t>& map)
    {
        std::cout << "  " << title << ":\n";
        std::vector<std::pair<std::size_t, std::string>> rows;
        for (const auto& [k, v] : map) rows.emplace_back(v, k);
        std::sort(rows.rbegin(), rows.rend());
        for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 20); ++i)
        {
            std::cout << "    " << rows[i].first << "x " << rows[i].second << '\n';
        }
    }

    void report(const AssetTally& tally)
    {
        print_map("read", tally.read);
        print_map("failures", tally.failures);
        print_map("pixel formats", tally.formats);
        print_map("mip bulk flags", tally.mip_flags);
        print_map("sound bulk flags", tally.sound_flags);
        print_map("audio containers", tally.containers);
        std::cout << "  textures decoded: " << tally.textures_decoded
                  << ", without stored mips: " << tally.textures_without_payload
                  << ", decode failures: " << tally.texture_decode_failures
                  << ", largest mip from another package: " << tally.separate_mips_decoded << '\n'
                  << "  sounds extracted: " << tally.sounds_extracted
                  << ", without payload: " << tally.sounds_without_payload << '\n'
                  << "  inline bulk data with a stale stored offset: " << tally.offset_mismatches << '\n';
    }
}

TEST(MirrorsEdgeAssetTests, SampleAssets)
{
    // A quick subset: up to 40 character (CH_), voice (A_V) and UI (TdU) packages.
    if (!fixtures_available())
    {
        GTEST_SKIP() << "UNREALLIB_MIRRORSEDGE_DIR is not set";
    }
    AssetTally tally;
    std::size_t count = 0;
    for (const auto& file : packages(true))
    {
        const std::string name = file.stem().string();
        if (!(name.starts_with("CH_") || name.starts_with("A_V") || name.starts_with("TdU")) || count >= 40)
        {
            continue;
        }
        ++count;
        Linker linker;
        linker.add_search_path(fixture_dir());
        check_assets(linker, linker.load(file), tally);
    }
    report(tally);
    EXPECT_GT(count, 0u);
    EXPECT_EQ(tally.total_failures, 0u);
}

TEST(MirrorsEdgeAssetTests, FullCorpusAssets)
{
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    AssetTally tally;
    for (const auto& file : packages(true))
    {
        Linker linker;
        linker.add_search_path(fixture_dir());
        check_assets(linker, linker.load(file), tally);
    }
    report(tally);
    EXPECT_EQ(tally.total_failures, 0u);
}

