// Unit tests for the Assets module: bulk data records, pixel decoders, image encoders, audio detection.

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include <UnrealLib/Assets/BulkData.hpp>
#include <UnrealLib/Assets/Image.hpp>
#include <UnrealLib/Assets/Sound.hpp>
#include <UnrealLib/Assets/Texture.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

using namespace UnrealLib;
using namespace UnrealLib::Assets;

namespace
{
    std::vector<std::byte> bytes(std::initializer_list<int> values)
    {
        std::vector<std::byte> out;
        for (const int v : values) out.push_back(static_cast<std::byte>(v));
        return out;
    }

    std::array<std::uint8_t, 4> pixel(Image& image, std::uint32_t x, std::uint32_t y)
    {
        const std::uint8_t* p = image.at(x, y);
        return {p[0], p[1], p[2], p[3]};
    }
}

TEST(BulkDataTests, InlineRecordRoundTrip)
{
    const auto payload = bytes({1, 2, 3, 4, 5});
    BulkData record;
    record.element_count = 5;
    BinaryWriter writer;
    writer.write_u32(0xAABBCCDD); // something before the record
    write_bulk_data(writer, record, payload, 100);
    EXPECT_EQ(record.size_on_disk, 5);
    EXPECT_EQ(record.offset_in_file, 100 + 4 + 16);

    const auto data = writer.take();
    BinaryReader reader(data, {}, 100);
    reader.seek(4);
    const BulkData read = read_bulk_data(reader);
    EXPECT_TRUE(reader.at_end());
    EXPECT_EQ(read.flags, 0u);
    EXPECT_EQ(read.element_count, 5);
    EXPECT_EQ(read.size_on_disk, 5);
    EXPECT_EQ(read.payload_offset, 120u);
    EXPECT_FALSE(read.offset_mismatch());
    EXPECT_TRUE(read.is_inline());
    EXPECT_TRUE(read.has_payload());
}

TEST(BulkDataTests, StaleOffsetIsToleratedAndReported)
{
    BinaryWriter writer;
    writer.write_u32(0);
    writer.write_i32(2);
    writer.write_i32(2);
    writer.write_i32(9999); // wrong offset; payload follows the record anyway
    writer.write_bytes(bytes({7, 8}));
    const auto data = writer.take();
    BinaryReader reader(data);
    const BulkData read = read_bulk_data(reader);
    EXPECT_TRUE(reader.at_end());
    EXPECT_EQ(read.payload_offset, 16u);
    EXPECT_TRUE(read.offset_mismatch());
}

TEST(BulkDataTests, SeparateFileRecordHasNoInlinePayload)
{
    BinaryWriter writer;
    writer.write_u32(BulkDataFlag::StoreInSeparateFile | BulkDataFlag::CompressedLzo);
    writer.write_i32(1024);
    writer.write_i32(300);
    writer.write_i32(123456);
    const auto data = writer.take();
    BinaryReader reader(data);
    const BulkData read = read_bulk_data(reader);
    EXPECT_TRUE(reader.at_end());
    EXPECT_FALSE(read.is_inline());
    EXPECT_TRUE(read.is_compressed());
    EXPECT_EQ(read.describe_flags(), "StoreInSeparateFile|CompressedLzo");
}

TEST(BulkDataTests, RejectsTruncatedAndNegativeRecords)
{
    BinaryWriter writer;
    writer.write_u32(0);
    writer.write_i32(10);
    writer.write_i32(10);
    writer.write_i32(16);
    writer.write_bytes(bytes({1, 2, 3})); // 7 bytes missing
    const auto truncated = writer.take();
    BinaryReader reader(truncated);
    EXPECT_THROW((void)read_bulk_data(reader), SerializationError);

    BinaryWriter negative;
    negative.write_u32(0);
    negative.write_i32(-1);
    negative.write_i32(0);
    negative.write_i32(16);
    const auto data = negative.take();
    BinaryReader r2(data);
    EXPECT_THROW((void)read_bulk_data(r2), SerializationError);
}

