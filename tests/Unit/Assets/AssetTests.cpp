// Unit tests for the Assets module: bulk data records, pixel decoders, image encoders, audio detection.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <UnrealLib/Assets/Animation.hpp>
#include <UnrealLib/Assets/Brush.hpp>
#include <UnrealLib/Assets/BulkData.hpp>
#include <UnrealLib/Assets/Font.hpp>
#include <UnrealLib/Assets/Image.hpp>
#include <UnrealLib/Assets/MeshTypes.hpp>
#include <UnrealLib/Assets/SkeletalMesh.hpp>
#include <UnrealLib/Assets/Sound.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Assets/Texture.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/SyntheticPackage.hpp"

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

TEST(SoundTests, ReadsEarlyUe3NativeSoundLayouts)
{
    using namespace UnrealLib::Testing;

    for (const auto version : {249, 251, 254, 260, 265, 266, 327, 376, 380, 385, 390, 419, 420})
    {
        SCOPED_TRACE(version);
        SyntheticPackageSpec spec;
        spec.file_version = static_cast<std::uint16_t>(version);
        spec.licensee_version = 0;
        const auto none = intern(spec, "None");
        const auto file_type = intern(spec, "wav");
        const auto sound_class = add_export(spec, {}, {}, {}, "SoundNodeWave");
        SerializationContext context;
        context.file_version = spec.file_version;
        BinaryWriter writer(context);
        if (version >= 322) writer.write_i32(-1); // NetIndex
        write_fname(writer, none);
        if (version < 380) write_fname(writer, file_type);
        if (version >= 385 && version < 420)
        {
            writer.write_i32(2);
            writer.write_i32(0);
            writer.write_i32(4);
            writer.write_i32(2);
            writer.write_i32(4);
            writer.write_i32(2);
        }
        const auto payload = bytes({1, 2, 3, 4, 5, 6});
        const auto lazy_end_offset_position = writer.position();
        if (version < 266)
        {
            writer.write_i32(0); // Skip offset, fixed up below for v265.
            if (version >= 254) writer.write_i32(static_cast<std::int32_t>(payload.size()));
            if (version >= 251) writer.write_u32(0);
            if (version >= 260) write_fname(writer, none);
            writer.write_index(static_cast<std::int32_t>(payload.size()));
            writer.write_bytes(payload);
        }
        else
        {
            BulkData raw;
            raw.element_count = static_cast<std::int32_t>(payload.size());
            write_bulk_data(writer, raw, payload);
        }
        if (version >= 390 && version < 420) writer.write_i32(2);
        BulkData empty;
        if (version >= 380) write_bulk_data(writer, empty, {});
        if (version >= 327) write_bulk_data(writer, empty, {});
        if (version >= 376) write_bulk_data(writer, empty, {});

        const auto sound_index = add_export(spec, sound_class, {}, {}, "TestSound", 0, writer.take());
        auto synthetic = build_synthetic_package(spec);
        if (version == 265)
        {
            Package probe;
            probe.load_from_memory(synthetic.file, "Probe");
            const auto& entry = probe.exports()[sound_index.export_index()];
            BinaryWriter offset(context);
            offset.write_i32(entry.serial_offset + entry.serial_size);
            std::ranges::copy(offset.data(), spec.exports.back().payload.begin() + lazy_end_offset_position);
            synthetic = build_synthetic_package(spec);
        }
        Package package;
        package.load_from_memory(synthetic.file, "TestSoundPackage");
        Linker linker;
        const auto& loaded = linker.add(std::move(package));
        const auto sound = read_sound_wave(linker, {&loaded, sound_index});
        EXPECT_EQ(sound.file_type, version < 380 ? std::optional{file_type} : std::nullopt);
        EXPECT_EQ(sound.channels, version >= 390 && version < 420 ? 2 : 0);
        if (version >= 385 && version < 420)
        {
            EXPECT_EQ(sound.channel_offsets, (std::vector<std::int32_t>{0, 4}));
            EXPECT_EQ(sound.channel_sizes, (std::vector<std::int32_t>{4, 2}));
        }
        else
        {
            EXPECT_TRUE(sound.channel_offsets.empty());
            EXPECT_TRUE(sound.channel_sizes.empty());
        }
        const auto extracted = extract_sound(loaded, sound);
        ASSERT_TRUE(extracted);
        EXPECT_EQ(extracted->bytes, payload);
    }
}

