#include <UnrealLib/Serialization/Archive.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

#include <fmt/format.h>
#ifdef _WIN32
#include <mio/mmap.hpp>
#endif

#include <UnrealLib/Core/Error.hpp>

namespace UnrealLib
{
    namespace
    {
        std::string path_to_utf8(const std::filesystem::path& path)
        {
            const auto u8 = path.u8string();
            return {reinterpret_cast<const char*>(u8.data()), u8.size()};
        }
    }

    void Archive::check_range(std::uint64_t offset, std::uint64_t count) const
    {
        const std::uint64_t total = size();
        if (offset > total || count > total - offset)
        {
            throw SerializationError(ErrorCode::OutOfBounds,
                fmt::format("range [{}, +{}) exceeds archive '{}' ({} bytes)", offset, count, name(), total), offset);
        }
    }

    MemoryArchive::MemoryArchive(std::vector<std::byte> bytes, std::string name)
        : m_bytes(std::move(bytes)), m_name(std::move(name))
    {
    }

    void MemoryArchive::read_at(std::uint64_t offset, std::span<std::byte> out)
    {
        check_range(offset, out.size());
        if (!out.empty())
        {
            std::memcpy(out.data(), m_bytes.data() + offset, out.size());
        }
    }

    FileArchive::FileArchive(const std::filesystem::path& path)
        : m_path(path), m_name(path_to_utf8(path)), m_stream(path, std::ios::binary)
    {
        if (!m_stream)
        {
            throw IoError(fmt::format("cannot open '{}'", m_name));
        }
        std::error_code ec;
        const auto file_size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            throw IoError(fmt::format("cannot determine the size of '{}': {}", m_name, ec.message()));
        }
        m_size = file_size;
    }

    void FileArchive::read_at(std::uint64_t offset, std::span<std::byte> out)
    {
        check_range(offset, out.size());
        if (out.empty())
        {
            return;
        }
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        {
            throw SerializationError(ErrorCode::OutOfBounds, "offset not representable as a stream offset", offset);
        }
        m_stream.clear();
        m_stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        m_stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (static_cast<std::size_t>(m_stream.gcount()) != out.size())
        {
            throw IoError(fmt::format("short read from '{}' at offset {}", m_name, offset));
        }
    }

    std::vector<std::byte> read_file(const std::filesystem::path& path, std::uint64_t max_size)
    {
        FileArchive archive(path);
        const auto limit = std::min<std::uint64_t>(max_size, std::vector<std::byte>().max_size());
        if (archive.size() > limit)
        {
            throw SerializationError(ErrorCode::LimitExceeded,
                fmt::format("file '{}' is {} bytes, above the limit of {} bytes", archive.name(), archive.size(),
                            limit));
        }
        const auto size = static_cast<std::size_t>(archive.size());
#ifdef _WIN32
        // Windows prevents truncation of a mapped file. Stream reads on POSIX can report short reads safely.
        if (size != 0)
        {
            std::error_code error;
            const auto mapping = mio::make_mmap_source(path.native(), 0, size, error);
            if (!error)
            {
                const auto mapped = std::as_bytes(std::span(mapping.data(), size));
                return {mapped.begin(), mapped.end()};
            }
        }
#endif
        std::vector<std::byte> bytes(size);
        archive.read_at(0, bytes);
        return bytes;
    }
}
