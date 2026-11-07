// Example: load a package, print its summary and the first few exports.
//
//   UnrealLibExampleLoadPackage "<path>/TdGame/CookedPC/Core.u"

#include <cstdio>
#include <filesystem>
#include <iostream>

#include <UnrealLib/UnrealLib.hpp>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " <package>\n";
        return 1;
    }

    try
    {
        UnrealLib::LoadOptions options;
        options.profile = "Mirror's Edge PC"; // or leave empty to auto-detect

        UnrealLib::Package package;
        package.load(std::filesystem::path(argv[1]), options);

        const auto& summary = package.summary();
        std::cout << package.name() << ": version " << summary.file_version << '/' << summary.licensee_version
                  << ", " << package.names().size() << " names, " << package.imports().size() << " imports, "
                  << package.exports().size() << " exports\n";

        for (std::size_t i = 0; i < package.exports().size() && i < 10; ++i)
        {
            const auto index = UnrealLib::ObjectIndex::from_export(i);
            std::cout << "  " << package.class_name(index) << ' ' << package.object_path(index) << '\n';
        }

        if (const auto object = package.find_export("Object"))
        {
            std::cout << "Found " << package.object_path(*object) << " (" << package.export_data(object->export_index()).size()
                      << " bytes of serialized data)\n";
        }

        const auto validation = UnrealLib::validate_package(package);
        std::cout << "Validation: " << validation.count(UnrealLib::Severity::Error) << " error(s), "
                  << validation.count(UnrealLib::Severity::Warning) << " warning(s)\n";
        return validation.has_errors() ? 1 : 0;
    }
    catch (const UnrealLib::Error& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}