TEST(FontTests, ReadsGlyphLayoutAndRemapAcrossVersionBoundaries)
{
    using namespace UnrealLib::Testing;

    for (const auto version : {505, 506, 510, 511, 536})
    {
        SCOPED_TRACE(version);
        SyntheticPackageSpec spec;
        spec.file_version = static_cast<std::uint16_t>(version);
        spec.licensee_version = version == 536 ? 43 : 0;
        const auto none = intern(spec, "None");
        const auto font_class = add_export(spec, {}, {}, {}, "Font");
        const auto multi_font_class = add_export(spec, {}, {}, {}, "MultiFont");
        const auto texture = add_export(spec, {}, {}, {}, "FontTexture");
        SerializationContext context;
        context.file_version = spec.file_version;
        BinaryWriter writer(context);
        writer.write_i32(-1); // NetIndex
        write_fname(writer, none);
        if (version < 511)
        {
            writer.write_i32(1); // Glyph count
            writer.write_i32(4);
            writer.write_i32(8);
            writer.write_i32(12);
            writer.write_i32(16);
            writer.write_u8(0);
            if (version >= 506) writer.write_i32(-2);
            writer.write_i32(1); // Texture count
            writer.write_index(texture.value);
            writer.write_i32(2); // Kerning
        }
        writer.write_i32(1); // Remap count
        writer.write_u16(65);
        writer.write_u16(1);
        if (version < 511) writer.write_i32(1); // IsRemapped
        if (version == 536)
        {
            writer.write_i32(1); // Kerning pairs
            writer.write_u16(65);
            writer.write_u16(86);
            writer.write_f32(-2.0f);
            writer.write_i32(2); // Resolution offsets
            writer.write_u32(0);
            writer.write_u32(1);
        }

        const auto font_index = add_export(spec, font_class, {}, {}, "TestFont", 0,
                                           std::vector<std::byte>(writer.data().begin(), writer.data().end()));
        if (version < 511)
        {
            writer.write_i32(2);
            writer.write_f32(480.0f);
            writer.write_f32(720.0f);
        }
        const auto multi_font_index = add_export(spec, multi_font_class, {}, {}, "TestMultiFont", 0, writer.take());
        Package package;
        package.load_from_memory(build_synthetic_package(spec).file, "TestFontPackage");
        Linker linker;
        const auto& loaded = linker.add(std::move(package));
        const auto font = read_font(linker, {&loaded, font_index});
        const auto multi_font = read_font(linker, {&loaded, multi_font_index});
        EXPECT_TRUE(font.resolution_test_table.empty());
        EXPECT_EQ(multi_font.char_remap, font.char_remap);
        EXPECT_EQ(multi_font.resolution_test_table,
                  version < 511 ? (std::vector<float>{480.0f, 720.0f}) : std::vector<float>{});
        if (version == 536)
        {
            ASSERT_EQ(multi_font.kerning_pairs.size(), 1u);
            EXPECT_EQ(multi_font.kerning_pairs.front().first, 65u);
            EXPECT_EQ(multi_font.kerning_pairs.front().second, 86u);
            EXPECT_EQ(multi_font.kerning_pairs.front().adjustment, -2.0f);
            EXPECT_EQ(multi_font.kerning_resolution_offsets, (std::vector<std::uint32_t>{0, 1}));
        }
        EXPECT_TRUE(font.is_remapped);
        ASSERT_EQ(font.char_remap.size(), 1u);
        EXPECT_EQ(font.char_remap.front().first, 65u);
        EXPECT_EQ(font.char_remap.front().second, 1u);
        if (version < 511)
        {
            ASSERT_EQ(font.characters.size(), 1u);
            EXPECT_EQ(font.characters.front().u_size, 12);
            EXPECT_EQ(font.characters.front().vertical_offset, version >= 506 ? -2 : 0);
            EXPECT_EQ(font.textures, std::vector<ObjectIndex>{texture});
            EXPECT_EQ(font.kerning, 2);
        }
        else
        {
            EXPECT_TRUE(font.characters.empty());
            EXPECT_TRUE(font.textures.empty());
        }
    }
}

