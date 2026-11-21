// UnrealPackageExtractor — extracts supported assets from a package.
//
//   textures (Texture2D and subclasses): PNG (default), TGA, or DDS with every available mip (lossless)
//   sounds (SoundNodeWave): the stored file as-is (Mirror's Edge PC: Ogg Vorbis)
//   static meshes: Wavefront OBJ (LOD 0); skeletal meshes: ActorX PSK (LOD 0, with skeleton and weights)
//   animations: one ActorX PSA per AnimSet (all its sequences)
//   materials: <Name>.mat.json (parent, parameters, textures; compiled shaders are not decoded)
//   particles: <Name>.particle.json (emitter, LOD and module links)
//   vehicles: <Name>.vehicle.json (tagged properties and resolved links, including class defaults)
//   levels: <Package>/<Package>.t3d (the actors of every Level export, with their subobjects, as T3D text)
//
// Output: <output>/<Package>/<Outer path>/<Name>.<ext>. Mips streamed from other packages are found through the
// search paths (default: the package's directory, recursively).
//
// Exit codes: 0 = success, 1 = some assets failed, 2 = the package failed to load, 3 = invalid command line.

#include <algorithm>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <UnrealLib/UnrealLib.hpp>

#include "../Common/PackageJson.hpp"

namespace fs = std::filesystem;
using namespace UnrealLib;
using namespace UnrealLib::Assets;

namespace
{
    struct Settings
    {
        std::string texture_format = "png";
        bool textures = true;
        bool sounds = true;
        bool meshes = true;
        bool animations = true;
        bool materials = true;
        bool particles = false;
        bool vehicles = false;
        bool levels = true;
        std::string filter;
        bool quiet = false;
    };

    struct Totals
    {
        std::size_t written = 0;
        std::size_t skipped = 0;
        std::size_t failed = 0;
    };

    /// Output path for export @p index: its outer chain becomes directories.
    fs::path output_path(const fs::path& root, const Package& package, ObjectIndex index, std::string_view extension)
    {
        std::string path = package.object_path(index);
        std::replace(path.begin(), path.end(), '.', '/');
        for (char& c : path)
        {
            if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        }
        return root / package.name() / Tools::utf8_path(path + "." + std::string(extension));
    }

    std::optional<DdsFormat> dds_format(PixelFormat format)
    {
        switch (format)
        {
            case PixelFormat::DXT1: return DdsFormat::DXT1;
            case PixelFormat::DXT3: return DdsFormat::DXT3;
            case PixelFormat::DXT5: return DdsFormat::DXT5;
            case PixelFormat::BC5: return DdsFormat::ATI2;
            case PixelFormat::A8R8G8B8: return DdsFormat::A8R8G8B8;
            case PixelFormat::G8: return DdsFormat::L8;
            default: return std::nullopt;
        }
    }

    void extract_texture(Linker& linker, const Package& package, ObjectRef ref, const fs::path& root,
                         const Settings& settings, Totals& totals)
    {
        const Texture2D texture = read_texture2d(linker, ref);
        const auto first = texture.first_mip_with_payload();
        if (!first)
        {
            ++totals.skipped;
            return;
        }
        if (settings.texture_format == "dds")
        {
            if (const auto format = dds_format(texture.format))
            {
                std::vector<std::vector<std::byte>> mips;
                for (std::size_t m = *first; m < texture.mips.size(); ++m)
                {
                    if (!texture.mips[m].data.has_payload()) break;
                    mips.push_back(load_mip(package, texture, m, &linker));
                }
                const auto& top = texture.mips[*first];
                write_file(output_path(root, package, ref.index, "dds"),
                           encode_dds(*format, static_cast<std::uint32_t>(top.size_x),
                                      static_cast<std::uint32_t>(top.size_y), mips));
                ++totals.written;
                return;
            }
            // Formats DDS cannot hold unchanged fall back to PNG.
        }
        const Image image = decode_mip(package, texture, *first, &linker);
        if (settings.texture_format == "tga")
        {
            write_file(output_path(root, package, ref.index, "tga"), image.to_tga());
        }
        else
        {
            write_file(output_path(root, package, ref.index, "png"), image.to_png());
        }
        ++totals.written;
    }

