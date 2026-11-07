#include <UnrealLib/Linker/Linker.hpp>

#include <unordered_set>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>

namespace UnrealLib
{
    namespace
    {
        constexpr std::string_view k_package_extensions[] = {".u", ".upk", ".umap", ".me1", ".ut3", ".udk"};

        bool is_package_file(const std::filesystem::path& path)
        {
            const std::string ext = Text::to_lower_ascii(path.extension().string());
            for (const auto candidate : k_package_extensions)
            {
                if (ext == candidate)
                {
                    return true;
                }
            }
            return false;
        }

        std::string utf8(const std::filesystem::path& path)
        {
            const auto u8 = path.u8string();
            return {reinterpret_cast<const char*>(u8.data()), u8.size()};
        }

        std::string name_key(std::int32_t outer, std::string_view name)
        {
            return fmt::format("{}|{}", outer, Text::to_lower_ascii(name));
        }

        std::string text_of(const Package& package, const FName& name)
        {
            return package.names().try_to_string(name).value_or(fmt::format("<name#{}>", name.index));
        }
    }

    std::string_view to_string(ResolveStatus status) noexcept
    {
        switch (status)
        {
        case ResolveStatus::Null: return "null";
        case ResolveStatus::Export: return "export";
        case ResolveStatus::Package: return "package";
        case ResolveStatus::Intrinsic: return "intrinsic";
        case ResolveStatus::PackageNotFound: return "package-not-found";
        case ResolveStatus::ObjectNotFound: return "object-not-found";
        case ResolveStatus::ClassMismatch: return "class-mismatch";
        case ResolveStatus::Invalid: return "invalid";
        }
        return "unknown";
    }

    struct Linker::PackageLookup
    {
        // name_key(outer, full name) -> export indices (several when objects of different classes share a name, e.g.
        // Core.u has a MetaData *instance* named "MetaData" while the class MetaData is intrinsic).
        std::unordered_map<std::string, std::vector<std::size_t>> exports;
    };

    Linker::Linker(LoadOptions options) : m_options(std::move(options))
    {
    }

    Linker::~Linker() = default;
    Linker::Linker(Linker&&) noexcept = default;
    Linker& Linker::operator=(Linker&&) noexcept = default;