TEST(BrushTests, ReadsModelComponentPrefixAndRejectsTruncation)
{
    using namespace UnrealLib::Testing;

    SyntheticPackageSpec spec;
    spec.file_version = 536;
    spec.licensee_version = 43;
    const auto none = intern(spec, "None");
    const auto component_class = add_export(spec, {}, {}, {}, "ModelComponent");
    const auto model = add_export(spec, {}, {}, {}, "TestModel");
    SerializationContext context;
    context.file_version = spec.file_version;
    BinaryWriter writer(context);
    writer.write_i32(-1); // NetIndex
    write_fname(writer, none);
    writer.write_index(model.value);
    const auto truncated = add_export(spec, component_class, {}, {}, "Truncated", 0,
                                      std::vector<std::byte>(writer.data().begin(), writer.data().end()));
    writer.write_i32(-1); // ZoneIndex
    const auto prefix_end = writer.position();
    writer.write_u32(0); // Remaining native data
    const auto component = add_export(spec, component_class, {}, {}, "Component", 0, writer.take());

    Package package;
    package.load_from_memory(build_synthetic_package(spec).file, "ModelComponents");
    Linker linker;
    const auto& loaded = linker.add(std::move(package));
    const auto result = read_model_component(linker, {&loaded, component});
    EXPECT_EQ(result.model, model);
    EXPECT_EQ(result.zone_index, -1);
    EXPECT_EQ(result.unparsed_offset, prefix_end);
    EXPECT_THROW((void)read_model_component(linker, {&loaded, truncated}), SerializationError);
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

TEST(TextureTests, ReadsDishonoredLightmapSuffix)
{
    using namespace UnrealLib::Testing;

    SyntheticPackageSpec spec;
    spec.file_version = 801;
    spec.licensee_version = 30;
    spec.engine_version = 9411;
    spec.cooker_version = 133;
    const auto none = intern(spec, "None");
    const auto lightmap_class = add_export(spec, {}, {}, {}, "LightMapTexture2D");
    const auto texture_class = add_export(spec, {}, {}, {}, "Texture2D");
    SerializationContext context;
    context.file_version = spec.file_version;
    BinaryWriter writer(context);
    writer.write_i32(-1); // NetIndex
    write_fname(writer, none);
    BulkData source_art;
    write_bulk_data(writer, source_art, {});
    writer.write_i32(0); // Mips
    writer.write_guid({});
    writer.write_i32(0); // CachedPVRTCMips
    const std::vector<std::byte> base(writer.data().begin(), writer.data().end());
    const auto base_index = add_export(spec, texture_class, {}, {}, "Texture", 0, base);
    const auto truncated = add_export(spec, lightmap_class, {}, {}, "TruncatedLightmap", 0, base);
    writer.write_u32(1);
    const auto lightmap = add_export(spec, lightmap_class, {}, {}, "Lightmap", 0, writer.take());

    Package package;
    package.load_from_memory(build_synthetic_package(spec).file, "DishonoredTextures");
    Linker linker;
    const auto& loaded = linker.add(std::move(package));
    ASSERT_EQ(loaded.profile().id, "dishonored-pc");
    EXPECT_EQ(read_texture2d(linker, {&loaded, lightmap}).lightmap_trailing_word, 1u);
    EXPECT_FALSE(read_texture2d(linker, {&loaded, base_index}).lightmap_trailing_word);
    EXPECT_THROW((void)read_texture2d(linker, {&loaded, truncated}), SerializationError);
}

TEST(TextureTests, ReadsNativeMovieAndVolumeTextureData)
{
    using namespace UnrealLib::Testing;

    SyntheticPackageSpec spec;
    spec.file_version = 536;
    spec.licensee_version = 43;
    const auto none = intern(spec, "None");
    const auto movie_class = add_export(spec, {}, {}, {}, "TextureMovie");
    const auto volume_class = add_export(spec, {}, {}, {}, "Texture3D");
    SerializationContext context;
    context.file_version = spec.file_version;
    const auto source_art = bytes({1, 2});
    const auto movie_bytes = bytes({3, 4, 5});
    const auto mip_bytes = bytes({6, 7, 8, 9});

    BinaryWriter movie_writer(context);
    movie_writer.write_i32(-1); // NetIndex
    write_fname(movie_writer, none);
    BulkData source;
    source.element_count = 2;
    write_bulk_data(movie_writer, source, source_art);
    BulkData movie_payload;
    movie_payload.element_count = 3;
    write_bulk_data(movie_writer, movie_payload, movie_bytes);
    const auto movie_index = add_export(spec, movie_class, {}, {}, "Movie", 0, movie_writer.take());

    BinaryWriter volume_writer(context);
    volume_writer.write_i32(-1); // NetIndex
    write_fname(volume_writer, none);
    BulkData empty;
    write_bulk_data(volume_writer, empty, {});
    volume_writer.write_u32(2); // SizeX
    volume_writer.write_u32(1); // SizeY
    volume_writer.write_i32(3); // Format ordinal
    volume_writer.write_i32(1); // Mip count
    BulkData mip_payload;
    mip_payload.element_count = 4;
    write_bulk_data(volume_writer, mip_payload, mip_bytes);
    volume_writer.write_u32(2);
    volume_writer.write_u32(1);
    const auto truncated = add_export(spec, volume_class, {}, {}, "TruncatedVolume", 0,
                                      std::vector<std::byte>(volume_writer.data().begin(),
                                                             volume_writer.data().end()));
    volume_writer.write_u32(2); // Mip depth
    const auto volume_index = add_export(spec, volume_class, {}, {}, "Volume", 0, volume_writer.take());

    Package package;
    package.load_from_memory(build_synthetic_package(spec).file, "ExtraTextures");
    Linker linker;
    const auto& loaded = linker.add(std::move(package));

    const auto movie = read_texture_movie(linker, {&loaded, movie_index});
    EXPECT_EQ(load_bulk_data(loaded, movie.source_art), source_art);
    EXPECT_EQ(load_bulk_data(loaded, movie.raw_data), movie_bytes);
    const auto volume = read_texture3d(linker, {&loaded, volume_index});
    EXPECT_EQ(volume.size_x, 2u);
    EXPECT_EQ(volume.size_y, 1u);
    EXPECT_EQ(volume.format, 3);
    ASSERT_EQ(volume.mips.size(), 1u);
    EXPECT_EQ(volume.mips[0].size_x, 2u);
    EXPECT_EQ(volume.mips[0].size_y, 1u);
    EXPECT_EQ(volume.mips[0].size_z, 2u);
    EXPECT_EQ(load_bulk_data(loaded, volume.mips[0].data), mip_bytes);
    EXPECT_THROW((void)read_texture3d(linker, {&loaded, truncated}), SerializationError);
    EXPECT_THROW((void)read_texture_movie(linker, {&loaded, volume_index}), Error);
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

TEST(MeshTypesTests, UnpacksPackedNormals)
{
    // Each byte maps b / 127.5 - 1: 0 -> -1, 255 -> 1, 0x80 -> ~0.0039.
    const Vec3 up = unpack_normal(0x00FF8080u);
    EXPECT_NEAR(up.x, 0.0039f, 1e-3f);
    EXPECT_NEAR(up.y, 0.0039f, 1e-3f);
    EXPECT_NEAR(up.z, 1.0f, 1e-6f);
    const Vec3 down = unpack_normal(0x00000000u);
    EXPECT_FLOAT_EQ(down.x, -1.0f);
    EXPECT_FLOAT_EQ(down.z, -1.0f);
}

TEST(SkeletalMeshTests, PskHasChunksInOrderAndMirrorsGeometry)
{
    SkeletalMesh mesh;
    mesh.name = "Tri";
    MeshBone root;
    root.name = "Root";
    root.orientation = {0.1f, 0.2f, 0.3f, 0.9f};
    root.position = {1.0f, 2.0f, 3.0f};
    mesh.skeleton.push_back(root);
    SkeletalLod lod;
    for (int i = 0; i < 3; ++i)
    {
        SkinnedVertex v;
        v.position = {static_cast<float>(i), 5.0f, 0.0f};
        v.weights = {255, 0, 0, 0};
        lod.vertices.push_back(v);
    }
    lod.indices = {0, 1, 2};
    lod.sections.push_back(SkeletalSection{0, 0, 0, 1});
    mesh.lods.push_back(lod);

    const Package package; // no materials referenced
    const auto psk = to_psk(package, mesh);
    BinaryReader r(psk);
    const auto chunk = [&](const char* id, std::int32_t size, std::int32_t count) {
        const auto name = r.read_view(20);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(name.data())), id);
        EXPECT_EQ(r.read_u32(), 20100422u);
        EXPECT_EQ(r.read_i32(), size) << id;
        EXPECT_EQ(r.read_i32(), count) << id;
    };
    chunk("ACTRHEAD", 0, 0);
    chunk("PNTS0000", 12, 3);
    EXPECT_FLOAT_EQ(r.read_f32(), 0.0f);
    EXPECT_FLOAT_EQ(r.read_f32(), -5.0f); // Y mirrored
    (void)r.read_view(12 * 2 + 4);
    chunk("VTXW0000", 16, 3);
    (void)r.read_view(48);
    chunk("FACE0000", 12, 1);
    EXPECT_EQ(r.read_u16(), 1); // first two corners swapped
    EXPECT_EQ(r.read_u16(), 0);
    EXPECT_EQ(r.read_u16(), 2);
    (void)r.read_view(6);
    chunk("MATT0000", 88, 1);
    (void)r.read_view(88);
    chunk("REFSKELT", 120, 1);
    (void)r.read_view(64 + 12);
    EXPECT_FLOAT_EQ(r.read_f32(), 0.1f);
    EXPECT_FLOAT_EQ(r.read_f32(), -0.2f);
    EXPECT_FLOAT_EQ(r.read_f32(), 0.3f);
    EXPECT_FLOAT_EQ(r.read_f32(), -0.9f); // root: only the mirror negates W (non-root bones are negated twice)
    (void)r.read_view(12 + 16);
    chunk("RAWWEIGHTS", 12, 3);
    EXPECT_FLOAT_EQ(r.read_f32(), 1.0f);
    (void)r.read_view(8 + 24);
    EXPECT_TRUE(r.at_end());
}

