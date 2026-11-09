// Uses the installed headers and libraries: version header, names, binary serialization, compression and T3D.

#include <cstdio>
#include <string_view>
#include <vector>

#include <UnrealLib/UnrealLib.hpp>

int main()
{
    using namespace UnrealLib;
    if (library_version_string != std::string_view(UNREALLIB_EXPECTED_VERSION))
    {
        std::fprintf(stderr, "version mismatch\n");
        return 1;
    }

    NameTable names;
    const FName none{names.add("None"), 0};

    BinaryWriter writer;
    writer.write_fstring(FString("installed"));
    write_fname(writer, none);
    BinaryReader reader(writer.data());
    if (reader.read_fstring().text != "installed" || names.to_string(read_fname(reader)) != "None")
    {
        std::fprintf(stderr, "serialization round trip failed\n");
        return 1;
    }

    const T3D::T3DDocument doc = T3D::parse("Begin Map\n   Begin Level NAME=PersistentLevel\n   End Level\nEnd Map\n");
    if (T3D::serialize(doc) != "Begin Map\n   Begin Level NAME=PersistentLevel\n   End Level\nEnd Map\n")
    {
        std::fprintf(stderr, "T3D round trip failed\n");
        return 1;
    }
    std::printf("UnrealLib %s consumer OK\n", library_version_string.data());
    return 0;
}