    void Linker::add_search_path(const std::filesystem::path& directory, bool recursive)
    {
        if (!std::filesystem::is_directory(directory))
        {
            throw Error(ErrorCode::NotFound, fmt::format("search path '{}' is not a directory", utf8(directory)));
        }
        auto add = [&](const std::filesystem::path& file) {
            if (!is_package_file(file))
            {
                return;
            }
            const std::string stem = utf8(file.stem());
            const auto [it, inserted] = m_index.try_emplace(stem, file);
            if (!inserted && it->second != file)
            {
                m_diagnostics.warning("linker.duplicate-package",
                    fmt::format("package '{}' found at '{}' and '{}'; using the first", stem, utf8(it->second),
                                utf8(file)));
            }
        };
        if (recursive)
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
            {
                if (entry.is_regular_file())
                {
                    add(entry.path());
                }
            }
        }
        else
        {
            for (const auto& entry : std::filesystem::directory_iterator(directory))
            {
                if (entry.is_regular_file())
                {
                    add(entry.path());
                }
            }
        }
    }

    const Package& Linker::insert(std::unique_ptr<Package> package)
    {
        const auto [it, inserted] = m_by_name.try_emplace(package->name(), package.get());
        if (!inserted)
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("package '{}' is already loaded", package->name()));
        }
        m_packages.push_back(std::move(package));
        return *m_packages.back();
    }

    const Package& Linker::load(const std::filesystem::path& file)
    {
        const std::string stem = utf8(file.stem());
        if (const auto it = m_by_name.find(stem); it != m_by_name.end())
        {
            return *it->second;
        }
        auto package = std::make_unique<Package>();
        package->load(file, m_options);
        return insert(std::move(package));
    }

    const Package& Linker::add(Package package)
    {
        return insert(std::make_unique<Package>(std::move(package)));
    }

    const Package* Linker::find_package(std::string_view name)
    {
        if (const auto it = m_by_name.find(name); it != m_by_name.end())
        {
            return it->second;
        }
        if (m_failed.find(name) != m_failed.end())
        {
            return nullptr;
        }
        const auto file = m_index.find(name);
        if (file == m_index.end())
        {
            return nullptr;
        }
        try
        {
            return &load(file->second);
        }
        catch (const Error& e)
        {
            m_failed.emplace(std::string(name), e.what());
            m_diagnostics.error("linker.load-failed",
                fmt::format("package '{}' ('{}') failed to load: {}", name, utf8(file->second), e.what()));
            return nullptr;
        }
    }

    const Linker::PackageLookup& Linker::lookup_for(const Package& package)
    {
        auto& slot = m_lookups[&package];
        if (!slot)
        {
            slot = std::make_unique<PackageLookup>();
            const auto exports = package.exports();
            slot->exports.reserve(exports.size());
            for (std::size_t i = 0; i < exports.size(); ++i)
            {
                slot->exports[name_key(exports[i].outer.value, text_of(package, exports[i].object_name))].push_back(i);
            }
        }
        return *slot;
    }

    const Resolution& Linker::resolve(const Package& package, ObjectIndex index)
    {
        const auto key = std::make_pair(&package, index.value);
        if (const auto it = m_cache.find(key); it != m_cache.end())
        {
            return it->second;
        }
        Resolution result = resolve_uncached(package, index);
        return m_cache.emplace(key, std::move(result)).first->second;
    }

    Resolution Linker::resolve_uncached(const Package& package, ObjectIndex index)
    {
        if (index.is_null())
        {
            return {ResolveStatus::Null, {}, {}};
        }
        if (!package.is_valid(index))
        {
            return {ResolveStatus::Invalid, {}, fmt::format("index {} is out of range", index.value)};
        }
        if (index.is_export())
        {
            return {ResolveStatus::Export, ObjectRef{&package, index}, {}};
        }
        return resolve_import(package, index.import_index());
    }

    Resolution Linker::resolve_import(const Package& package, std::size_t import_index)
    {
        // Collect the outer chain: [import, outer, ..., package]. Outers are imports, or forced exports (seek-free
        // cooked stand-ins for the original package and its groups).
        struct Element
        {
            FName name;
            std::string class_package;
            std::string class_name;
        };
        std::vector<Element> chain;
        std::unordered_set<std::int32_t> seen;
        const ObjectIndex self = ObjectIndex::from_import(import_index);
        for (ObjectIndex current = self; !current.is_null();)
        {
            if (!package.is_valid(current))
            {
                return {ResolveStatus::Invalid, {},
                        fmt::format("import {} has an invalid outer {}", self.value, current.value)};
            }
            if (!seen.insert(current.value).second)
            {
                return {ResolveStatus::Invalid, {}, fmt::format("import {} has a cyclic outer chain", self.value)};
            }
            if (current.is_import())
            {
                const ImportEntry& entry = package.imports()[current.import_index()];
                chain.push_back({entry.object_name, text_of(package, entry.class_package),
                                 text_of(package, entry.class_name)});
                current = entry.outer;
            }
            else
            {
                const ExportEntry& entry = package.exports()[current.export_index()];
                if ((entry.export_flags & ExportFlag::ForcedExport) == 0)
                {
                    return {ResolveStatus::Invalid, {},
                            fmt::format("import {} is nested in export {}, which is not a forced export", self.value,
                                        current.value)};
                }
                chain.push_back({entry.object_name, "Core", package.class_name(current)});
                current = entry.outer;
            }
        }

        const std::string package_name = text_of(package, chain.back().name);
        const Package* target = find_package(package_name);
        if (!target)
        {
            const auto failed = m_failed.find(package_name);
            return {ResolveStatus::PackageNotFound, {},
                    failed != m_failed.end() ? fmt::format("package '{}' failed to load", package_name)
                                             : fmt::format("package '{}' is not in the search paths", package_name)};
        }
        if (chain.size() == 1)
        {
            return {ResolveStatus::Package, ObjectRef{target, ObjectIndex{}}, {}};
        }

        const PackageLookup& lookup = lookup_for(*target);
        auto export_class = [&](std::size_t export_index) {
            const ObjectIndex cls = target->exports()[export_index].class_index;
            return target->is_valid(cls) ? target->class_name(ObjectIndex::from_export(export_index))
                                         : std::string("<invalid>");
        };

        ObjectIndex outer{};
        for (std::size_t k = chain.size() - 1; k-- > 0;)
        {
            const Element& entry = chain[k];
            const std::string name = text_of(package, entry.name);
            const std::string& wanted_class = entry.class_name;
            const auto found = lookup.exports.find(name_key(outer.value, name));

            std::optional<std::size_t> match;
            if (found != lookup.exports.end())
            {
                for (const std::size_t candidate : found->second)
                {
                    if (Text::iequals(export_class(candidate), wanted_class))
                    {
                        match = candidate;
                        break;
                    }
                }
            }
            if (match)
            {
                outer = ObjectIndex::from_export(*match);
                continue;
            }

            const std::string path = package.object_path(self);
            const bool is_class_import =
                Text::iequals(entry.class_package, "Core") && Text::iequals(wanted_class, "Class");
            if (k == 0 && is_class_import)
            {
                // UE3 classes that exist in script always have a UClass export in their package; a class import
                // without one refers to a native-only (intrinsic) class.
                return {ResolveStatus::Intrinsic, {},
                        fmt::format("class '{}' has no export in '{}' (native-only class)", name, package_name)};
            }
            if (found != lookup.exports.end())
            {
                const std::size_t candidate = found->second.front();
                return {ResolveStatus::ClassMismatch, ObjectRef{target, ObjectIndex::from_export(candidate)},
                        fmt::format("'{}' expects class '{}' but '{}' has a '{}'", path, wanted_class, package_name,
                                    export_class(candidate))};
            }
            return {ResolveStatus::ObjectNotFound, {}, fmt::format("'{}' not found in package '{}'", path, package_name)};
        }
        return {ResolveStatus::Export, ObjectRef{target, outer}, {}};
    }

    std::string Linker::qualified_path(ObjectRef ref) const
    {
        if (!ref.valid())
        {
            return "None";
        }
        if (ref.index.is_null())
        {
            return ref.package->name();
        }
        if (ref.index.is_import())
        {
            return ref.package->object_path(ref.index);
        }
        // Objects inside a top-level forced export belong to the original package that export stands for.
        ObjectIndex outermost = ref.index;
        for (std::size_t steps = 0; steps <= ref.package->exports().size(); ++steps)
        {
            const ObjectIndex outer = ref.package->outer(outermost);
            if (!outer.is_export() || !ref.package->is_valid(outer))
            {
                break;
            }
            outermost = outer;
        }
        if ((ref.package->exports()[outermost.export_index()].export_flags & ExportFlag::ForcedExport) != 0)
        {
            return ref.package->object_path(ref.index);
        }
        return ref.package->name() + "." + ref.package->object_path(ref.index);
    }

    const Resolution& Linker::class_of(ObjectRef object)
    {
        if (!object.is_export())
        {
            throw Error(ErrorCode::InvalidArgument, "class_of() requires an export reference");
        }
        return resolve(*object.package, object.export_entry().class_index);
    }

    std::optional<bool> Linker::class_is_a(ObjectRef class_ref, std::string_view class_path)
    {
        std::unordered_set<const ExportEntry*> visited;
        while (class_ref.is_export())
        {
            if (Text::iequals(qualified_path(class_ref), class_path))
            {
                return true;
            }
            if (!visited.insert(&class_ref.export_entry()).second)
            {
                return std::nullopt; // cyclic super chain
            }
            const Resolution& super = resolve(*class_ref.package, class_ref.export_entry().super_index);
            if (super.status == ResolveStatus::Null)
            {
                return false; // reached the root (Core.Object)
            }
            if (super.status != ResolveStatus::Export)
            {
                return std::nullopt;
            }
            class_ref = super.target;
        }
        return std::nullopt;
    }

    std::optional<bool> Linker::is_a(ObjectRef object, std::string_view class_path)
    {
        const Resolution& cls = class_of(object);
        switch (cls.status)
        {
        case ResolveStatus::Null:
            // The object is itself a UClass.
            return Text::iequals(class_path, "Core.Class") || Text::iequals(class_path, "Core.State") ||
                   Text::iequals(class_path, "Core.Struct") || Text::iequals(class_path, "Core.Field") ||
                   Text::iequals(class_path, "Core.Object");
        case ResolveStatus::Intrinsic:
        {
            // Intrinsic classes are only known by name; their hierarchy is not in any package.
            const ObjectIndex class_index = object.export_entry().class_index;
            if (Text::iequals(object.package->object_path(class_index), class_path))
            {
                return true;
            }
            return Text::iequals(class_path, "Core.Component") ? std::optional<bool>(false) : std::nullopt;
        }
        case ResolveStatus::Export:
            return class_is_a(cls.target, class_path);
        default:
            return std::nullopt;
        }
    }

    std::optional<ObjectHeaderInput> Linker::header_input(ObjectRef object)
    {
        if (!object.is_export())
        {
            throw Error(ErrorCode::InvalidArgument, "header_input() requires an export reference");
        }
        const ExportEntry& entry = object.export_entry();
        ObjectHeaderInput input;
        input.object_flags = entry.object_flags;
        input.is_class = entry.class_index.is_null();

        // ClassDefaultObject on the object or any of its (export) outers.
        for (ObjectIndex current = object.index; current.is_export() && object.package->is_valid(current);
             current = object.package->exports()[current.export_index()].outer)
        {
            if ((object.package->exports()[current.export_index()].object_flags & ObjectFlag::ClassDefaultObject) != 0)
            {
                input.is_class_default_template = true;
                break;
            }
            if (current == object.package->exports()[current.export_index()].outer)
            {
                break; // self-outer guard
            }
        }

        const auto component = is_a(object, "Core.Component");
        if (!component)
        {
            return std::nullopt;
        }
        input.is_component = *component;
        return input;
    }

    ObjectHeader Linker::read_header(ObjectRef object)
    {
        const auto input = header_input(object);
        if (!input)
        {
            const Resolution& cls = class_of(object);
            throw Error(ErrorCode::NotFound,
                fmt::format("cannot determine the class hierarchy of '{}' (class {}: {})", qualified_path(object),
                            to_string(cls.status), cls.detail));
        }
        const auto data = object.package->export_data(object.index.export_index());
        BinaryReader reader(data, object.package->context(),
                            static_cast<std::uint64_t>(object.export_entry().serial_offset));
        return read_object_header(reader, *input);
    }
}