TEST(AnimationTests, DecodesConstantKeyLerpTracks)
{
    // Track 0: two translation keys (floats) and two Fixed48NoW rotation keys preceded by 24 bytes of mins/ranges
    // (version < 761). Track 1: one translation key and one rotation key (stored as Float96NoW).
    BinaryWriter w;
    for (const float f : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f}) w.write_f32(f); // offset 0: 2 positions
    const auto rot0 = static_cast<std::int32_t>(w.position());
    for (int i = 0; i < 6; ++i) w.write_f32(0.0f);                          // mins/ranges
    for (const std::uint16_t v : std::array<std::uint16_t, 6>{32767, 32767, 32767, 65534, 32767, 32767}) w.write_u16(v); // (0,0,0,1), (1,0,0,0)
    const auto trans1 = static_cast<std::int32_t>(w.position());
    for (const float f : {7.0f, 8.0f, 9.0f}) w.write_f32(f);
    const auto rot1 = static_cast<std::int32_t>(w.position());
    for (const float f : {0.0f, 0.6f, 0.0f}) w.write_f32(f);
    const auto bytes = w.take();

    AnimSequence seq;
    seq.num_frames = 3;
    seq.rotation_format = AnimCompression::Fixed48NoW;
    seq.track_offsets = {0, 2, rot0, 2, trans1, 1, rot1, 1};
    for (const std::byte b : bytes) seq.stream.push_back(std::to_integer<std::uint8_t>(b));
    decode_anim_tracks(seq, true);

    ASSERT_EQ(seq.tracks.size(), 2u);
    EXPECT_EQ(seq.stream_bytes_used, seq.stream.size());
    EXPECT_EQ(seq.tracks[0].positions[1], (Vec3{4.0f, 5.0f, 6.0f}));
    EXPECT_FLOAT_EQ(seq.tracks[0].rotations[0].w, 1.0f);
    EXPECT_NEAR(seq.tracks[0].rotations[1].x, 1.0f, 1e-4f);
    EXPECT_NEAR(seq.tracks[1].rotations[0].y, 0.6f, 1e-6f);
    EXPECT_NEAR(seq.tracks[1].rotations[0].w, 0.8f, 1e-6f); // W restored: sqrt(1 - 0.36)

    // Sampling: frame 1 of 3 lies halfway between the two keys.
    Vec3 p;
    Quat q;
    seq.tracks[0].sample(1, 3, p, q);
    EXPECT_FLOAT_EQ(p.x, 2.5f);
    EXPECT_NEAR(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, 1.0f, 1e-5f);
}

