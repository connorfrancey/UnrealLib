#include <UnrealLib/Writer/PackageWriter.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Package/CompressedChunk.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Serialization/LegacyVersion.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    PackageModel PackageModel::from_package(const Package& package)
    {
        PackageModel m;
        m.name = package.name();
        m.summary = package.summary();
        m.names.assign(package.names().entries().begin(), package.names().entries().end());
        m.imports.assign(package.imports().begin(), package.imports().end());
        m.exports.assign(package.exports().begin(), package.exports().end());
        const SerializationContext ctx = package.context();
        if (ctx.at_least(LegacyVersion::AddedDependsTable) && !m.exports.empty())
        {
            if (package.depends().size() != m.exports.size())
            {
                throw Error(ErrorCode::InvalidArgument,
                            "the package's depends table is not loaded (LoadOptions::read_depends)");
            }
            m.depends = package.depends();
        }
        else
        {
            m.depends.assign(m.exports.size(), {});
        }
        m.export_data.reserve(m.exports.size());
        for (std::size_t i = 0; i < m.exports.size(); ++i)
        {
            const auto data = package.export_data(i);
            m.export_data.emplace_back(data.begin(), data.end());
        }

        // Header tail: from the end of the depends table (or export table) to the first export's data.
        const auto data = package.data();
        std::size_t tail_start = 0;
        if (ctx.at_least(LegacyVersion::AddedDependsTable) && m.summary.depends_offset > 0)
        {
            std::size_t size = 0;
            for (const auto& list : m.depends) size += 4 + list.size() * 4;
            tail_start = static_cast<std::size_t>(m.summary.depends_offset) + size;
        }
        std::size_t first_data = data.size();
        for (const ExportEntry& e : m.exports)
        {
            if (e.serial_size > 0) first_data = std::min(first_data, static_cast<std::size_t>(e.serial_offset));
        }
        if (tail_start > 0 && tail_start < first_data && first_data <= data.size())
        {
            m.header_tail.assign(data.begin() + static_cast<std::ptrdiff_t>(tail_start),
                                 data.begin() + static_cast<std::ptrdiff_t>(first_data));
        }
        return m;
    }

    std::int32_t PackageModel::add_name(std::string_view text, std::uint64_t flags)
    {
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (Text::iequals(names[i].text.text, text)) return static_cast<std::int32_t>(i);
        }
        NameEntry e;
        e.text = FString(std::string(text));
        e.flags = flags;
        names.push_back(std::move(e));
        return static_cast<std::int32_t>(names.size() - 1);
    }

    FName PackageModel::make_name(std::string_view text)
    {
        // "Base_N" (N without leading zeros) is stored as Base with number N + 1, as Unreal does.
        const std::size_t underscore = text.rfind('_');
        if (underscore != std::string_view::npos && underscore + 1 < text.size())
        {
            const std::string_view digits = text.substr(underscore + 1);
            int value = 0;
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
            if (ec == std::errc{} && ptr == digits.data() + digits.size() && (digits.size() == 1 || digits[0] != '0'))
            {
                return FName{add_name(text.substr(0, underscore)), value + 1};
            }
        }
        return FName{add_name(text), 0};
    }

    std::string PackageModel::name_text(const FName& n) const
    {
        if (n.index < 0 || static_cast<std::size_t>(n.index) >= names.size())
        {
            return fmt::format("<invalid name {}>", n.index);
        }
        const std::string& base = names[static_cast<std::size_t>(n.index)].text.text;
        return n.number > 0 ? fmt::format("{}_{}", base, n.number - 1) : base;
    }

    ObjectIndex PackageModel::add_import(std::string_view class_package, std::string_view class_name, ObjectIndex outer,
                                         std::string_view object_name)
    {
        ImportEntry e;
        e.class_package = make_name(class_package);
        e.class_name = make_name(class_name);
        e.outer = outer;
        e.object_name = make_name(object_name);
        imports.push_back(e);
        return ObjectIndex::from_import(imports.size() - 1);
    }

    std::optional<ObjectIndex> PackageModel::find_import(std::string_view class_name, ObjectIndex outer,
                                                         std::string_view object_name) const
    {
        for (std::size_t i = 0; i < imports.size(); ++i)
        {
            const ImportEntry& e = imports[i];
            if (e.outer == outer && Text::iequals(name_text(e.class_name), class_name) &&
                Text::iequals(name_text(e.object_name), object_name))
            {
                return ObjectIndex::from_import(i);
            }
        }
        return std::nullopt;
    }

    ObjectIndex PackageModel::add_export(ExportEntry entry, std::vector<std::byte> data)
    {
        entry.serial_size = static_cast<std::int32_t>(data.size());
        exports.push_back(std::move(entry));
        export_data.push_back(std::move(data));
        depends.emplace_back();
        return ObjectIndex::from_export(exports.size() - 1);
    }

    std::string PackageModel::object_path(ObjectIndex index) const
    {
        std::string path;
        for (int depth = 0; !index.is_null() && depth < 256; ++depth)
        {
            FName n;
            ObjectIndex outer;
            if (index.is_export() && index.export_index() < exports.size())
            {
                n = exports[index.export_index()].object_name;
                outer = exports[index.export_index()].outer;
            }
            else if (index.is_import() && index.import_index() < imports.size())
            {
                n = imports[index.import_index()].object_name;
                outer = imports[index.import_index()].outer;
            }
            else
            {
                return path.empty() ? "<invalid>" : "<invalid>." + path;
            }
            path = path.empty() ? name_text(n) : name_text(n) + "." + path;
            index = outer;
        }
        return path;
    }

    std::optional<ObjectIndex> PackageModel::find_export(std::string_view path) const
    {
        for (std::size_t i = 0; i < exports.size(); ++i)
        {
            if (Text::iequals(object_path(ObjectIndex::from_export(i)), path)) return ObjectIndex::from_export(i);
        }
        return std::nullopt;
    }

    void PackageModel::rename(ObjectIndex index, std::string_view new_name)
    {
        const FName n = make_name(new_name);
        if (index.is_export() && index.export_index() < exports.size()) exports[index.export_index()].object_name = n;
        else if (index.is_import() && index.import_index() < imports.size()) imports[index.import_index()].object_name = n;
        else throw Error(ErrorCode::InvalidArgument, fmt::format("rename: invalid object index {}", index.value));
    }

    void PackageModel::validate() const
    {
        const auto fail = [](const std::string& message) { throw Error(ErrorCode::InvalidState, message); };
        if (export_data.size() != exports.size()) fail("export_data must have one entry per export");
        if (depends.size() != exports.size()) fail("depends must have one list per export");
        const auto valid_index = [&](ObjectIndex i) {
            return i.is_null() || (i.is_export() && i.export_index() < exports.size()) ||
                   (i.is_import() && i.import_index() < imports.size());
        };
        const auto valid_name = [&](const FName& n) {
            return n.index >= 0 && static_cast<std::size_t>(n.index) < names.size() && n.number >= 0;
        };
        for (std::size_t i = 0; i < imports.size(); ++i)
        {
            const ImportEntry& e = imports[i];
            if (!valid_index(e.outer) || !valid_name(e.class_package) || !valid_name(e.class_name) ||
                !valid_name(e.object_name))
            {
                fail(fmt::format("import {} has an invalid reference", i));
            }
        }
        for (std::size_t i = 0; i < exports.size(); ++i)
        {
            const ExportEntry& e = exports[i];
            if (!valid_index(e.class_index) || !valid_index(e.super_index) || !valid_index(e.outer) ||
                !valid_index(e.archetype) || !valid_name(e.object_name))
            {
                fail(fmt::format("export {} has an invalid reference", i));
            }
            for (const ComponentMapEntry& c : e.component_map)
            {
                if (!valid_name(c.name) || !valid_index(c.object)) fail(fmt::format("export {} component map", i));
            }
            for (const ObjectIndex d : depends[i])
            {
                if (!valid_index(d)) fail(fmt::format("export {} depends on invalid index {}", i, d.value));
            }
        }
    }

    namespace
    {
        /// Compresses @p body into UE3 compressed chunks; returns the chunk records (compressed offsets relative to
        /// the start of @p out, which receives the chunk data).
        std::vector<CompressedChunkInfo> compress_body(std::span<const std::byte> body, std::size_t body_offset,
                                                       const WriteOptions& options, BinaryWriter& out,
                                                       const SerializationContext& context)
        {
            std::vector<CompressedChunkInfo> chunks;
            for (std::size_t start = 0; start < body.size(); start += static_cast<std::size_t>(options.chunk_size))
            {
                const std::size_t size = std::min(body.size() - start, static_cast<std::size_t>(options.chunk_size));
                const auto chunk_in = body.subspan(start, size);
                CompressedChunkInfo info;
                info.uncompressed_offset = static_cast<std::int32_t>(body_offset + start);
                info.uncompressed_size = static_cast<std::int32_t>(size);
                info.compressed_offset = static_cast<std::int32_t>(out.position()); // relative; fixed up by caller

                std::vector<std::vector<std::byte>> blocks;
                for (std::size_t b = 0; b < size; b += static_cast<std::size_t>(options.block_size))
                {
                    const auto block = chunk_in.subspan(b, std::min(size - b, static_cast<std::size_t>(options.block_size)));
                    blocks.push_back(compress(options.compression, block));
                }
                const std::size_t header_start = out.position();
                out.write_u32(package_signature);
                out.write_i32(options.block_size);
                std::size_t total = 0;
                for (const auto& b : blocks) total += b.size();
                out.write_i32(static_cast<std::int32_t>(total));
                out.write_i32(static_cast<std::int32_t>(size));
                for (std::size_t b = 0; b < blocks.size(); ++b)
                {
                    out.write_i32(static_cast<std::int32_t>(blocks[b].size()));
                    out.write_i32(static_cast<std::int32_t>(
                        std::min(size - b * static_cast<std::size_t>(options.block_size),
                                 static_cast<std::size_t>(options.block_size))));
                }
                for (const auto& b : blocks) out.write_bytes(b);
                info.compressed_size = static_cast<std::int32_t>(out.position() - header_start);
                chunks.push_back(info);
            }
            (void)context;
            return chunks;
        }
    }

    std::vector<std::byte> write_package(const PackageModel& model, const WriteOptions& options)
    {
        model.validate();
        if (options.compression != CompressionMethod::None && !can_compress(options.compression))
        {
            throw UnsupportedError(fmt::format("cannot write {}-compressed packages", to_string(options.compression)));
        }
        if (options.chunk_size <= 0 || options.block_size <= 0)
        {
            throw Error(ErrorCode::InvalidArgument, "chunk and block sizes must be positive");
        }

        PackageSummary summary = model.summary;
        const SerializationContext ctx = summary.context();
        const std::int32_t old_names = summary.name_count;
        const std::int32_t old_exports = summary.export_count;
        summary.name_count = static_cast<std::int32_t>(model.names.size());
        summary.import_count = static_cast<std::int32_t>(model.imports.size());
        summary.export_count = static_cast<std::int32_t>(model.exports.size());
        summary.compression_flags = 0;
        summary.compressed_chunks.clear();
        // The current generation records the counts it was saved with.
        if (!summary.generations.empty() && summary.generations.back().name_count == old_names &&
            summary.generations.back().export_count == old_exports)
        {
            summary.generations.back().name_count = summary.name_count;
            summary.generations.back().export_count = summary.export_count;
        }

        // Pass 1 computes the layout with placeholder offsets (no field's size depends on an offset value); pass 2
        // writes the final values.
        std::vector<ExportEntry> exports = model.exports;
        const auto layout = [&](BinaryWriter& w) {
            write_package_summary(w, summary);
            summary.name_offset = static_cast<std::int32_t>(w.position());
            for (const NameEntry& n : model.names) write_name_entry(w, n);
            summary.import_offset = static_cast<std::int32_t>(w.position());
            for (const ImportEntry& i : model.imports) write_import_entry(w, i);
            summary.export_offset = static_cast<std::int32_t>(w.position());
            for (const ExportEntry& e : exports) write_export_entry(w, e);
            if (ctx.at_least(LegacyVersion::AddedDependsTable))
            {
                summary.depends_offset = static_cast<std::int32_t>(w.position());
                for (const auto& list : model.depends)
                {
                    w.write_i32(static_cast<std::int32_t>(list.size()));
                    for (const ObjectIndex d : list) w.write_i32(d.value);
                }
            }
            w.write_bytes(model.header_tail);
            summary.header_size = static_cast<std::int32_t>(w.position());
            for (std::size_t i = 0; i < exports.size(); ++i)
            {
                exports[i].serial_offset = static_cast<std::int32_t>(w.position()); // UE3 SavePackage: Ar.Tell(), even if empty
                exports[i].serial_size = static_cast<std::int32_t>(model.export_data[i].size());
                w.write_bytes(model.export_data[i]);
            }
        };
        {
            BinaryWriter probe(ctx);
            layout(probe);
        }
        BinaryWriter writer(ctx);
        layout(writer);
        std::vector<std::byte> image = writer.take();
        if (options.compression == CompressionMethod::None)
        {
            return image;
        }

        // Compressed: summary (with chunk table) + chunks of everything after the summary.
        BinaryWriter summary_probe(ctx);
        write_package_summary(summary_probe, summary);
        const std::size_t body_offset = summary_probe.position();
        const auto body = std::span<const std::byte>(image).subspan(body_offset);
        BinaryWriter chunk_data(ctx);
        std::vector<CompressedChunkInfo> chunks = compress_body(body, body_offset, options, chunk_data, ctx);

        summary.compression_flags = options.compression == CompressionMethod::Zlib ? 0x1u : 0x2u;
        summary.compressed_chunks = chunks;
        BinaryWriter header(ctx);
        write_package_summary(header, summary); // sized with the final chunk count
        const std::size_t data_start = header.position();
        for (CompressedChunkInfo& c : summary.compressed_chunks)
        {
            c.compressed_offset += static_cast<std::int32_t>(data_start);
        }
        BinaryWriter out(ctx);
        write_package_summary(out, summary);
        out.write_bytes(chunk_data.take());
        return out.take();
    }

    void save_package(const PackageModel& model, const std::filesystem::path& path, const WriteOptions& options)
    {
        const std::vector<std::byte> bytes = write_package(model, options);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            throw IoError(fmt::format("cannot open '{}' for writing", path.string()));
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            throw IoError(fmt::format("cannot write '{}'", path.string()));
        }
    }

    void replace_properties(PackageModel& model, ObjectIndex index, std::size_t properties_offset,
                            std::size_t properties_end, const std::vector<Property>& properties)
    {
        if (!index.is_export() || index.export_index() >= model.exports.size())
        {
            throw Error(ErrorCode::InvalidArgument, "replace_properties: not an export");
        }
        std::vector<std::byte>& data = model.export_data[index.export_index()];
        if (properties_offset > properties_end || properties_end > data.size())
        {
            throw Error(ErrorCode::InvalidArgument,
                        fmt::format("replace_properties: range [{}, {}) outside {} bytes", properties_offset,
                                    properties_end, data.size()));
        }
        const std::optional<std::int32_t> none = [&]() -> std::optional<std::int32_t> {
            for (std::size_t i = 0; i < model.names.size(); ++i)
            {
                if (Text::iequals(model.names[i].text.text, "None")) return static_cast<std::int32_t>(i);
            }
            return std::nullopt;
        }();
        const FName none_name{none ? *none : model.add_name("None"), 0};
        BinaryWriter w(model.summary.context());
        write_properties(w, properties, none_name);
        const std::vector<std::byte> encoded = w.take();
        std::vector<std::byte> out;
        out.reserve(data.size() - (properties_end - properties_offset) + encoded.size());
        out.insert(out.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(properties_offset));
        out.insert(out.end(), encoded.begin(), encoded.end());
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(properties_end), data.end());
        data = std::move(out);
        model.exports[index.export_index()].serial_size = static_cast<std::int32_t>(data.size());
    }
}
