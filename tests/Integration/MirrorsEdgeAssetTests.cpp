// Asset deserialization against real Mirror's Edge packages.
// Skipped unless UNREALLIB_MIRRORSEDGE_DIR is set; the full corpus needs UNREALLIB_FIXTURE_SWEEP=1.
//
// Every asset export of a supported class is read; its native data must be consumed exactly. Payloads stored in the
// package are loaded (decompressed) and decoded: the largest stored mip of every texture, every sound's container.
//
// UNREALLIB_ASSET_DUMP=<directory> writes a sample of decoded assets (game-derived: never commit them).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <UnrealLib/Assets/Animation.hpp>
#include <UnrealLib/Assets/Brush.hpp>
#include <UnrealLib/Assets/Image.hpp>
#include <UnrealLib/Assets/Material.hpp>
#include <UnrealLib/Assets/SkeletalMesh.hpp>
#include <UnrealLib/Assets/Sound.hpp>
#include <UnrealLib/Assets/StaticMesh.hpp>
#include <UnrealLib/Assets/Texture.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
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
        std::size_t mesh_vertices = 0;
        std::size_t mesh_triangles = 0;
        std::size_t meshes_dumped = 0;
        std::size_t skeletal_bones = 0;
        std::size_t skeletal_vertices = 0;
        std::size_t skeletal_dumped = 0;
        std::size_t anim_keys = 0;
        std::map<std::string, std::size_t> anim_formats;
        std::size_t anim_raw_tracks_compared = 0;
        std::map<std::string, std::size_t> anim_set_bones;
        std::map<std::string, std::size_t> uv_sets;
        std::size_t polygons = 0;
        std::size_t model_surfaces = 0;
        std::size_t models_with_polys = 0;

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

    void check_assets(Linker& linker, const Package& package, AssetTally& tally, bool brushes_only = false)
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
            if (brushes_only && cls != "Polys" && cls != "Model") continue;
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
                else if (cls == "StaticMesh")
                {
                    const StaticMesh mesh = read_static_mesh(linker, ref);
                    ++tally.read[cls];
                    for (const auto& lod : mesh.lods)
                    {
                        tally.mesh_vertices += lod.positions.size();
                        // Independent check of the position stream: every LOD 0 vertex lies in the stored bounds
                        // (computed from LOD 0; 72 lower LODs exceed them by up to 2x in the real data).
                        const auto& b = mesh.bounds;
                        const float slack = 0.01f * std::max({b.box_extent.x, b.box_extent.y, b.box_extent.z, 1.0f});
                        for (const Vec3& p : lod.positions)
                        {
                            if (&lod != mesh.lods.data()) break;
                            if (std::abs(p.x - b.origin.x) > b.box_extent.x + slack ||
                                std::abs(p.y - b.origin.y) > b.box_extent.y + slack ||
                                std::abs(p.z - b.origin.z) > b.box_extent.z + slack)
                            {
                                tally.fail(cls, linker.qualified_path(ref), "vertex outside the bounds");
                                break;
                            }
                        }
                        tally.mesh_triangles += lod.indices.size() / 3;
                        ++tally.uv_sets[std::to_string(lod.num_uv_sets) + (lod.full_precision_uvs ? " float" : " half")];
                    }
                    if (!dump.empty() && tally.meshes_dumped < 20 && tally.read[cls] % 1000 == 0 && !mesh.lods.empty())
                    {
                        ++tally.meshes_dumped;
                        const std::string obj = to_obj(package, mesh);
                        write_file(fs::path(dump) / "meshes" / (package.name() + "." + mesh.name + ".obj"),
                                   std::as_bytes(std::span(obj)));
                    }
                }
                else if (cls == "SkeletalMesh")
                {
                    const SkeletalMesh mesh = read_skeletal_mesh(linker, ref);
                    ++tally.read[cls];
                    tally.skeletal_bones += mesh.skeleton.size();
                    for (const auto& lod : mesh.lods)
                    {
                        tally.skeletal_vertices += lod.vertices.size();
                        ++tally.uv_sets["skeletal " + std::to_string(lod.num_uv_sets) +
                                        (lod.full_precision_uvs ? " float" : " half")];
                        // Every vertex's weights sum to 255.
                        for (const auto& v : lod.vertices)
                        {
                            if (v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3] != 255)
                            {
                                tally.fail(cls, linker.qualified_path(ref), "weights do not sum to 255");
                                break;
                            }
                        }
                    }
                    if (!dump.empty() && tally.skeletal_dumped < 10 && !mesh.lods.empty() && tally.read[cls] % 50 == 1)
                    {
                        ++tally.skeletal_dumped;
                        write_file(fs::path(dump) / "skeletal" / (package.name() + "." + mesh.name + ".psk"),
                                   to_psk(package, mesh));
                    }
                }
                else if (cls == "AnimSequence")
                {
                    const AnimSequence seq = read_anim_sequence(linker, ref);
                    ++tally.read[cls];
                    ++tally.anim_formats["key format " + std::to_string(static_cast<int>(seq.key_format)) +
                                         ", rotation codec " + std::to_string(static_cast<int>(seq.rotation_format))];
                    tally.anim_keys += seq.num_frames;
                    // The decoded tracks must cover the byte stream exactly, and every rotation be a unit quaternion.
                    if (seq.stream_bytes_used != seq.stream.size())
                    {
                        tally.fail(cls, linker.qualified_path(ref),
                                   "tracks cover " + std::to_string(seq.stream_bytes_used) + " of " +
                                       std::to_string(seq.stream.size()) + " bytes");
                    }
                    // Where the package kept RawAnimData, frame 0 of every compressed track must match it.
                    if (seq.raw_tracks.size() == seq.tracks.size())
                    {
                        for (std::size_t t = 0; t < seq.tracks.size(); ++t)
                        {
                            const AnimTrack& raw = seq.raw_tracks[t];
                            if (raw.positions.empty() || raw.rotations.empty()) continue;
                            Vec3 p;
                            Quat q;
                            seq.tracks[t].sample(0, seq.num_frames, p, q);
                            const Vec3& rp = raw.positions[0];
                            const Quat& rq = raw.rotations[0];
                            const float dot = std::abs(q.x * rq.x + q.y * rq.y + q.z * rq.z + q.w * rq.w);
                            const float dist = std::sqrt((p.x - rp.x) * (p.x - rp.x) + (p.y - rp.y) * (p.y - rp.y) + (p.z - rp.z) * (p.z - rp.z));
                            ++tally.anim_raw_tracks_compared;
                            if (dot < 0.999f || dist > 1.0f) // key reduction may drift ~0.1-0.3 units from the raw data
                            {
                                tally.fail(cls, linker.qualified_path(ref),
                                           "frame 0 differs from RawAnimData (rotation dot " + std::to_string(dot) +
                                               ", position distance " + std::to_string(dist) + ")");
                                break;
                            }
                        }
                    }
                    for (const auto& track : seq.tracks)
                    {
                        for (const auto& q : track.rotations)
                        {
                            const float len = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                            if (std::abs(len - 1.0f) > 0.01f)
                            {
                                tally.fail(cls, linker.qualified_path(ref), "non-unit rotation");
                                break;
                            }
                        }
                    }
                }
                else if (cls == "AnimSet" || cls == "TdAnimSet")
                {
                    const AnimSet set = read_anim_set(linker, ref);
                    ++tally.read[cls];
                    tally.anim_set_bones[set.track_bone_names.empty() ? "empty" : "with bones"]++;
                    // Every sequence of the set has one track per bone.
                    for (const ObjectIndex s : set.sequences)
                    {
                        if (!s.is_export()) continue;
                        const AnimSequence seq = read_anim_sequence(linker, ObjectRef{&package, s});
                        if (seq.tracks.size() != set.track_bone_names.size())
                        {
                            tally.fail(cls, linker.qualified_path(ref), "sequence track count differs from bone count");
                            break;
                        }
                    }
                }
                else if (cls == "Polys")
                {
                    // read_polys() checks that the data is consumed exactly; every polygon is planar-ish data with
                    // at least 3 vertices and a unit normal, and the owner is the Polys' outer Model when it has one.
                    const Polys polys = read_polys(linker, ref);
                    ++tally.read[cls];
                    tally.polygons += polys.elements.size();
                    const ObjectIndex outer = package.exports()[i].outer;
                    // The owner is the Polys itself in ~86% of the game's Polys (e.g. i_Maintenance prefabs).
                    if (!outer.is_null() && package.class_name(outer) == "Model" && polys.element_owner != outer &&
                        polys.element_owner != ref.index)
                    {
                        tally.fail(cls, linker.qualified_path(ref), "ElementOwner is not the outer Model");
                    }
                    // write_polys() must reproduce the native bytes exactly.
                    const auto make_name = [&](std::string_view text) {
                        const std::size_t u = text.rfind('_');
                        if (u != std::string_view::npos && u + 1 < text.size() && text[u + 1] != '0' &&
                            text.find_first_not_of("0123456789", u + 1) == std::string_view::npos)
                        {
                            if (const auto base = package.names().find(text.substr(0, u)))
                            {
                                return FName{*base, std::stoi(std::string(text.substr(u + 1))) + 1};
                            }
                        }
                        return FName{package.names().find(text).value_or(-1), 0};
                    };
                    const std::vector<std::byte> written =
                        write_polys(polys, package.context(), make_name);
                    const auto data = package.export_data(i);
                    if (polys.native_offset > data.size() ||
                        !std::ranges::equal(written, data.subspan(polys.native_offset)))
                    {
                        tally.fail(cls, linker.qualified_path(ref), "write_polys does not reproduce the data");
                    }
                    for (const Poly& p : polys.elements)
                    {
                        const float len = p.normal.x * p.normal.x + p.normal.y * p.normal.y + p.normal.z * p.normal.z;
                        if (p.vertices.size() < 3 || std::abs(len - 1.0f) > 0.01f)
                        {
                            tally.fail(cls, linker.qualified_path(ref), "degenerate polygon");
                            break;
                        }
                    }
                }
                else if (cls == "Model")
                {
                    // The decoded prefix must end in a Polys reference (or null), and surfaces must index the
                    // decoded vectors and points.
                    const Model model = read_model(linker, ref);
                    ++tally.read[cls];
                    tally.model_surfaces += model.surfaces.size();
                    if (model.surface_owner != ref.index)
                    {
                        tally.fail(cls, linker.qualified_path(ref), "surface owner is not the model");
                    }
                    if (!model.polys.is_null())
                    {
                        ++tally.models_with_polys;
                        if (!package.is_valid(model.polys) || package.class_name(model.polys) != "Polys")
                        {
                            tally.fail(cls, linker.qualified_path(ref), "Polys reference is not a Polys");
                        }
                    }
                    for (const BspSurface& s : model.surfaces)
                    {
                        const auto in = [](std::int32_t i, std::size_t n) { return i >= 0 && static_cast<std::size_t>(i) < n; };
                        if (!in(s.base, model.points.size()) || !in(s.normal, model.vectors.size()) ||
                            !in(s.texture_u, model.vectors.size()) || !in(s.texture_v, model.vectors.size()))
                        {
                            tally.fail(cls, linker.qualified_path(ref), "surface index out of range");
                            break;
                        }
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
                                              &sound.compressed_ps3, &sound.compressed_wiiu,
                                              &sound.compressed_iphone, &sound.compressed_flash})
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
        print_map("static mesh UV sets", tally.uv_sets);
        print_map("anim sets", tally.anim_set_bones);
        print_map("anim formats", tally.anim_formats);
        std::cout << "  static mesh vertices: " << tally.mesh_vertices << ", triangles: " << tally.mesh_triangles << '\n'
                  << "  brush polygons: " << tally.polygons << ", BSP surfaces: " << tally.model_surfaces
                  << ", models with Polys: " << tally.models_with_polys << '\n'
                  << "  anim tracks checked against RawAnimData: " << tally.anim_raw_tracks_compared << '\n'
                  << "  skeletal mesh bones: " << tally.skeletal_bones << ", vertices: " << tally.skeletal_vertices
                  << '\n';
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



