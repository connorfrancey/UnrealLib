// Fuzz target "assets": every asset reader on the exports of a fuzzed package (texture, sound, meshes, animation,
// material, brush), dispatched by class name, and pixel decoding of what a texture reader accepted.

#include <UnrealLib/Assets/Animation.hpp>
#include <UnrealLib/Assets/Brush.hpp>
#include <UnrealLib/Assets/Material.hpp>
#include <UnrealLib/Assets/SkeletalMesh.hpp>
#include <UnrealLib/Assets/Sound.hpp>
#include <UnrealLib/Assets/StaticMesh.hpp>
#include <UnrealLib/Assets/Texture.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Package/Package.hpp>

#include "../FuzzTargets.hpp"

namespace UnrealLib::Fuzz
{
    LoadOptions fuzz_load_options(); // PackageTarget.cpp
}

UNREALLIB_FUZZ_TARGET(assets)
{
    using namespace UnrealLib;
    using namespace UnrealLib::Assets;
    const LoadOptions options = Fuzz::fuzz_load_options();
    Package package;
    package.load_from_memory(std::vector<std::byte>(Fuzz::as_bytes(data, size).begin(), Fuzz::as_bytes(data, size).end()),
                             "FuzzedAssets", options);
    Linker linker(options);
    const Package& added = linker.add(std::move(package));
    for (std::size_t i = 0; i < added.exports().size(); ++i)
    {
        const ObjectRef ref{&added, ObjectIndex::from_export(i)};
        try
        {
            const std::string cls = added.class_name(ref.index);
            if (is_texture2d_class(cls))
            {
                const Texture2D texture = read_texture2d(linker, ref);
                for (std::size_t m = 0; m < texture.mips.size() && m < 2; ++m)
                {
                    (void)decode_mip(added, texture, m);
                }
            }
            else if (cls == "TextureCube") (void)read_texture_cube(linker, ref);
            else if (cls == "SoundNodeWave") (void)read_sound_wave(linker, ref);
            else if (cls == "StaticMesh") (void)read_static_mesh(linker, ref);
            else if (cls == "SkeletalMesh") (void)read_skeletal_mesh(linker, ref);
            else if (cls == "AnimSequence") (void)read_anim_sequence(linker, ref);
            else if (cls == "AnimSet") (void)read_anim_set(linker, ref);
            else if (cls == "Material" || cls == "MaterialInstanceConstant") (void)read_material(linker, ref);
            else if (cls == "Polys") (void)read_polys(linker, ref);
            else if (cls == "Model") (void)read_model(linker, ref);
        }
        catch (const Error&)
        {
        }
    }
}