TEST(AnimationTests, Fixed32NoWBitLayoutAndErrors)
{
    // X (11 bits, high), Y (11 bits), Z (10 bits, low): X = 2046 -> 1.0, Y = 1023 -> 0.0, Z = 511 -> 0.0.
    BinaryWriter w;
    for (int i = 0; i < 6; ++i) w.write_f32(0.0f);
    w.write_u32((2046u << 21) | (1023u << 10) | 511u);
    w.write_u32((1023u << 21) | (1023u << 10) | 511u);
    AnimSequence seq;
    seq.rotation_format = AnimCompression::Fixed32NoW;
    seq.track_offsets = {0, 0, 0, 2};
    for (const std::byte b : w.take()) seq.stream.push_back(std::to_integer<std::uint8_t>(b));
    decode_anim_tracks(seq, true);
    EXPECT_NEAR(seq.tracks[0].rotations[0].x, 1.0f, 1e-6f);
    EXPECT_NEAR(seq.tracks[0].rotations[1].w, 1.0f, 1e-6f);

    seq.track_offsets = {0, 0, 1000, 2};
    EXPECT_THROW(decode_anim_tracks(seq, true), SerializationError);
    seq.key_format = AnimKeyFormat::Unknown;
    EXPECT_THROW(decode_anim_tracks(seq, true), UnsupportedError);
    EXPECT_EQ(anim_compression_from_name("ACF_Fixed48NoW"), AnimCompression::Fixed48NoW);
    EXPECT_EQ(anim_key_format_from_name("AKF_PerTrackCompression"), AnimKeyFormat::PerTrackCompression);
}

