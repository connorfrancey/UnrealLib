#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/Archive.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

namespace
{
    class TempFile
    {
    public:
        explicit TempFile(const std::vector<std::byte>& contents)
        {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            m_path = std::filesystem::temp_directory_path() /
                     (std::string("unreallib_") + info->test_suite_name() + "_" + info->name() + ".bin");
            std::ofstream out(m_path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
        }
        ~TempFile()
        {
            std::error_code ec;
            std::filesystem::remove(m_path, ec);
        }
        TempFile(const TempFile&) = delete;
        TempFile& operator=(const TempFile&) = delete;
        [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

    private:
        std::filesystem::path m_path;
    };
}

TEST(ArchiveTests, MemoryArchiveReadsRanges)
{
    MemoryArchive archive(bytes_of({1, 2, 3, 4, 5}), "test");
    EXPECT_EQ(archive.size(), 5u);
    EXPECT_EQ(archive.name(), "test");
    std::vector<std::byte> out(2);
    archive.read_at(3, out);
    EXPECT_EQ(out, bytes_of({4, 5}));
    EXPECT_THROW(archive.read_at(4, out), SerializationError);
    std::vector<std::byte> none;
    archive.read_at(5, none);
    EXPECT_THROW(archive.read_at(6, none), SerializationError);
}

TEST(ArchiveTests, FileArchiveReadsRanges)
{
    const TempFile file(bytes_of({10, 20, 30, 40}));
    FileArchive archive(file.path());
    EXPECT_EQ(archive.size(), 4u);
    std::vector<std::byte> out(3);
    archive.read_at(1, out);
    EXPECT_EQ(out, bytes_of({20, 30, 40}));
    EXPECT_THROW(archive.read_at(2, out), SerializationError);
}

TEST(ArchiveTests, FileArchiveMissingFileThrowsIoError)
{
    EXPECT_THROW(FileArchive(std::filesystem::path("this/file/does/not/exist.upk")), IoError);
}

TEST(ArchiveTests, ReadFileEnforcesLimit)
{
    const TempFile file(bytes_of({1, 2, 3, 4, 5, 6, 7, 8}));
    EXPECT_EQ(read_file(file.path(), 8).size(), 8u);
    try
    {
        (void)read_file(file.path(), 7);
        FAIL();
    }
    catch (const SerializationError& e)
    {
        EXPECT_EQ(e.code(), ErrorCode::LimitExceeded);
    }
}
