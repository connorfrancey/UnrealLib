#pragma once

/// @file
/// @brief Random-access byte sources: in-memory and file-backed.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UnrealLib
{
    /// @brief A read-only, random-access source of bytes.
    class Archive
    {
    public:
        virtual ~Archive() = default;
        Archive(const Archive&) = delete;
        Archive& operator=(const Archive&) = delete;

        /// @brief Total number of bytes.
        [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

        /// @brief Copies `out.size()` bytes starting at @p offset into @p out.
        /// @throws SerializationError (OutOfBounds) if the range exceeds size(); IoError on read failure.
        virtual void read_at(std::uint64_t offset, std::span<std::byte> out) = 0;

        /// @brief A descriptive name (file path or user-supplied label) used in diagnostics.
        [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    protected:
        Archive() = default;
        Archive(Archive&&) = default;            ///< Move-constructs.
        Archive& operator=(Archive&&) = default; ///< Move-assigns.

        /// @brief Throws if `[offset, offset + count)` is not within size().
        void check_range(std::uint64_t offset, std::uint64_t count) const;
    };

    /// @brief An Archive backed by an owned in-memory buffer.
    class MemoryArchive final : public Archive
    {
    public:
        /// @param bytes Buffer to take ownership of.
        /// @param name  Label used in diagnostics.
        explicit MemoryArchive(std::vector<std::byte> bytes, std::string name = "<memory>");

        [[nodiscard]] std::uint64_t size() const noexcept override { return m_bytes.size(); }
        void read_at(std::uint64_t offset, std::span<std::byte> out) override;
        [[nodiscard]] std::string_view name() const noexcept override { return m_name; }

        /// @brief Direct access to the whole buffer.
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return m_bytes; }

        /// @brief Releases the buffer.
        [[nodiscard]] std::vector<std::byte> release() noexcept { return std::move(m_bytes); }

    private:
        std::vector<std::byte> m_bytes;
        std::string m_name;
    };

    /// @brief An Archive that reads lazily from a file.
    /// @note Not thread-safe: concurrent read_at() calls on one instance must be externally synchronized.
    class FileArchive final : public Archive
    {
    public:
        /// @brief Opens @p path for reading. Throws IoError if it cannot be opened.
        explicit FileArchive(const std::filesystem::path& path);

        [[nodiscard]] std::uint64_t size() const noexcept override { return m_size; }
        void read_at(std::uint64_t offset, std::span<std::byte> out) override;
        [[nodiscard]] std::string_view name() const noexcept override { return m_name; }

        /// @brief The opened path.
        [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    private:
        std::filesystem::path m_path;
        std::string m_name;
        std::ifstream m_stream;
        std::uint64_t m_size = 0;
    };

    /// @brief Reads an entire file into memory.
    /// @param path     File to read.
    /// @param max_size Files larger than this are rejected with SerializationError (LimitExceeded).
    /// @throws IoError if the file cannot be opened or read.
    [[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path, std::uint64_t max_size);
}
