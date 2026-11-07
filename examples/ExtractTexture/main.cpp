// Example: decode the largest mip of a texture and save it as PNG. Mips stored in another package (streamed textures)
// are loaded through the linker's search path.
//
//   UnrealLibExampleExtractTexture "<path>/TdGame/CookedPC/EngineResources.upk" DefaultTexture DefaultTexture.png
//
// Arguments: <package> <texture name or path> <output.png> [directory with the game's packages]

#include <filesystem>
#include <fstream>
#include <iostream>

#include <UnrealLib/UnrealLib.hpp>

namespace fs = std::filesystem;
using namespace UnrealLib;

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: " << argv[0] << " <package> <texture> <output.png> [game package directory]\n";
        return 1;
    }
    try
    {
        const fs::path file = argv[1];
        Linker linker;
        linker.add_search_path(argc > 4 ? fs::path(argv[4]) : fs::absolute(file).parent_path());
        const Package& package = linker.load(file);

        const auto index = package.find_export(argv[2]);
        if (!index || !Assets::is_texture2d_class(package.class_name(*index)))
        {
            std::cerr << "no texture named '" << argv[2] << "' in " << package.name() << '\n';
            return 1;
        }
        const Assets::Texture2D texture = Assets::read_texture2d(linker, ObjectRef{&package, *index});
        if (texture.mips.empty())
        {
            std::cerr << texture.name << " has no mips\n";
            return 1;
        }
        const Assets::Image image = Assets::decode_mip(package, texture, 0, &linker); // mip 0 is the largest
        const std::vector<std::byte> png = image.to_png();
        std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char*>(png.data()),
                                                       static_cast<std::streamsize>(png.size()));
        std::cout << texture.name << ": " << image.width << 'x' << image.height << " -> " << argv[3] << '\n';
        return 0;
    }
    catch (const Error& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}