    void extract_static_mesh(Linker& linker, const Package& package, ObjectRef ref, const fs::path& root,
                             Totals& totals)
    {
        const StaticMesh mesh = read_static_mesh(linker, ref);
        if (mesh.lods.empty())
        {
            ++totals.skipped;
            return;
        }
        const std::string obj = to_obj(package, mesh);
        write_file(output_path(root, package, ref.index, "obj"), std::as_bytes(std::span(obj)));
        ++totals.written;
    }

    void extract_skeletal_mesh(Linker& linker, const Package& package, ObjectRef ref, const fs::path& root,
                               Totals& totals)
    {
        const SkeletalMesh mesh = read_skeletal_mesh(linker, ref);
        if (mesh.lods.empty())
        {
            ++totals.skipped;
            return;
        }
        write_file(output_path(root, package, ref.index, "psk"), to_psk(package, mesh));
        ++totals.written;
    }

    void extract_anim_set(Linker& linker, const Package& package, ObjectRef ref, const fs::path& root, Totals& totals)
    {
        const AnimSet set = read_anim_set(linker, ref);
        std::vector<AnimSequence> sequences;
        for (const ObjectIndex s : set.sequences)
        {
            if (s.is_export()) sequences.push_back(read_anim_sequence(linker, ObjectRef{&package, s}));
        }
        if (set.track_bone_names.empty() || sequences.empty())
        {
            ++totals.skipped;
            return;
        }
        write_file(output_path(root, package, ref.index, "psa"), to_psa(set, sequences));
        ++totals.written;
    }

    void extract_material(Linker& linker, const Package& package, ObjectRef ref, const fs::path& root,
                          const PropertySchema* schema, Totals& totals)
    {
        const MaterialInfo info = read_material(linker, ref, schema);
        const auto path_of = [&](ObjectIndex i) -> nlohmann::ordered_json {
            if (i.is_null() || !package.is_valid(i)) return nullptr;
            return package.class_name(i) + "'" + (i.is_export() ? package.name() + "." : "") + package.object_path(i) + "'";
        };
        nlohmann::ordered_json j;
        j["name"] = info.name;
        j["class"] = info.class_name;
        j["parent"] = path_of(info.parent);
        j["blend_mode"] = info.blend_mode;
        j["two_sided"] = info.two_sided;
        nlohmann::ordered_json params = nlohmann::ordered_json::array();
        for (const auto& p : info.parameters)
        {
            nlohmann::ordered_json e;
            e["name"] = p.name;
            switch (p.kind)
            {
                case MaterialParameter::Kind::Scalar: e["scalar"] = p.scalar; break;
                case MaterialParameter::Kind::Vector: e["vector"] = p.vector; break;
                case MaterialParameter::Kind::Texture: e["texture"] = path_of(p.texture); break;
            }
            params.push_back(std::move(e));
        }
        j["parameters"] = std::move(params);
        nlohmann::ordered_json textures = nlohmann::ordered_json::array();
        for (const ObjectIndex t : info.textures) textures.push_back(path_of(t));
        j["textures"] = std::move(textures);
        j["expressions"] = info.expressions.size();
        j["native_shader_bytes"] = info.native_bytes;
        const std::string text = j.dump(2) + "\n";
        write_file(output_path(root, package, ref.index, "mat.json"), std::as_bytes(std::span(text)));
        ++totals.written;
    }

    nlohmann::ordered_json asset_link_json(Linker& linker, ObjectIndex index, const Resolution& resolution)
    {
        if (index.is_null()) return nullptr;
        if (resolution.status == ResolveStatus::Export)
        {
            const ObjectRef target = resolution.target;
            return {{"path", linker.qualified_path(target)}, {"class", target.package->class_name(target.index)}};
        }
        return {{"index", index.value}, {"status", to_string(resolution.status)}, {"detail", resolution.detail}};
    }

