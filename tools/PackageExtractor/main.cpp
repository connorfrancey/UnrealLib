// UnrealPackageExtractor — extracts supported assets from a package.
//
//   textures (Texture2D and subclasses): PNG (default), TGA, or DDS with every available mip (lossless)
//   sounds (SoundNodeWave): the stored file as-is (Mirror's Edge PC: Ogg Vorbis)
//   static meshes: Wavefront OBJ (LOD 0); skeletal meshes: ActorX PSK (LOD 0, with skeleton and weights)
//
// Output: <output>/<Package>/<Outer path>/<Name>.<ext>. Mips streamed from other packages are found through the
// search paths (default: the package's directory, recursively).
//
// Exit codes: 0 = success, 1 = some assets failed, 2 = the package failed to load, 3 = invalid command line.

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
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
    CLI::App app{"Extract assets (textures, sounds, meshes) from an Unreal Engine 3 package (UnrealLib)",
                 "UnrealPackageExtractor"};
    argv = app.ensure_utf8(argv);

    std::string path;
    std::string output;
    std::string profile;
    std::vector<std::string> search_paths;
    std::vector<std::string> types{"textures", "sounds", "meshes"};
    Settings settings;

    app.add_option("package", path, "Package file")->required()->check(CLI::ExistingFile);
    app.add_option("-o,--output", output, "Output directory")->required();
    app.add_option("--profile", profile, "Game profile id or name (default: auto-detect)");
    app.add_option("-s,--search-path", search_paths,
                   "Directories with the packages that hold streamed texture mips (default: the package's directory)")
        ->check(CLI::ExistingDirectory);
    app.add_option("-t,--types", types, "Asset types to extract: textures, sounds, meshes")
        ->check(CLI::IsMember({"textures", "sounds", "meshes"}))
        ->delimiter(',');
    app.add_option("--texture-format", settings.texture_format, "png, tga or dds (dds keeps every mip losslessly)")
        ->check(CLI::IsMember({"png", "tga", "dds"}));
    app.add_option("--filter", settings.filter, "Only objects whose path contains this text (case-insensitive)");
    app.add_flag("-q,--quiet", settings.quiet, "Only print problems and the totals");
    CLI11_PARSE(app, argc, argv);

    settings.textures = std::find(types.begin(), types.end(), "textures") != types.end();
    settings.sounds = std::find(types.begin(), types.end(), "sounds") != types.end();
    settings.meshes = std::find(types.begin(), types.end(), "meshes") != types.end();

    try
    {
        LoadOptions options;
        if (!profile.empty()) options.profile = profile;
        const fs::path file = Tools::utf8_path(path);
        Linker linker(options);
        if (search_paths.empty())
        {
            linker.add_search_path(fs::absolute(file).parent_path());
        }
        for (const auto& dir : search_paths) linker.add_search_path(Tools::utf8_path(dir));
        const Package& package = linker.load(file);
        const fs::path root = Tools::utf8_path(output);

        Totals totals;
        const std::string filter = Text::to_lower_ascii(settings.filter);
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ObjectRef ref{&package, ObjectIndex::from_export(i)};
            const ExportEntry& entry = package.exports()[i];
            if (entry.serial_size <= 0 || (entry.object_flags & ObjectFlag::ClassDefaultObject) != 0)
            {
                continue;
            }
            const std::string cls = package.class_name(ref.index);
            const bool texture = settings.textures && is_texture2d_class(cls);
            const bool sound = settings.sounds && cls == "SoundNodeWave";
            const bool mesh = settings.meshes && cls == "StaticMesh";
            const bool skeletal = settings.meshes && cls == "SkeletalMesh";
            if (!texture && !sound && !mesh && !skeletal)
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
        fmt::print(stderr, "{} written, {} without data, {} failed\n", totals.written, totals.skipped, totals.failed);
        return totals.failed == 0 ? 0 : 1;
    }
    catch (const Error& e)
    {
        fmt::print(stderr, "error ({}): {}\n", to_string(e.code()), e.what());
        return 2;
    }
}
