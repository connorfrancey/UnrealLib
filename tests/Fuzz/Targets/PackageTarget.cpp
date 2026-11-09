// Fuzz target "package": package summary, compressed chunks, name/import/export/depends tables, validation, object
// paths, export data, UObject headers and tagged properties of every export.

#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Package/PackageValidator.hpp>

#include "../FuzzTargets.hpp"

namespace UnrealLib::Fuzz
{
    LoadOptions fuzz_load_options()
    {
        LoadOptions options;
        options.limits.max_allocation_bytes = 16u << 20;
        options.limits.max_decompressed_size = 16u << 20;
        return options;
    }
}

UNREALLIB_FUZZ_TARGET(package)
{
    using namespace UnrealLib;
    const LoadOptions options = Fuzz::fuzz_load_options();
    Package package;
    package.load_from_memory(std::vector<std::byte>(Fuzz::as_bytes(data, size).begin(), Fuzz::as_bytes(data, size).end()),
                             "Fuzzed", options);
    (void)validate_package(package);
    Linker linker(options);
    const Package& added = linker.add(std::move(package));
    for (std::size_t i = 0; i < added.exports().size(); ++i)
    {
        const ObjectRef ref{&added, ObjectIndex::from_export(i)};
        try
        {
            (void)added.object_path(ref.index);
            (void)added.class_name(ref.index);
            (void)added.export_data(i);
            (void)linker.qualified_path(ref);
            (void)linker.read_properties(ref);
        }
        catch (const Error&)
        {
            // Per-object errors are expected; keep going with the next export.
        }
    }
}
