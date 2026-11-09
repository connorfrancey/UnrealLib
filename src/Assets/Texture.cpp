#include <UnrealLib/Assets/Texture.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/LegacyVersion.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib::Assets
{
    namespace
    {
        struct FormatInfo
        {
            PixelFormat format;
            std::string_view name;
            std::uint32_t block;      // block edge in pixels (1 = not block compressed)
            std::uint32_t block_bytes; // bytes per block (per pixel when block == 1); 0 = no fixed size
        };

        // Names and sizes from UE3's GPixelFormats table (EPixelFormat). Only the names are stored on disk.
        constexpr std::array<FormatInfo, 25> formats{{
            {PixelFormat::Unknown, "PF_Unknown", 1, 0},
            {PixelFormat::A32B32G32R32F, "PF_A32B32G32R32F", 1, 16},
            {PixelFormat::A8R8G8B8, "PF_A8R8G8B8", 1, 4},
            {PixelFormat::G8, "PF_G8", 1, 1},
            {PixelFormat::G16, "PF_G16", 1, 2},
            {PixelFormat::DXT1, "PF_DXT1", 4, 8},
            {PixelFormat::DXT3, "PF_DXT3", 4, 16},
            {PixelFormat::DXT5, "PF_DXT5", 4, 16},
            {PixelFormat::UYVY, "PF_UYVY", 2, 4},
            {PixelFormat::FloatRGB, "PF_FloatRGB", 1, 12},
            {PixelFormat::FloatRGBA, "PF_FloatRGBA", 1, 8},
            {PixelFormat::DepthStencil, "PF_DepthStencil", 1, 0},
            {PixelFormat::ShadowDepth, "PF_ShadowDepth", 1, 0},
            {PixelFormat::FilteredShadowDepth, "PF_FilteredShadowDepth", 1, 0},
            {PixelFormat::R32F, "PF_R32F", 1, 4},
            {PixelFormat::G16R16, "PF_G16R16", 1, 4},
            {PixelFormat::G16R16F, "PF_G16R16F", 1, 4},
            {PixelFormat::G32R32F, "PF_G32R32F", 1, 8},
            {PixelFormat::A2B10G10R10, "PF_A2B10G10R10", 1, 4},
            {PixelFormat::A16B16G16R16, "PF_A16B16G16R16", 1, 8},
            {PixelFormat::D24, "PF_D24", 1, 0},
            {PixelFormat::R16F, "PF_R16F", 1, 2},
            {PixelFormat::BC5, "PF_BC5", 4, 16},
            {PixelFormat::V8U8, "PF_V8U8", 1, 2},
            {PixelFormat::A1, "PF_A1", 1, 0},
        }};

        const FormatInfo& info(PixelFormat format) noexcept
        {
            for (const auto& f : formats)
            {
                if (f.format == format) return f;
            }
            return formats[0];
        }

        std::uint32_t read_le16(const std::byte* p) noexcept
        {
            return std::to_integer<std::uint32_t>(p[0]) | std::to_integer<std::uint32_t>(p[1]) << 8;
        }

        std::uint32_t read_le32(const std::byte* p) noexcept
        {
            return read_le16(p) | read_le16(p + 2) << 16;
        }

        float read_f32(const std::byte* p) noexcept
        {
            const std::uint32_t bits = read_le32(p);
            float f = 0;
            std::memcpy(&f, &bits, sizeof f);
            return f;
        }

        float half_to_float(std::uint32_t h) noexcept
        {
            const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
            float value = 0;
            if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
            else if (exponent == 31) value = mantissa != 0 ? 0.0f : 65504.0f;
            else value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
            return sign != 0 ? -value : value;
        }

        std::uint8_t to_unorm8(float v) noexcept
        {
            if (!(v > 0.0f)) return 0;
            if (v >= 1.0f) return 255;
            return static_cast<std::uint8_t>(std::lround(v * 255.0f));
        }

        std::uint8_t expand5(std::uint32_t v) noexcept { return static_cast<std::uint8_t>((v << 3) | (v >> 2)); }
        std::uint8_t expand6(std::uint32_t v) noexcept { return static_cast<std::uint8_t>((v << 2) | (v >> 4)); }

        /// BC1 color block -> 16 RGBA texels. @p force_four: DXT3/5 always use the 4-color mode.
        void decode_color_block(const std::byte* block, std::array<std::array<std::uint8_t, 4>, 16>& out,
                                bool force_four) noexcept
        {
            const std::uint32_t c0 = read_le16(block), c1 = read_le16(block + 2);
            std::array<std::array<std::uint8_t, 4>, 4> palette{};
            palette[0] = {expand5(c0 >> 11), expand6((c0 >> 5) & 0x3F), expand5(c0 & 0x1F), 255};
            palette[1] = {expand5(c1 >> 11), expand6((c1 >> 5) & 0x3F), expand5(c1 & 0x1F), 255};
            if (c0 > c1 || force_four)
            {
                for (int i = 0; i < 3; ++i)
                {
                    palette[2][i] = static_cast<std::uint8_t>((2 * palette[0][i] + palette[1][i] + 1) / 3);
                    palette[3][i] = static_cast<std::uint8_t>((palette[0][i] + 2 * palette[1][i] + 1) / 3);
                }
                palette[2][3] = palette[3][3] = 255;
            }
            else
            {
                for (int i = 0; i < 3; ++i)
                {
                    palette[2][i] = static_cast<std::uint8_t>((palette[0][i] + palette[1][i]) / 2);
                }
                palette[2][3] = 255;
                palette[3] = {0, 0, 0, 0};
            }
            const std::uint32_t indices = read_le32(block + 4);
            for (int i = 0; i < 16; ++i)
            {
                out[static_cast<std::size_t>(i)] = palette[(indices >> (2 * i)) & 3];
            }
        }

        /// BC3/BC4 interpolated 8-bit channel block -> 16 values.
        void decode_alpha_block(const std::byte* block, std::array<std::uint8_t, 16>& out) noexcept
        {
            const std::uint32_t a0 = std::to_integer<std::uint32_t>(block[0]);
            const std::uint32_t a1 = std::to_integer<std::uint32_t>(block[1]);
            std::array<std::uint32_t, 8> values{a0, a1};
            if (a0 > a1)
            {
                for (std::uint32_t i = 1; i < 7; ++i) values[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
            }
            else
            {
                for (std::uint32_t i = 1; i < 5; ++i) values[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
                values[6] = 0;
                values[7] = 255;
            }
            std::uint64_t bits = 0;
            for (int i = 0; i < 6; ++i) bits |= std::to_integer<std::uint64_t>(block[2 + i]) << (8 * i);
            for (int i = 0; i < 16; ++i)
            {
                out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(values[(bits >> (3 * i)) & 7]);
            }
        }

        std::uint8_t reconstruct_z(std::uint8_t x, std::uint8_t y) noexcept
        {
            const float nx = x / 127.5f - 1.0f, ny = y / 127.5f - 1.0f;
            const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
            return to_unorm8((nz + 1.0f) * 0.5f);
        }
    }

    PixelFormat pixel_format_from_name(std::string_view name) noexcept
    {
        for (const auto& f : formats)
        {
            if (Text::iequals(f.name, name)) return f.format;
        }
        return PixelFormat::Unknown;
    }

    std::string_view to_string(PixelFormat format) noexcept { return info(format).name; }

    bool can_decode(PixelFormat format) noexcept
    {
        switch (format)
        {
            case PixelFormat::DXT1:
            case PixelFormat::DXT3:
            case PixelFormat::DXT5:
            case PixelFormat::BC5:
            case PixelFormat::A8R8G8B8:
            case PixelFormat::G8:
            case PixelFormat::G16:
            case PixelFormat::V8U8:
            case PixelFormat::A32B32G32R32F:
            case PixelFormat::FloatRGBA:
            case PixelFormat::FloatRGB:
            case PixelFormat::A1:
                return true;
            default:
                return false;
        }
    }

    std::optional<std::uint64_t> surface_size(PixelFormat format, std::uint32_t width, std::uint32_t height) noexcept
    {
        if (format == PixelFormat::A1)
        {
            return static_cast<std::uint64_t>((width + 7) / 8) * height;
        }
        const FormatInfo& f = info(format);
        if (f.block_bytes == 0)
        {
            return std::nullopt;
        }
        const std::uint64_t bx = std::max<std::uint64_t>(1, (width + f.block - 1) / f.block);
        const std::uint64_t by = std::max<std::uint64_t>(1, (height + f.block - 1) / f.block);
        return bx * by * f.block_bytes;
    }

    Image decode_pixels(std::span<const std::byte> data, PixelFormat format, std::uint32_t width,
                        std::uint32_t height)
    {
        if (!can_decode(format))
        {
            throw UnsupportedError(fmt::format("cannot decode pixel format {}", to_string(format)));
        }
        const auto needed = surface_size(format, width, height);
        if (!needed || data.size() < *needed)
        {
            throw SerializationError(fmt::format("{}x{} {} surface needs {} bytes, got {}", width, height,
                                                 to_string(format), needed.value_or(0), data.size()));
        }
        if (static_cast<std::uint64_t>(width) * height > (1ull << 28))
        {
            throw Error(ErrorCode::LimitExceeded, fmt::format("{}x{} image is too large", width, height));
        }
        Image image = Image::create(width, height);
        const std::byte* p = data.data();

        if (format == PixelFormat::DXT1 || format == PixelFormat::DXT3 || format == PixelFormat::DXT5 ||
            format == PixelFormat::BC5)
        {
            const std::uint32_t blocks_x = std::max(1u, (width + 3) / 4), blocks_y = std::max(1u, (height + 3) / 4);
            const std::size_t block_size = format == PixelFormat::DXT1 ? 8 : 16;
            std::array<std::array<std::uint8_t, 4>, 16> texels{};
            std::array<std::uint8_t, 16> alpha{}, second{};
            for (std::uint32_t by = 0; by < blocks_y; ++by)
            {
                for (std::uint32_t bx = 0; bx < blocks_x; ++bx)
                {
                    const std::byte* block = p + (static_cast<std::size_t>(by) * blocks_x + bx) * block_size;
                    switch (format)
                    {
                        case PixelFormat::DXT1:
                            decode_color_block(block, texels, false);
                            break;
                        case PixelFormat::DXT3:
                            decode_color_block(block + 8, texels, true);
                            for (int i = 0; i < 16; ++i)
                            {
                                const std::uint32_t nibble =
                                    (std::to_integer<std::uint32_t>(block[i / 2]) >> (4 * (i % 2))) & 0xF;
                                texels[static_cast<std::size_t>(i)][3] = static_cast<std::uint8_t>(nibble * 17);
                            }
                            break;
                        case PixelFormat::DXT5:
                            decode_color_block(block + 8, texels, true);
                            decode_alpha_block(block, alpha);
                            for (int i = 0; i < 16; ++i) texels[static_cast<std::size_t>(i)][3] = alpha[static_cast<std::size_t>(i)];
                            break;
                        default: // BC5: X in the first block, Y in the second
                            decode_alpha_block(block, alpha);
                            decode_alpha_block(block + 8, second);
                            for (std::size_t i = 0; i < 16; ++i)
                            {
                                texels[i] = {alpha[i], second[i], reconstruct_z(alpha[i], second[i]), 255};
                            }
                            break;
                    }
                    for (std::uint32_t ty = 0; ty < 4; ++ty)
                    {
                        for (std::uint32_t tx = 0; tx < 4; ++tx)
                        {
                            const std::uint32_t x = bx * 4 + tx, y = by * 4 + ty;
                            if (x < width && y < height)
                            {
                                std::memcpy(image.at(x, y), texels[ty * 4 + tx].data(), 4);
                            }
                        }
                    }
                }
            }
            return image;
        }

        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::size_t i = static_cast<std::size_t>(y) * width + x;
                std::uint8_t* out = image.at(x, y);
                switch (format)
                {
                    case PixelFormat::A8R8G8B8: // memory order B, G, R, A
                        out[0] = std::to_integer<std::uint8_t>(p[i * 4 + 2]);
                        out[1] = std::to_integer<std::uint8_t>(p[i * 4 + 1]);
                        out[2] = std::to_integer<std::uint8_t>(p[i * 4]);
                        out[3] = std::to_integer<std::uint8_t>(p[i * 4 + 3]);
                        break;
                    case PixelFormat::G8:
                        out[0] = out[1] = out[2] = std::to_integer<std::uint8_t>(p[i]);
                        out[3] = 255;
                        break;
                    case PixelFormat::G16:
                        out[0] = out[1] = out[2] = static_cast<std::uint8_t>(read_le16(p + i * 2) >> 8);
                        out[3] = 255;
                        break;
                    case PixelFormat::V8U8: // signed U (low byte), V (high byte)
                    {
                        const auto u = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(p[i * 2]));
                        const auto v = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(p[i * 2 + 1]));
                        out[0] = static_cast<std::uint8_t>(u + 128);
                        out[1] = static_cast<std::uint8_t>(v + 128);
                        out[2] = reconstruct_z(out[0], out[1]);
                        out[3] = 255;
                        break;
                    }
                    case PixelFormat::A32B32G32R32F:
                        for (int c = 0; c < 4; ++c) out[c] = to_unorm8(read_f32(p + i * 16 + static_cast<std::size_t>(c) * 4));
                        break;
                    case PixelFormat::FloatRGB:
                        for (int c = 0; c < 3; ++c) out[c] = to_unorm8(read_f32(p + i * 12 + static_cast<std::size_t>(c) * 4));
                        out[3] = 255;
                        break;
                    case PixelFormat::FloatRGBA:
                        for (int c = 0; c < 4; ++c)
                        {
                            out[c] = to_unorm8(half_to_float(read_le16(p + i * 8 + static_cast<std::size_t>(c) * 2)));
                        }
                        break;
                    case PixelFormat::A1:
                    {
                        const std::size_t row = (width + 7) / 8;
                        const bool on =
                            ((std::to_integer<std::uint32_t>(p[y * row + x / 8]) >> (x % 8)) & 1) != 0;
                        out[0] = out[1] = out[2] = out[3] = on ? 255 : 0;
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        return image;
    }

    std::optional<std::size_t> Texture2D::first_inline_mip() const noexcept
    {
        for (std::size_t i = 0; i < mips.size(); ++i)
        {
            if (mips[i].data.is_inline() && mips[i].data.has_payload()) return i;
        }
        return std::nullopt;
    }

    std::optional<std::size_t> Texture2D::first_mip_with_payload() const noexcept
    {
        for (std::size_t i = 0; i < mips.size(); ++i)
        {
            if (mips[i].data.has_payload()) return i;
        }
        return std::nullopt;
    }

    bool is_texture2d_class(std::string_view class_name) noexcept
    {
        for (const std::string_view c : {"Texture2D", "LightMapTexture2D", "ShadowMapTexture2D",
                                         "TerrainWeightMapTexture", "Texture2DComposite", "TextureFlipBook",
                                         "Texture2DDynamic"})
        {
            if (Text::iequals(c, class_name)) return true;
        }
        return false;
    }

    namespace
    {
        std::vector<TextureMip> read_mips(BinaryReader& reader)
        {
            const std::uint32_t count = reader.read_array_count(24);
            std::vector<TextureMip> mips(count);
            for (TextureMip& mip : mips)
            {
                mip.data = read_bulk_data(reader);
                mip.size_x = reader.read_i32();
                mip.size_y = reader.read_i32();
            }
            return mips;
        }
    }

    Texture2D read_texture2d(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        const AssetSource source = open_asset(linker, object, schema);
        if (!is_texture2d_class(source.class_name))
        {
            throw Error(ErrorCode::InvalidArgument,
                        fmt::format("'{}' is a {}, not a Texture2D", linker.qualified_path(object), source.class_name));
        }
        Texture2D texture;
        texture.name = object.package->object_name(object.index);
        texture.class_name = source.class_name;
        const AssetProperties& p = source.properties;
        texture.size_x = p.get_int("SizeX").value_or(0);
        texture.size_y = p.get_int("SizeY").value_or(0);
        texture.format_name = p.get_name("Format").value_or("PF_Unknown");
        texture.format = pixel_format_from_name(texture.format_name);
        texture.srgb = p.get_bool("SRGB").value_or(true);
        texture.compression_settings = p.get_name("CompressionSettings").value_or("TC_Default");
        texture.lod_group = p.get_name("LODGroup").value_or("TEXTUREGROUP_World");
        texture.texture_file_cache = p.get_name("TextureFileCacheName").value_or("");
        if (texture.texture_file_cache == "None") texture.texture_file_cache.clear();
        {
            ObjectIndex outer = object.index;
            while (!object.package->outer(outer).is_null()) outer = object.package->outer(outer);
            texture.storage_package = outer == object.index ? object.package->name() : object.package->object_name(outer);
            // Only a Package object as outermost outer names another file; otherwise it is this package.
            if (outer != object.index && object.package->class_name(outer) != "Package")
            {
                texture.storage_package = object.package->name();
            }
        }

        BinaryReader reader = source.native_reader();
        texture.source_art = read_bulk_data(reader);
        texture.mips = read_mips(reader);
        if (reader.context().file_version >= 567) // AddedTextureFileCacheGuidToTexture2D (upstream)
        {
            texture.texture_file_cache_guid = reader.read_guid();
        }
        // Mobile caches of later UE3/UDK builds (upstream UTexture2D.Deserialize).
        const std::uint16_t version = reader.context().file_version;
        if (version >= 674) texture.pvrtc_mips = read_mips(reader); // AddedPVRTCToUTexture2D
        if (version >= 857)                                          // AddedATITCToUTexture2D
        {
            texture.flash_mip_max_resolution = reader.read_i32();
            texture.atitc_mips = read_mips(reader);
            texture.flash_mip_data = read_bulk_data(reader);
        }
        if (version >= 864) texture.etc_mips = read_mips(reader); // AddedETCToUTexture2D
        expect_end(reader, linker.qualified_path(object));
        texture.properties = source.properties;
        return texture;
    }

    std::vector<std::byte> load_mip(const Package& package, const Texture2D& texture, std::size_t mip,
                                    const Linker* linker)
    {
        if (mip >= texture.mips.size())
        {
            throw Error(ErrorCode::InvalidArgument,
                        fmt::format("texture '{}' has {} mips, not {}", texture.name, texture.mips.size(), mip + 1));
        }
        const BulkData& data = texture.mips[mip].data;
        if (!data.has_payload())
        {
            throw UnsupportedError(fmt::format("mip {} of '{}' has no payload (flags {})", mip, texture.name,
                                               data.describe_flags()));
        }
        if (data.is_inline())
        {
            return load_bulk_data(package, data);
        }
        if ((data.flags & BulkDataFlag::StoreInSeparateFile) == 0 || linker == nullptr)
        {
            throw UnsupportedError(fmt::format("mip {} of '{}' is stored outside the package (flags {})", mip,
                                               texture.name, data.describe_flags()));
        }
        // A texture with a TextureFileCacheName keeps its separate mips in <name>.tfc (UDK and later UE3 builds);
        // otherwise they are in the package that owns the texture (Mirror's Edge: the texture's outermost package).
        if (!texture.texture_file_cache.empty())
        {
            if (const auto cache = linker->find_texture_file_cache(texture.texture_file_cache))
            {
                return load_bulk_data_from_file(*cache, data, package.context());
            }
        }
        const auto file = linker->find_package_file(texture.storage_package);
        if (!file)
        {
            throw UnsupportedError(fmt::format(
                "mip {} of '{}' is stored in {}, which is not in the search paths", mip, texture.name,
                texture.texture_file_cache.empty() ? "package '" + texture.storage_package + "'"
                                                   : "texture file cache '" + texture.texture_file_cache + ".tfc'"));
        }
        return load_bulk_data_from_file(*file, data, package.context());
    }

    Image decode_mip(const Package& package, const Texture2D& texture, std::size_t mip, const Linker* linker)
    {
        const std::vector<std::byte> bytes = load_mip(package, texture, mip, linker);
        const TextureMip& m = texture.mips[mip];
        if (m.size_x <= 0 || m.size_y <= 0)
        {
            throw SerializationError(fmt::format("mip {} of '{}' has size {}x{}", mip, texture.name, m.size_x,
                                                 m.size_y));
        }
        return decode_pixels(bytes, texture.format, static_cast<std::uint32_t>(m.size_x),
                             static_cast<std::uint32_t>(m.size_y));
    }

    TextureCube read_texture_cube(Linker& linker, ObjectRef object, const PropertySchema* schema)
    {
        const AssetSource source = open_asset(linker, object, schema);
        if (!Text::iequals(source.class_name, "TextureCube"))
        {
            throw Error(ErrorCode::InvalidArgument, fmt::format("'{}' is a {}, not a TextureCube",
                                                                linker.qualified_path(object), source.class_name));
        }
        TextureCube cube;
        cube.name = object.package->object_name(object.index);
        constexpr std::array<std::string_view, 6> names{"FacePosX", "FaceNegX", "FacePosY",
                                                        "FaceNegY", "FacePosZ", "FaceNegZ"};
        for (std::size_t i = 0; i < 6; ++i)
        {
            cube.faces[i] = source.properties.get_object(names[i]).value_or(ObjectIndex{});
        }
        BinaryReader reader = source.native_reader();
        cube.source_art = read_bulk_data(reader);
        expect_end(reader, linker.qualified_path(object));
        cube.properties = source.properties;
        return cube;
    }
}
