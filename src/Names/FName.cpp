#include <UnrealLib/Names/FName.hpp>

#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

namespace UnrealLib
{
    FName read_fname(BinaryReader& reader)
    {
        FName name;
        name.index = reader.read_index();
        if (reader.context().names_have_number())
        {
            name.number = reader.read_i32();
        }
        return name;
    }

    void write_fname(BinaryWriter& writer, const FName& name)
    {
        writer.write_index(name.index);
        if (writer.context().names_have_number())
        {
            writer.write_i32(name.number);
        }
    }
}