TEST(MirrorsEdgeAssetTests, FullCorpusMaterials)
{
    // Every material's metadata, with a ReflectionSchema so parameter arrays decode. Batches share one Linker and
    // schema (see MirrorsEdgePropertyTests.FullCorpusPropertiesWithReflectionSchema).
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    constexpr std::size_t batch = 32;
    const auto files = packages(true);
    std::map<std::string, std::size_t> classes;
    std::size_t parameters = 0, textures = 0, expressions = 0, failures = 0, undecoded = 0, unnamed = 0;
    for (std::size_t first = 0; first < files.size(); first += batch)
    {
        Linker linker;
        linker.add_search_path(fixture_dir());
        ReflectionSchema schema(linker);
        for (const Package* script : load_script_packages(linker)) schema.add_package(*script);
        for (std::size_t f = first; f < std::min(files.size(), first + batch); ++f)
        {
            const Package& package = linker.load(files[f]);
            for (std::size_t i = 0; i < package.exports().size(); ++i)
            {
                const ObjectRef ref{&package, ObjectIndex::from_export(i)};
                const std::string cls = package.class_name(ref.index);
                if ((cls != "Material" && cls != "MaterialInstanceConstant" && cls != "MaterialInstanceTimeVarying") ||
                    (package.exports()[i].object_flags & ObjectFlag::ClassDefaultObject) != 0)
                {
                    continue;
                }
                try
                {
                    const MaterialInfo info = read_material(linker, ref, &schema);
                    ++classes[cls];
                    parameters += info.parameters.size();
                    textures += info.textures.size();
                    expressions += info.expressions.size();
                    undecoded += info.undecoded_parameters;
                    for (const auto& p : info.parameters) unnamed += p.name.empty() ? 1 : 0;
                }
                catch (const Error& e)
                {
                    if (failures++ < 10) ADD_FAILURE() << linker.qualified_path(ref) << ": " << e.what();
                }
            }
        }
    }
    for (const auto& [c, n] : classes) std::cout << "    " << n << "x " << c << '\n';
    std::cout << "  parameters: " << parameters << ", textures referenced: " << textures
              << ", expressions: " << expressions << ", undecoded parameter arrays: " << undecoded << '\n';
    EXPECT_EQ(failures, 0u);
    EXPECT_EQ(undecoded, 0u);
    EXPECT_EQ(unnamed, 0u);
}

TEST(MirrorsEdgeAssetTests, FullCorpusBrushes)
{
    // Every Model and Polys export of the game (brush geometry). Loading every package takes minutes in Debug, so it
    // runs with the fixture sweep.
    if (!fixtures_available() || env("UNREALLIB_FIXTURE_SWEEP") != "1")
    {
        GTEST_SKIP() << "needs UNREALLIB_MIRRORSEDGE_DIR and UNREALLIB_FIXTURE_SWEEP=1";
    }
    AssetTally tally;
    for (const auto& file : packages(true))
    {
        Linker linker;
        linker.add_search_path(fixture_dir());
        check_assets(linker, linker.load(file), tally, true);
    }
    print_map("read", tally.read);
    print_map("failures", tally.failures);
    std::cout << "  brush polygons: " << tally.polygons << ", BSP surfaces: " << tally.model_surfaces
              << ", models with Polys: " << tally.models_with_polys << std::endl;
    EXPECT_GT(tally.read["Model"], 0u);
    EXPECT_GT(tally.read["Polys"], 0u);
    EXPECT_EQ(tally.total_failures, 0u);
}