    void extract_particles(Linker& linker, ObjectRef ref, const fs::path& root, const PropertySchema& schema,
                           Totals& totals)
    {
        using Json = nlohmann::ordered_json;
        const ParticleSystem system = read_particle_system(linker, ref, schema);
        const auto link_json = [&](const ParticleLink& link) {
            return asset_link_json(linker, link.index, link.resolution);
        };
        Json emitters = Json::array();
        for (const ParticleLink& emitter_link : system.emitters)
        {
            Json emitter_json = link_json(emitter_link);
            if (emitter_link.resolution.status == ResolveStatus::Export)
            {
                const ParticleEmitter emitter = read_particle_emitter(linker, emitter_link.resolution.target, schema);
                Json levels = Json::array();
                for (const ParticleLink& lod_link : emitter.lod_levels)
                {
                    Json lod_json = link_json(lod_link);
                    if (lod_link.resolution.status == ResolveStatus::Export)
                    {
                        const ParticleLodLevel lod = read_particle_lod_level(linker, lod_link.resolution.target, schema);
                        if (const auto level = lod.source.properties.get_int("Level")) lod_json["level"] = *level;
                        if (const auto enabled = lod.source.properties.get_bool("bEnabled")) lod_json["enabled"] = *enabled;
                        lod_json["required_module"] = link_json(lod.required_module);
                        lod_json["spawn_module"] = link_json(lod.spawn_module);
                        lod_json["type_data_module"] = link_json(lod.type_data_module);
                        Json modules = Json::array();
                        for (const ParticleLink& module : lod.modules)
                        {
                            modules.push_back(link_json(module));
                        }
                        lod_json["modules"] = std::move(modules);
                    }
                    levels.push_back(std::move(lod_json));
                }
                emitter_json["lod_levels"] = std::move(levels);
            }
            emitters.push_back(std::move(emitter_json));
        }
        const Json json{{"object", linker.qualified_path(ref)}, {"emitters", std::move(emitters)}};
        const std::string text = json.dump(2) + "\n";
        write_file(output_path(root, *ref.package, ref.index, "particle.json"), std::as_bytes(std::span(text)));
        ++totals.written;
    }

    void extract_vehicle(Linker& linker, ObjectRef ref, const fs::path& root, const PropertySchema& schema,
                         Totals& totals)
    {
        using Json = nlohmann::ordered_json;
        const VehicleObject vehicle = read_vehicle_object(linker, ref, schema);
        PropertyFormatOptions format{&ref.package->names(), {}};
        Json properties = Json::array();
        for (const Property& property : vehicle.source.properties.all())
        {
            properties.push_back({{"name", property.name}, {"array_index", property.array_index},
                                  {"value", format_property_value(property.value, format)}});
        }
        Json links = Json::array();
        for (const VehicleLink& link : vehicle.links)
        {
            Json entry{{"property", link.property}, {"target", asset_link_json(linker, link.index, link.resolution)}};
            if (link.array_index) entry["array_index"] = *link.array_index;
            links.push_back(std::move(entry));
        }
        const Json json{{"object", linker.qualified_path(ref)}, {"class", vehicle.source.class_name},
                        {"properties", std::move(properties)}, {"links", std::move(links)}};
        const std::string text = json.dump(2) + "\n";
        write_file(output_path(root, *ref.package, ref.index, "vehicle.json"), std::as_bytes(std::span(text)));
        ++totals.written;
    }

    /// Writes <Package>/<Package>.t3d if the package has a Level export (no-op otherwise).
    void extract_levels(Linker& linker, const Package& package, const fs::path& root, const PropertySchema* schema,
                        const Settings& settings, Totals& totals)
    {
        try
        {
            T3D::ExportOptions options;
            options.schema = schema;
            Diagnostics diagnostics;
            const std::string text = T3D::serialize(T3D::export_level(linker, package, options, &diagnostics));
            for (const Diagnostic& d : diagnostics.entries()) fmt::print(stderr, "warning: {}\n", d.message);
            write_file(root / Tools::utf8_path(package.name()) / Tools::utf8_path(package.name() + ".t3d"),
                       std::as_bytes(std::span(text)));
            ++totals.written;
            if (!settings.quiet) fmt::print("Level {}\n", package.name());
        }
        catch (const Error& e)
        {
            if (e.code() == ErrorCode::NotFound) return; // not a map
            ++totals.failed;
            fmt::print(stderr, "error: Level {}: {}\n", package.name(), e.what());
        }
    }

