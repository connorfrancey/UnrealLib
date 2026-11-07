#include "SyntheticPackage.hpp"

#include <algorithm>
#include <stdexcept>

#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Package/CompressedChunk.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

namespace UnrealLib::Testing
{
    namespace
    {
        FName name_ref(const std::vector<std::string>& names, const std::string& text, std::int32_t number = 0)
        {
            const auto it = std::find(names.begin(), names.end(), text);
            if (it == names.end())
            {
                throw std::logic_error("synthetic package: unknown name " + text);
            }
            return FName{static_cast<std::int32_t>(it - names.begin()), number};
        }

        PackageSummary make_summary(const SyntheticPackageSpec& spec)
        {
            PackageSummary s;
            s.file_version = spec.file_version;
            s.licensee_version = spec.licensee_version;
            s.folder_name = FString("None");
            s.package_flags = spec.package_flags;
            s.name_count = static_cast<std::int32_t>(spec.names.size());
            s.import_count = static_cast<std::int32_t>(spec.imports.size());
            s.export_count = static_cast<std::int32_t>(spec.exports.size());
            s.guid = Guid{0x11111111, 0x22222222, 0x33333333, 0x44444444};
            s.generations.push_back(GenerationInfo{s.export_count, s.name_count, s.export_count});
            s.engine_version = spec.engine_version;
            s.cooker_version = spec.cooker_version;
            s.package_source = 0xC082C6B6u;
            return s;
        }
    }

    FName intern(SyntheticPackageSpec& spec, const std::string& text, std::int32_t number)
    {
        const auto it = std::find(spec.names.begin(), spec.names.end(), text);
        if (it != spec.names.end())
        {
            return FName{static_cast<std::int32_t>(it - spec.names.begin()), number};
        }
        spec.names.push_back(text);
        return FName{static_cast<std::int32_t>(spec.names.size() - 1), number};
    }

    ObjectIndex add_import(SyntheticPackageSpec& spec, const std::string& class_package, const std::string& class_name,
                           ObjectIndex outer, const std::string& name)
    {
        spec.imports.push_back(
            ImportEntry{intern(spec, class_package), intern(spec, class_name), outer, intern(spec, name)});
        return ObjectIndex::from_import(spec.imports.size() - 1);
    }

    ObjectIndex add_export(SyntheticPackageSpec& spec, ObjectIndex class_index, ObjectIndex super_index,
                           ObjectIndex outer, const std::string& name, std::uint64_t flags,
                           std::vector<std::byte> payload)
    {
        SyntheticExport e;
        e.entry.class_index = class_index;
        e.entry.super_index = super_index;
        e.entry.outer = outer;
        e.entry.object_name = intern(spec, name);
        e.entry.object_flags = flags;
        e.entry.generation_net_object_count = {0};
        e.payload = std::move(payload);
        spec.exports.push_back(std::move(e));
        return ObjectIndex::from_export(spec.exports.size() - 1);
    }