TEST(PixelFormatTests, NamesAndSurfaceSizes)
{
    EXPECT_EQ(pixel_format_from_name("PF_DXT1"), PixelFormat::DXT1);
    EXPECT_EQ(pixel_format_from_name("pf_dxt5"), PixelFormat::DXT5);
    EXPECT_EQ(pixel_format_from_name("PF_Nonsense"), PixelFormat::Unknown);
    EXPECT_EQ(to_string(PixelFormat::A8R8G8B8), "PF_A8R8G8B8");
    EXPECT_EQ(surface_size(PixelFormat::DXT1, 256, 256), 32768u);
    EXPECT_EQ(surface_size(PixelFormat::DXT5, 256, 256), 65536u);
    EXPECT_EQ(surface_size(PixelFormat::DXT1, 1, 1), 8u); // one block minimum
    EXPECT_EQ(surface_size(PixelFormat::DXT5, 2, 6), 32u);
    EXPECT_EQ(surface_size(PixelFormat::A8R8G8B8, 3, 2), 24u);
    EXPECT_EQ(surface_size(PixelFormat::G8, 5, 5), 25u);
    EXPECT_FALSE(surface_size(PixelFormat::DepthStencil, 4, 4).has_value());
}

TEST(PixelDecodeTests, Dxt1FourColorBlock)
{
    // c0 = pure red (0xF800), c1 = pure blue (0x001F); indices: row 0 = 0,1,2,3, other rows 0.
    const auto block = bytes({0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0});
    Image image = decode_pixels(block, PixelFormat::DXT1, 4, 4);
    EXPECT_EQ(pixel(image, 0, 0), (std::array<std::uint8_t, 4>{255, 0, 0, 255}));
    EXPECT_EQ(pixel(image, 1, 0), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
    EXPECT_EQ(pixel(image, 2, 0), (std::array<std::uint8_t, 4>{170, 0, 85, 255}));
    EXPECT_EQ(pixel(image, 3, 0), (std::array<std::uint8_t, 4>{85, 0, 170, 255}));
    EXPECT_EQ(pixel(image, 3, 3), (std::array<std::uint8_t, 4>{255, 0, 0, 255}));
}

TEST(PixelDecodeTests, Dxt1PunchThroughAlpha)
{
    // c0 <= c1 selects 3-color mode: index 3 is transparent black.
    const auto block = bytes({0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF});
    Image image = decode_pixels(block, PixelFormat::DXT1, 4, 4);
    EXPECT_EQ(pixel(image, 2, 2), (std::array<std::uint8_t, 4>{0, 0, 0, 0}));
}

TEST(PixelDecodeTests, Dxt3ExplicitAlphaAndDxt5InterpolatedAlpha)
{
    // DXT3: first texel alpha nibble 0xF, second 0x0; color block white.
    auto dxt3 = bytes({0x0F, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0});
    Image a = decode_pixels(dxt3, PixelFormat::DXT3, 4, 4);
    EXPECT_EQ(pixel(a, 0, 0)[3], 255);
    EXPECT_EQ(pixel(a, 1, 0)[3], 0);
    EXPECT_EQ(pixel(a, 0, 0)[0], 255);

    // DXT5: a0 = 255, a1 = 0, first texel index 1 (= a1), second index 0 (= a0).
    auto dxt5 = bytes({255, 0, 0b00000001, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0});
    Image b = decode_pixels(dxt5, PixelFormat::DXT5, 4, 4);
    EXPECT_EQ(pixel(b, 0, 0)[3], 0);
    EXPECT_EQ(pixel(b, 1, 0)[3], 255);
}

TEST(PixelDecodeTests, UncompressedFormats)
{
    // A8R8G8B8 is stored B, G, R, A.
    Image argb = decode_pixels(bytes({10, 20, 30, 40}), PixelFormat::A8R8G8B8, 1, 1);
    EXPECT_EQ(pixel(argb, 0, 0), (std::array<std::uint8_t, 4>{30, 20, 10, 40}));
    Image g8 = decode_pixels(bytes({77}), PixelFormat::G8, 1, 1);
    EXPECT_EQ(pixel(g8, 0, 0), (std::array<std::uint8_t, 4>{77, 77, 77, 255}));
    Image v8u8 = decode_pixels(bytes({0, 0}), PixelFormat::V8U8, 1, 1); // flat normal
    EXPECT_EQ(pixel(v8u8, 0, 0), (std::array<std::uint8_t, 4>{128, 128, 255, 255}));
}

TEST(PixelDecodeTests, RejectsShortDataAndUnsupportedFormats)
{
    EXPECT_THROW((void)decode_pixels(bytes({0, 0, 0}), PixelFormat::DXT1, 4, 4), SerializationError);
    EXPECT_THROW((void)decode_pixels(bytes({0, 0, 0, 0}), PixelFormat::DepthStencil, 1, 1), UnsupportedError);
}

TEST(ImageEncodeTests, PngHasValidStructure)
{
    Image image = Image::create(2, 1);
    image.at(0, 0)[0] = 255;
    image.at(1, 0)[3] = 255;
    const auto png = image.to_png();
    ASSERT_GT(png.size(), 33u);
    const std::array<int, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(std::to_integer<int>(png[i]), signature[i]);
    EXPECT_EQ(std::memcmp(png.data() + 12, "IHDR", 4), 0);
    // IHDR CRC of a 2x1 RGBA8 image (computed independently with zlib.crc32).
    const std::uint32_t crc = std::to_integer<std::uint32_t>(png[29]) << 24 |
                              std::to_integer<std::uint32_t>(png[30]) << 16 |
                              std::to_integer<std::uint32_t>(png[31]) << 8 | std::to_integer<std::uint32_t>(png[32]);
    EXPECT_EQ(crc, 0xF4227F8Au);
    EXPECT_EQ(std::memcmp(png.data() + png.size() - 8, "IEND", 4), 0);
}

TEST(ImageEncodeTests, TgaAndDds)
{
    Image image = Image::create(1, 1);
    image.at(0, 0)[0] = 1;
    image.at(0, 0)[1] = 2;
    image.at(0, 0)[2] = 3;
    image.at(0, 0)[3] = 4;
    const auto tga = image.to_tga();
    ASSERT_EQ(tga.size(), 18u + 4u);
    EXPECT_EQ(std::to_integer<int>(tga[18]), 3); // stored B, G, R, A
    EXPECT_EQ(std::to_integer<int>(tga[21]), 4);

    const std::vector<std::vector<std::byte>> mips{std::vector<std::byte>(8), std::vector<std::byte>(8)};
    const auto dds = encode_dds(DdsFormat::DXT1, 4, 4, mips);
    ASSERT_EQ(dds.size(), 128u + 16u);
    EXPECT_EQ(std::memcmp(dds.data(), "DDS ", 4), 0);
    EXPECT_EQ(std::memcmp(dds.data() + 84, "DXT1", 4), 0);
    EXPECT_EQ(std::to_integer<int>(dds[28]), 2); // mip count
}

TEST(AudioTests, DetectsContainers)
{
    std::vector<std::byte> ogg = bytes({'O', 'g', 'g', 'S', 0});
    EXPECT_EQ(detect_audio_container(ogg), AudioContainer::Ogg);
    std::vector<std::byte> wav = bytes({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16,
                                        0, 0, 0, 1, 0});
    EXPECT_EQ(detect_audio_container(wav), AudioContainer::Wave);
    wav[20] = std::byte{0x65};
    wav[21] = std::byte{0x01};
    EXPECT_EQ(detect_audio_container(wav), AudioContainer::Xma);
    EXPECT_EQ(detect_audio_container(bytes({1, 2})), AudioContainer::Unknown);
    EXPECT_EQ(file_extension(AudioContainer::Ogg), "ogg");
}