TEST(AnimationTests, DecodesVariableKeyLerpFrameTables)
{
    // One track: three float translation keys at frames 0, 2 and 9, then the frame table (bytes: < 256 frames),
    // padded to 4 bytes. No rotation keys.
    BinaryWriter w;
    for (const float f : {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 9.0f, 0.0f, 0.0f}) w.write_f32(f);
    for (const std::uint8_t f : std::array<std::uint8_t, 4>{0, 2, 9, 0x55}) w.write_u8(f);
    AnimSequence seq;
    seq.num_frames = 10;
    seq.key_format = AnimKeyFormat::VariableKeyLerp;
    seq.translation_format = AnimCompression::Float96NoW;
    seq.track_offsets = {0, 3, 0, 0};
    for (const std::byte b : w.take()) seq.stream.push_back(std::to_integer<std::uint8_t>(b));
    decode_anim_tracks(seq, false);
    ASSERT_EQ(seq.tracks.size(), 1u);
    EXPECT_EQ(seq.stream_bytes_used, seq.stream.size());
    EXPECT_EQ(seq.tracks[0].position_frames, (std::vector<float>{0.0f, 2.0f, 9.0f}));

    // Frame 5 lies 3/7 of the way from the key at frame 2 to the key at frame 9.
    Vec3 p;
    Quat q;
    seq.tracks[0].sample(5, 10, p, q);
    EXPECT_FLOAT_EQ(p.x, 5.0f);
    seq.tracks[0].sample(9, 10, p, q);
    EXPECT_FLOAT_EQ(p.x, 9.0f);
}