    SyntheticPackageSpec make_basic_spec()
    {
        SyntheticPackageSpec spec;
        spec.names = {"None", "Core", "Class", "Package", "Object", "TestPkg", "MyObject"};
        const auto& n = spec.names;

        spec.imports.push_back(ImportEntry{name_ref(n, "Core"), name_ref(n, "Package"), ObjectIndex{0},
                                           name_ref(n, "Core")});
        spec.imports.push_back(ImportEntry{name_ref(n, "Core"), name_ref(n, "Class"), ObjectIndex::from_import(0),
                                           name_ref(n, "Object")});

        // Export 0: a "MyObject" of class Core.Object at top level.
        SyntheticExport e0;
        e0.entry.class_index = ObjectIndex::from_import(1);
        e0.entry.object_name = name_ref(n, "MyObject");
        e0.entry.object_flags = 0x0007000400000000ull;
        e0.entry.generation_net_object_count = {0};
        e0.payload = bytes_of({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
        e0.depends = {ObjectIndex::from_import(1)};

        // Export 1: "MyObject_2" (name number 3) nested in export 0.
        SyntheticExport e1;
        e1.entry.class_index = ObjectIndex::from_import(1);
        e1.entry.outer = ObjectIndex::from_export(0);
        e1.entry.object_name = name_ref(n, "MyObject", 3);
        e1.entry.archetype = ObjectIndex::from_export(0);
        e1.entry.generation_net_object_count = {0};
        e1.payload.assign(300, std::byte{0xAB});

        // Export 2: a class-less export (a UClass), no payload.
        SyntheticExport e2;
        e2.entry.object_name = name_ref(n, "TestPkg");
        e2.entry.generation_net_object_count = {0};

        spec.exports = {e0, e1, e2};
        return spec;
    }

    SyntheticPackage build_synthetic_package(const SyntheticPackageSpec& spec)
    {
        const bool compressed = spec.compression != CompressionMethod::None;
        PackageSummary summary = make_summary(spec);
        if (compressed)
        {
            summary.compression_flags = spec.compression == CompressionMethod::Zlib
                                            ? static_cast<std::uint32_t>(CompressionFlag::Zlib)
                                            : static_cast<std::uint32_t>(CompressionFlag::Lzo);
            summary.compressed_chunks.resize(1); // placeholder so the summary size is final
        }

        // Pass 1: measure the summary.
        BinaryWriter measure;
        write_package_summary(measure, summary);
        const auto summary_size = static_cast<std::int32_t>(measure.size());

        // Body (uncompressed image after the summary).
        BinaryWriter body(summary.context());
        auto absolute = [&](std::size_t body_position) {
            return static_cast<std::int32_t>(summary_size + static_cast<std::int32_t>(body_position));
        };

        summary.name_offset = absolute(body.position());
        for (const auto& text : spec.names)
        {
            write_name_entry(body, NameEntry{FString(text), 0x0007001000000000ull});
        }
        summary.import_offset = absolute(body.position());
        for (const auto& entry : spec.imports)
        {
            write_import_entry(body, entry);
        }

        // Exports: serial offsets are known only after the header; write placeholders and patch afterwards.
        summary.export_offset = absolute(body.position());
        std::vector<ExportEntry> exports;
        exports.reserve(spec.exports.size());
        for (const auto& e : spec.exports)
        {
            exports.push_back(e.entry);
        }
        const std::size_t export_table_start = body.position();
        for (const auto& e : exports)
        {
            write_export_entry(body, e);
        }

        summary.depends_offset = absolute(body.position());
        for (const auto& e : spec.exports)
        {
            body.write_i32(static_cast<std::int32_t>(e.depends.size()));
            for (const auto& d : e.depends)
            {
                body.write_i32(d.value);
            }
        }
        summary.header_size = absolute(body.position());

        for (std::size_t i = 0; i < spec.exports.size(); ++i)
        {
            exports[i].serial_size = static_cast<std::int32_t>(spec.exports[i].payload.size());
            exports[i].serial_offset = absolute(body.position());
            body.write_bytes(spec.exports[i].payload);
        }
        // Rewrite the export table with final offsets (entry sizes are unchanged).
        const std::size_t end = body.position();
        body.seek(export_table_start);
        for (const auto& e : exports)
        {
            write_export_entry(body, e);
        }
        body.seek(end);

        const std::vector<std::byte> body_bytes(body.data().begin(), body.data().end());

        SyntheticPackage result;
        if (!compressed)
        {
            BinaryWriter file;
            write_package_summary(file, summary);
            file.write_bytes(body_bytes);
            result.file = file.take();
            result.uncompressed = result.file;
            result.summary = summary;
            return result;
        }

        // Compressed chunk: header + blocks, placed directly after the summary.
        if (spec.compression != CompressionMethod::Zlib)
        {
            throw std::logic_error("synthetic package: only zlib compression can be produced");
        }
        BinaryWriter chunk(summary.context());
        std::vector<std::vector<std::byte>> blocks;
        std::vector<std::int32_t> block_sizes;
        for (std::size_t offset = 0; offset < body_bytes.size(); offset += static_cast<std::size_t>(spec.block_size))
        {
            const std::size_t length =
                std::min(static_cast<std::size_t>(spec.block_size), body_bytes.size() - offset);
            blocks.push_back(compress_zlib(std::span<const std::byte>(body_bytes).subspan(offset, length)));
            block_sizes.push_back(static_cast<std::int32_t>(length));
        }
        std::int32_t compressed_total = 0;
        for (const auto& b : blocks)
        {
            compressed_total += static_cast<std::int32_t>(b.size());
        }
        chunk.write_u32(package_signature);
        chunk.write_i32(spec.block_size);
        chunk.write_i32(compressed_total);
        chunk.write_i32(static_cast<std::int32_t>(body_bytes.size()));
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            chunk.write_i32(static_cast<std::int32_t>(blocks[i].size()));
            chunk.write_i32(block_sizes[i]);
        }
        for (const auto& b : blocks)
        {
            chunk.write_bytes(b);
        }

        summary.compressed_chunks[0] = CompressedChunkInfo{summary_size, static_cast<std::int32_t>(body_bytes.size()),
                                                           summary_size, static_cast<std::int32_t>(chunk.size())};

        BinaryWriter file;
        write_package_summary(file, summary);
        file.write_bytes(chunk.data());
        result.file = file.take();

        BinaryWriter image;
        write_package_summary(image, summary);
        image.write_bytes(body_bytes);
        result.uncompressed = image.take();
        result.summary = summary;
        return result;
    }
}