    void extract_sound(Linker& linker, const Package& package, ObjectRef ref, const fs::path& root, Totals& totals)
    {
        const SoundWave sound = read_sound_wave(linker, ref);
        const auto extracted = Assets::extract_sound(package, sound);
        if (!extracted)
        {
            ++totals.skipped;
            return;
        }
        write_file(output_path(root, package, ref.index, file_extension(extracted->container)), extracted->bytes);
        ++totals.written;
    }
}

int main(int argc, char** argv)
{
    CLI::App app{"Extract assets from an Unreal Engine 3 package (UnrealLib)",
                 "UnrealPackageExtractor"};
    argv = app.ensure_utf8(argv);

    std::string path;
    std::string output;
    std::string profile;
    std::vector<std::string> search_paths;
    std::vector<std::string> types{"textures", "sounds", "meshes", "animations", "materials", "levels"};
    Settings settings;

    app.add_option("package", path, "Package file")->required()->check(CLI::ExistingFile);
    app.add_option("-o,--output", output, "Output directory")->required();
    app.add_option("--profile", profile, "Game profile id or name (default: auto-detect)");
    app.add_option("-s,--search-path", search_paths,
                   "Directories with the game's packages (default: the package's directory and the nearest parent with Core.u)")
        ->check(CLI::ExistingDirectory);
    app.add_option("-t,--types", types,
                   "Asset types to extract: textures, sounds, meshes, animations, materials, levels, particles, vehicles")
        ->check(CLI::IsMember({"textures", "sounds", "meshes", "animations", "materials", "levels", "particles",
                               "vehicles"}))
        ->delimiter(',');
    app.add_option("--texture-format", settings.texture_format, "png, tga or dds (dds keeps every mip losslessly)")
        ->check(CLI::IsMember({"png", "tga", "dds"}));
    app.add_option("--filter", settings.filter, "Only objects whose path contains this text (case-insensitive)");
    app.add_flag("-q,--quiet", settings.quiet, "Only print problems and the totals");
    CLI11_PARSE(app, argc, argv);

    settings.textures = std::find(types.begin(), types.end(), "textures") != types.end();
    settings.sounds = std::find(types.begin(), types.end(), "sounds") != types.end();
    settings.meshes = std::find(types.begin(), types.end(), "meshes") != types.end();
    settings.animations = std::find(types.begin(), types.end(), "animations") != types.end();
    settings.materials = std::find(types.begin(), types.end(), "materials") != types.end();
    settings.particles = std::find(types.begin(), types.end(), "particles") != types.end();
    settings.vehicles = std::find(types.begin(), types.end(), "vehicles") != types.end();
    settings.levels = std::find(types.begin(), types.end(), "levels") != types.end();

    try
    {
        LoadOptions options;
        if (!profile.empty()) options.profile = profile;
        const fs::path file = Tools::utf8_path(path);
        Linker linker(options);
        if (search_paths.empty())
        {
            // Default: the package's directory; if the script packages (Core.u) are not below it, the nearest parent
            // directory that has them (e.g. CookedPC for CookedPC/Animations/X.upk), so class hierarchies resolve.
            fs::path dir = fs::absolute(file).parent_path();
            linker.add_search_path(dir);
            for (int up = 0; up < 4 && !linker.find_package_file("Core") && dir.has_parent_path() &&
                             dir.parent_path() != dir;
                 ++up)
            {
                dir = dir.parent_path();
                if (fs::exists(dir / "Core.u")) linker.add_search_path(dir);
            }
            if (!linker.find_package_file("Core"))
            {
                fmt::print(stderr, "warning: Core.u not found; pass --search-path <CookedPC> so that subclasses "
                                   "(e.g. TdAnimSet) and streamed texture mips are found\n");
            }
        }
        for (const auto& dir : search_paths) linker.add_search_path(Tools::utf8_path(dir));
        const Package& package = linker.load(file);
        // Resolve reflected property types for the readers that need them.
        std::unique_ptr<ReflectionSchema> schema;
        if (settings.materials || settings.levels || settings.particles || settings.vehicles)
        {
            schema = std::make_unique<ReflectionSchema>(linker);
            for (const char* script : {"Core", "Engine", "GameFramework", "TdGame"})
            {
                if (const Package* p = linker.find_package(script)) schema->add_package(*p);
            }
        }
        const fs::path root = Tools::utf8_path(output);

        Totals totals;
        std::unordered_map<std::int32_t, bool> vehicle_classes;
        const std::string filter = Text::to_lower_ascii(settings.filter);
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ObjectRef ref{&package, ObjectIndex::from_export(i)};
            const ExportEntry& entry = package.exports()[i];
            if (entry.serial_size <= 0)
            {
                continue;
            }
            bool vehicle = false;
            if (settings.vehicles && !entry.class_index.is_null())
            {
                const auto [cached, inserted] = vehicle_classes.try_emplace(entry.class_index.value, false);
                if (inserted) cached->second = vehicle_object_kind(linker, ref).has_value();
                vehicle = cached->second;
            }
            if (!vehicle && (entry.object_flags & ObjectFlag::ClassDefaultObject) != 0) continue;
            const std::string cls = package.class_name(ref.index);
            const bool texture = settings.textures && is_texture2d_class(cls);
            const bool sound = settings.sounds && cls == "SoundNodeWave";
            const bool mesh = settings.meshes && cls == "StaticMesh";
            const bool skeletal = settings.meshes && cls == "SkeletalMesh";
            const bool anim_set =
                settings.animations && (cls == "AnimSet" || linker.is_a(ref, "Engine.AnimSet").value_or(false));
            const bool material = settings.materials && (cls == "Material" || cls == "MaterialInstanceConstant" ||
                                                         cls == "MaterialInstanceTimeVarying");
            const bool particle = settings.particles && cls == "ParticleSystem";
            if (!texture && !sound && !mesh && !skeletal && !anim_set && !material && !particle && !vehicle)
            {
                continue;
            }
            const std::string object_path = package.object_path(ref.index);
            if (!filter.empty() && Text::to_lower_ascii(object_path).find(filter) == std::string::npos)
            {
                continue;
            }
            try
            {
                const std::size_t before = totals.written;
                if (texture) extract_texture(linker, package, ref, root, settings, totals);
                else if (mesh) extract_static_mesh(linker, package, ref, root, totals);
                else if (skeletal) extract_skeletal_mesh(linker, package, ref, root, totals);
                else if (anim_set) extract_anim_set(linker, package, ref, root, totals);
                else if (material) extract_material(linker, package, ref, root, schema.get(), totals);
                else if (particle) extract_particles(linker, ref, root, *schema, totals);
                else if (vehicle) extract_vehicle(linker, ref, root, *schema, totals);
                else extract_sound(linker, package, ref, root, totals);
                if (!settings.quiet && totals.written != before)
                {
                    fmt::print("{} {}\n", cls, object_path);
                }
            }
            catch (const Error& e)
            {
                ++totals.failed;
                fmt::print(stderr, "error: {} {}: {}\n", cls, object_path, e.what());
            }
        }
        if (settings.levels && settings.filter.empty())
        {
            extract_levels(linker, package, root, schema.get(), settings, totals);
        }
        fmt::print(stderr, "{} written, {} without data, {} failed\n", totals.written, totals.skipped, totals.failed);
        return totals.failed == 0 ? 0 : 1;
    }
    catch (const Error& e)
    {
        fmt::print(stderr, "error ({}): {}\n", to_string(e.code()), e.what());
        return 2;
    }
}