TEST(AnimationTests, DecodesPerTrackCompression)
{
    // Track 0: translation Float96NoW with only X present (mask 1), 1 key; rotation Fixed48NoW with mask 7 and two
    // keys plus a frame table (flag 8). Track 1: identity translation (-1); rotation IntervalFixed32NoW with Y only.
    BinaryWriter w;
    w.write_u32((1u << 28) | (1u << 24) | 1u);
    w.write_f32(42.0f);
    const auto rot0 = static_cast<std::int32_t>(w.position());
    w.write_u32((2u << 28) | (15u << 24) | 2u);
    for (const std::uint16_t v : std::array<std::uint16_t, 6>{32767, 32767, 32767, 65534, 32767, 32767}) w.write_u16(v);
    for (const std::uint8_t f : std::array<std::uint8_t, 4>{0, 4, 0x55, 0x55}) w.write_u8(f);
    const auto rot1 = static_cast<std::int32_t>(w.position());
    w.write_u32((3u << 28) | (2u << 24) | 1u);
    w.write_f32(0.25f); // min Y
    w.write_f32(0.5f);  // range Y
    w.write_u32((1023u << 21) | (2046u << 10) | 511u); // Y: (2046 / 1023 - 1) * 0.5 + 0.25 = 0.75
    AnimSequence seq;
    seq.num_frames = 5;
    seq.key_format = AnimKeyFormat::PerTrackCompression;
    seq.track_offsets = {0, rot0, -1, rot1};
    for (const std::byte b : w.take()) seq.stream.push_back(std::to_integer<std::uint8_t>(b));
    decode_anim_tracks(seq, false);

    ASSERT_EQ(seq.tracks.size(), 2u);
    EXPECT_EQ(seq.stream_bytes_used, seq.stream.size());
    EXPECT_EQ(seq.tracks[0].positions, (std::vector<Vec3>{{42.0f, 0.0f, 0.0f}}));
    ASSERT_EQ(seq.tracks[0].rotations.size(), 2u);
    EXPECT_FLOAT_EQ(seq.tracks[0].rotations[0].w, 1.0f);
    EXPECT_NEAR(seq.tracks[0].rotations[1].x, 1.0f, 1e-4f);
    EXPECT_EQ(seq.tracks[0].rotation_frames, (std::vector<float>{0.0f, 4.0f}));
    EXPECT_EQ(seq.tracks[1].positions, (std::vector<Vec3>{{}}));
    ASSERT_EQ(seq.tracks[1].rotations.size(), 1u);
    EXPECT_NEAR(seq.tracks[1].rotations[0].y, 0.75f, 1e-6f);
    EXPECT_NEAR(seq.tracks[1].rotations[0].x, 0.0f, 1e-6f);

    seq.track_offsets = {0, rot0, -1};
    EXPECT_THROW(decode_anim_tracks(seq, false), SerializationError);
}
