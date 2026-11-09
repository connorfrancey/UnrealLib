// Example: find an object in a package and inspect it — class, tagged properties (typed with the script packages)
// and, for functions, states and classes, the decompiled UnrealScript.
//
//   UnrealLibExampleInspectObjects "<path>/TdGame/CookedPC/Engine.u" Actor.TriggerEventClass
//   UnrealLibExampleInspectObjects "<path>/TdGame/CookedPC/Maps/SP04/Subway_Train.me1" BlockingVolume_0 "<path>/TdGame/CookedPC"
//
// Arguments: <package> <object name or path> [directory with the game's packages, default: the package's directory]

#include <filesystem>
#include <iostream>
#include <vector>

#include <UnrealLib/UnrealLib.hpp>

namespace fs = std::filesystem;
using namespace UnrealLib;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " <package> <object name or path> [game package directory]\n";
        return 1;
    }
    try
    {
        const fs::path file = argv[1];
        Linker linker;
        linker.add_search_path(argc > 3 ? fs::path(argv[3]) : fs::absolute(file).parent_path());
        const Package& package = linker.load(file);

        const auto index = package.find_export(argv[2]);
        if (!index)
        {
            std::cerr << "no export named '" << argv[2] << "' in " << package.name() << '\n';
            return 1;
        }
        const ObjectRef object{&package, *index};
        std::cout << linker.qualified_path(object) << " (class " << package.class_name(*index) << ")\n";

        // Type information for struct and array values comes from the script packages that define the classes.
        ReflectionSchema schema(linker);
        std::vector<const Package*> scripts;
        for (const char* name : {"Core", "Engine", "GameFramework", "TdGame"})
        {
            if (const Package* p = linker.find_package(name))
            {
                schema.add_package(*p);
                scripts.push_back(p);
            }
        }

        const std::string cls = package.class_name(*index);
        if (cls == "Function" || cls == "State" || cls == "Class")
        {
            Decompiler::ScriptDecompiler decompiler(linker, Decompiler::NativeTable::from_packages(linker, scripts));
            decompiler.set_schema(&schema);
            const auto script = cls == "Class" ? decompiler.decompile_class(object) : decompiler.decompile(object);
            std::cout << script.source;
            return 0;
        }

        PropertyFormatOptions format;
        format.names = &package.names();
        format.object_name = [&](ObjectIndex i) { return package.object_path(i); };
        const ObjectProperties properties = linker.read_properties(object, &schema);
        for (const Property& p : properties.properties)
        {
            std::cout << "  " << p.name;
            if (p.array_index != 0) std::cout << '(' << p.array_index << ')';
            std::cout << " = " << format_property_value(p.value, format) << '\n';
        }
        std::cout << properties.properties.size() << " properties; native data starts at byte "
                  << properties.end_offset << " of " << package.export_data(index->export_index()).size() << '\n';
        return 0;
    }
    catch (const Error& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}
