#include <UnrealLib/Package/Package.hpp>

#include <algorithm>
#include <unordered_set>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Core/Log.hpp>
#include <UnrealLib/Package/CompressedChunk.hpp>
#include <UnrealLib/Serialization/Archive.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    namespace
    {
        // Lower bounds of serialized entry sizes for UE3 (>= v343) packages; used to reject absurd counts early.
        constexpr std::size_t k_min_name_entry_size = 4 + 4;   // empty FString + 32-bit flags (conservative)
        constexpr std::size_t k_min_import_entry_size = 4 * 4; // 3 names (index only) + outer
        constexpr std::size_t k_min_export_entry_size = 4 * 6; // conservative

        std::uint32_t checked_table(BinaryReader& reader, std::int32_t count, std::int32_t offset,
                                    std::size_t min_entry_size, std::string_view table)
        {
            if (count < 0)
            {
                throw PackageError(fmt::format("{} table has a negative count ({})", table, count));
            }
            if (count == 0)
            {
                return 0;
            }
            if (offset < 0 || static_cast<std::size_t>(offset) > reader.size())
            {
                throw PackageError(ErrorCode::OutOfBounds,
                    fmt::format("{} table offset {} is outside the package ({} bytes)", table, offset, reader.size()));
            }
            reader.seek(static_cast<std::size_t>(offset));
            const auto ucount = static_cast<std::uint32_t>(count);
            if (ucount > reader.context().limits.max_array_count)
            {
                throw PackageError(ErrorCode::LimitExceeded,
                    fmt::format("{} table count {} exceeds the limit", table, ucount));
            }
            if (static_cast<std::uint64_t>(ucount) * min_entry_size > reader.remaining())
            {
                throw PackageError(ErrorCode::OutOfBounds,
                    fmt::format("{} table with {} entries cannot fit in the remaining {} bytes", table, ucount,
                                reader.remaining()),
                    reader.absolute_position());
            }
            return ucount;
        }

        std::string stem_of(const std::filesystem::path& path)
        {
            const auto u8 = path.stem().u8string();
            return {reinterpret_cast<const char*>(u8.data()), u8.size()};
        }
    }

    Package Package::open(const std::filesystem::path& path, const LoadOptions& options)
    {
        Package package;
        package.load(path, options);
        return package;
    }

    void Package::load(const std::filesystem::path& path, const LoadOptions& options)
    {
        load_from_memory(read_file(path, options.limits.max_file_size), stem_of(path), options);
    }

    void Package::load_from_memory(std::vector<std::byte> bytes, std::string name, const LoadOptions& options)
    {
        *this = Package{};
        m_name = std::move(name);
        m_limits = options.limits;
        m_file_size = bytes.size();

        // 1. Summary.
        SerializationContext base_context;
        base_context.limits = options.limits;
        BinaryReader file_reader(bytes, base_context);
        m_summary = read_package_summary(file_reader);
        log(LogLevel::Debug, "{}: version {}/{}, engine {}, flags {}", m_name, m_summary.file_version,
            m_summary.licensee_version, m_summary.engine_version, describe_package_flags(m_summary.package_flags));

        // 2. Profile.
        const GameProfileRegistry& registry = options.registry ? *options.registry : GameProfileRegistry::builtin();
        if (options.profile && !options.profile->empty())
        {
            const GameProfile* profile = registry.find(*options.profile);
            if (!profile)
            {
                throw Error(ErrorCode::NotFound, fmt::format("unknown game profile '{}'", *options.profile));
            }
            m_profile = *profile;
            m_profile_reasons.push_back(fmt::format("selected explicitly: {}", profile->id));

            const ProfileDetection detected = registry.detect(m_summary);
            if (detected.profile != profile)
            {
                m_diagnostics.warning("profile.mismatch",
                    fmt::format("explicit profile '{}' does not match auto-detection ('{}') for version {}/{}",
                                profile->id, detected.profile ? detected.profile->id : std::string("none"),
                                m_summary.file_version, m_summary.licensee_version));
            }
        }
        else
        {
            ProfileDetection detected = registry.detect(m_summary);
            if (!detected.profile)
            {
                throw UnsupportedError(fmt::format("no game profile accepts package version {}/{}",
                                                   m_summary.file_version, m_summary.licensee_version));
            }
            m_profile = *detected.profile;
            m_profile_reasons = std::move(detected.reasons);
            if (!m_profile.verified)
            {
                m_diagnostics.warning("profile.unverified",
                    fmt::format("using unverified profile '{}' for version {}/{}", m_profile.id,
                                m_summary.file_version, m_summary.licensee_version));
            }
        }

        // 3. Decompression.
        if (m_summary.is_compressed())
        {
            const CompressionMethod method = compression_method_from_flags(m_summary.compression_flags);
            if (std::find(m_profile.compression_methods.begin(), m_profile.compression_methods.end(), method) ==
                m_profile.compression_methods.end())
            {
                m_diagnostics.warning("compression.unexpected-method",
                    fmt::format("profile '{}' does not list compression method '{}'", m_profile.id, to_string(method)));
            }
            m_data = decompress_package(bytes, m_summary, options.limits);
            bytes.clear();
            bytes.shrink_to_fit();
        }
        else
        {
            m_data = std::move(bytes);
        }

        // 4. Tables.
        parse_tables(options);
        m_loaded = true;
    }

    void Package::parse_tables(const LoadOptions& options)
    {
        SerializationContext ctx = m_summary.context();
        ctx.limits = m_limits;
        BinaryReader reader(m_data, ctx);

        {
            const std::uint32_t count =
                checked_table(reader, m_summary.name_count, m_summary.name_offset, k_min_name_entry_size, "name");
            std::vector<NameEntry> entries;
            entries.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                entries.push_back(read_name_entry(reader));
            }
            m_names = NameTable(std::move(entries));
        }

        {
            const std::uint32_t count = checked_table(reader, m_summary.import_count, m_summary.import_offset,
                                                      k_min_import_entry_size, "import");
            m_imports.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                m_imports.push_back(read_import_entry(reader));
            }
        }

        {
            const std::uint32_t count = checked_table(reader, m_summary.export_count, m_summary.export_offset,
                                                      k_min_export_entry_size, "export");
            m_exports.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                m_exports.push_back(read_export_entry(reader));
            }
        }

        if (options.read_depends && ctx.at_least(LegacyVersion::AddedDependsTable) && m_summary.depends_offset > 0 &&
            !m_exports.empty())
        {
            try
            {
                reader.seek(static_cast<std::size_t>(m_summary.depends_offset));
                std::vector<std::vector<ObjectIndex>> depends;
                depends.reserve(m_exports.size());
                for (std::size_t i = 0; i < m_exports.size(); ++i)
                {
                    const std::uint32_t count = reader.read_array_count(4);
                    std::vector<ObjectIndex> list;
                    list.reserve(count);
                    for (std::uint32_t k = 0; k < count; ++k)
                    {
                        list.push_back(ObjectIndex{reader.read_i32()});
                    }
                    depends.push_back(std::move(list));
                }
                m_depends = std::move(depends);
            }
            catch (const SerializationError& e)
            {
                m_diagnostics.warning("depends.unreadable",
                    fmt::format("depends table could not be read: {}", e.message()), e.offset());
            }
        }

        if (ctx.at_least(LegacyVersion::AddedThumbnailTable) && m_summary.thumbnail_table_offset > 0)
        {
            m_diagnostics.info("thumbnails.not-parsed", "thumbnail table is present but not parsed");
        }
        if (ctx.at_least(LegacyVersion::AddedImportExportGuidsTable) && m_summary.import_export_guids_offset > 0)
        {
            m_diagnostics.info("guids.not-parsed", "import/export GUID table is present but not parsed");
        }
    }

    void Package::require_loaded() const
    {
        if (!m_loaded)
        {
            throw Error(ErrorCode::InvalidState, "package is not loaded");
        }
    }

    const std::string& Package::name() const
    {
        require_loaded();
        return m_name;
    }

    const PackageSummary& Package::summary() const
    {
        require_loaded();
        return m_summary;
    }

    const GameProfile& Package::profile() const
    {
        require_loaded();
        return m_profile;
    }

    std::span<const std::string> Package::profile_reasons() const
    {
        require_loaded();
        return m_profile_reasons;
    }

    const NameTable& Package::names() const
    {
        require_loaded();
        return m_names;
    }

    std::span<const ImportEntry> Package::imports() const
    {
        require_loaded();
        return m_imports;
    }

    std::span<const ExportEntry> Package::exports() const
    {
        require_loaded();
        return m_exports;
    }

    const std::vector<std::vector<ObjectIndex>>& Package::depends() const
    {
        require_loaded();
        return m_depends;
    }

    std::uint64_t Package::file_size() const
    {
        require_loaded();
        return m_file_size;
    }

    std::span<const std::byte> Package::data() const
    {
        require_loaded();
        return m_data;
    }

    const Diagnostics& Package::diagnostics() const
    {
        require_loaded();
        return m_diagnostics;
    }

    SerializationContext Package::context() const
    {
        require_loaded();
        SerializationContext ctx = m_summary.context();
        ctx.limits = m_limits;
        return ctx;
    }

    BinaryReader Package::reader() const
    {
        return BinaryReader(data(), context());
    }

    bool Package::is_valid(ObjectIndex index) const
    {
        require_loaded();
        if (index.is_null())
        {
            return true;
        }
        if (index.is_export())
        {
            return index.export_index() < m_exports.size();
        }
        return index.import_index() < m_imports.size();
    }

    const FName& Package::object_fname(ObjectIndex index) const
    {
        if (!is_valid(index) || index.is_null())
        {
            throw Error(ErrorCode::OutOfBounds, fmt::format("object index {} is out of range", index.value));
        }
        return index.is_export() ? m_exports[index.export_index()].object_name
                                 : m_imports[index.import_index()].object_name;
    }

    std::string Package::object_name(ObjectIndex index) const
    {
        require_loaded();
        if (index.is_null())
        {
            return "None";
        }
        return m_names.to_string(object_fname(index));
    }

    std::string Package::class_name(ObjectIndex index) const
    {
        require_loaded();
        if (!is_valid(index) || index.is_null())
        {
            throw Error(ErrorCode::OutOfBounds, fmt::format("object index {} is out of range", index.value));
        }
        if (index.is_import())
        {
            return m_names.to_string(m_imports[index.import_index()].class_name);
        }
        const ObjectIndex cls = m_exports[index.export_index()].class_index;
        return cls.is_null() ? std::string("Class") : object_name(cls);
    }

    ObjectIndex Package::outer(ObjectIndex index) const
    {
        require_loaded();
        if (!is_valid(index) || index.is_null())
        {
            throw Error(ErrorCode::OutOfBounds, fmt::format("object index {} is out of range", index.value));
        }
        return index.is_export() ? m_exports[index.export_index()].outer : m_imports[index.import_index()].outer;
    }

    std::string Package::object_path(ObjectIndex index) const
    {
        require_loaded();
        if (index.is_null())
        {
            return "None";
        }
        std::vector<ObjectIndex> chain;
        std::unordered_set<std::int32_t> seen;
        bool cycle = false;
        bool broken = false;
        for (ObjectIndex current = index; !current.is_null();)
        {
            if (!is_valid(current))
            {
                broken = true;
                break;
            }
            if (!seen.insert(current.value).second)
            {
                cycle = true;
                break;
            }
            chain.push_back(current);
            current = outer(current);
        }

        std::string path;
        if (cycle)
        {
            path = "<cycle>";
        }
        else if (broken)
        {
            path = "<invalid>";
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            if (!path.empty())
            {
                path += '.';
            }
            const auto text = m_names.try_to_string(object_fname(*it));
            path += text ? *text : fmt::format("<name#{}>", object_fname(*it).index);
        }
        return path;
    }

    std::optional<ObjectIndex> Package::find_export(std::string_view name_or_path) const
    {
        require_loaded();
        std::optional<ObjectIndex> by_name;
        for (std::size_t i = 0; i < m_exports.size(); ++i)
        {
            const ObjectIndex index = ObjectIndex::from_export(i);
            if (Text::iequals(object_path(index), name_or_path))
            {
                return index;
            }
            if (!by_name)
            {
                const auto text = m_names.try_to_string(m_exports[i].object_name);
                if (text && Text::iequals(*text, name_or_path))
                {
                    by_name = index;
                }
            }
        }
        return by_name;
    }

    std::optional<ObjectIndex> Package::find_import(std::string_view name_or_path) const
    {
        require_loaded();
        std::optional<ObjectIndex> by_name;
        for (std::size_t i = 0; i < m_imports.size(); ++i)
        {
            const ObjectIndex index = ObjectIndex::from_import(i);
            if (Text::iequals(object_path(index), name_or_path))
            {
                return index;
            }
            if (!by_name)
            {
                const auto text = m_names.try_to_string(m_imports[i].object_name);
                if (text && Text::iequals(*text, name_or_path))
                {
                    by_name = index;
                }
            }
        }
        return by_name;
    }

    std::span<const std::byte> Package::export_data(std::size_t export_index) const
    {
        require_loaded();
        if (export_index >= m_exports.size())
        {
            throw Error(ErrorCode::OutOfBounds, fmt::format("export index {} is out of range", export_index));
        }
        const ExportEntry& e = m_exports[export_index];
        if (e.serial_offset < 0 || e.serial_size < 0 || static_cast<std::size_t>(e.serial_offset) > m_data.size() ||
            static_cast<std::size_t>(e.serial_size) > m_data.size() - static_cast<std::size_t>(e.serial_offset))
        {
            throw PackageError(ErrorCode::OutOfBounds,
                fmt::format("export {} data [{}, +{}) is outside the package ({} bytes)", export_index,
                            e.serial_offset, e.serial_size, m_data.size()));
        }
        return std::span<const std::byte>(m_data).subspan(static_cast<std::size_t>(e.serial_offset),
                                                          static_cast<std::size_t>(e.serial_size));
    }
}
