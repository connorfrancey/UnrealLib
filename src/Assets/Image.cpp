#include <UnrealLib/Assets/Image.hpp>

#include <array>
#include <fstream>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Serialization/Compression.hpp>

namespace UnrealLib::Assets
{
    namespace
    {
        constexpr std::array<std::uint32_t, 256> make_crc_table()
        {
            std::array<std::uint32_t, 256> table{};
            for (std::uint32_t n = 0; n < 256; ++n)
            {
                std::uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                {
                    c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                }
                table[n] = c;
            }
            return table;
        }

        constexpr auto crc_table = make_crc_table();

        std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t crc = 0)
        {
            crc = ~crc;
            for (const std::byte b : data)
            {
                crc = crc_table[(crc ^ std::to_integer<std::uint32_t>(b)) & 0xFF] ^ (crc >> 8);
            }
            return ~crc;
        }

        void put_be32(std::vector<std::byte>& out, std::uint32_t v)
        {
            out.push_back(static_cast<std::byte>(v >> 24));
            out.push_back(static_cast<std::byte>(v >> 16));
            out.push_back(static_cast<std::byte>(v >> 8));
            out.push_back(static_cast<std::byte>(v));
        }

        void put_chunk(std::vector<std::byte>& out, const char (&type)[5], std::span<const std::byte> data)
        {
            put_be32(out, static_cast<std::uint32_t>(data.size()));
            const std::size_t start = out.size();
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(type[i]));
            out.insert(out.end(), data.begin(), data.end());
            put_be32(out, crc32(std::span<const std::byte>(out).subspan(start)));
        }
    }

    Image Image::create(std::uint32_t width, std::uint32_t height)
    {
        Image image;
        image.width = width;
        image.height = height;
        image.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
        return image;
    }

    std::vector<std::byte> Image::to_png() const
    {
        if (rgba.size() != static_cast<std::size_t>(width) * height * 4)
        {
            throw Error(ErrorCode::InvalidArgument, "image buffer does not match its size");
        }
        std::vector<std::byte> out;
        constexpr std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        for (const auto b : signature) out.push_back(static_cast<std::byte>(b));

        std::vector<std::byte> header;
        put_be32(header, width);
        put_be32(header, height);
        for (const std::uint8_t b : std::array<std::uint8_t, 5>{8, 6, 0, 0, 0}) header.push_back(static_cast<std::byte>(b)); // 8-bit RGBA
        put_chunk(out, "IHDR", header);

        // Filter type 0 (none) per row.
        std::vector<std::byte> raw;
        raw.reserve(static_cast<std::size_t>(height) * (width * 4 + 1));
        for (std::uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(std::byte{0});
            const auto* row = rgba.data() + static_cast<std::size_t>(y) * width * 4;
            for (std::size_t i = 0; i < static_cast<std::size_t>(width) * 4; ++i)
            {
                raw.push_back(static_cast<std::byte>(row[i]));
            }
        }
        put_chunk(out, "IDAT", compress_zlib(raw, 6));
        put_chunk(out, "IEND", {});
        return out;
    }

    std::vector<std::byte> Image::to_tga() const
    {
        BinaryWriter w;
        w.write_u8(0);  // id length
        w.write_u8(0);  // no color map
        w.write_u8(2);  // uncompressed true-color
        for (int i = 0; i < 5; ++i) w.write_u8(0);
        w.write_u16(0); // x origin
        w.write_u16(0); // y origin
        w.write_u16(static_cast<std::uint16_t>(width));
        w.write_u16(static_cast<std::uint16_t>(height));
        w.write_u8(32);
        w.write_u8(0x28); // 8 alpha bits, top-left origin
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
        {
            w.write_u8(rgba[i + 2]);
            w.write_u8(rgba[i + 1]);
            w.write_u8(rgba[i]);
            w.write_u8(rgba[i + 3]);
        }
        return w.take();
    }

    void Image::save_png(const std::filesystem::path& path) const
    {
        write_file(path, to_png());
    }

    std::vector<std::byte> encode_dds(DdsFormat format, std::uint32_t width, std::uint32_t height,
                                      std::span<const std::vector<std::byte>> mips)
    {
        constexpr std::uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8,
                                DDSD_PIXELFORMAT = 0x1000, DDSD_MIPMAPCOUNT = 0x20000, DDSD_LINEARSIZE = 0x80000;
        constexpr std::uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40, DDPF_LUMINANCE = 0x20000;
        constexpr std::uint32_t DDSCAPS_COMPLEX = 0x8, DDSCAPS_TEXTURE = 0x1000, DDSCAPS_MIPMAP = 0x400000;

        const bool compressed = format == DdsFormat::DXT1 || format == DdsFormat::DXT3 || format == DdsFormat::DXT5 ||
                                format == DdsFormat::ATI2;
        const auto fourcc = [](const char* s) {
            return static_cast<std::uint32_t>(s[0]) | static_cast<std::uint32_t>(s[1]) << 8 |
                   static_cast<std::uint32_t>(s[2]) << 16 | static_cast<std::uint32_t>(s[3]) << 24;
        };

        BinaryWriter w;
        w.write_u32(fourcc("DDS "));
        w.write_u32(124);
        std::uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
        flags |= compressed ? DDSD_LINEARSIZE : DDSD_PITCH;
        if (mips.size() > 1) flags |= DDSD_MIPMAPCOUNT;
        w.write_u32(flags);
        w.write_u32(height);
        w.write_u32(width);
        const std::uint32_t bpp = format == DdsFormat::A8R8G8B8 ? 4 : 1;
        w.write_u32(compressed ? (mips.empty() ? 0 : static_cast<std::uint32_t>(mips[0].size())) : width * bpp);
        w.write_u32(0); // depth
        w.write_u32(static_cast<std::uint32_t>(mips.size()));
        for (int i = 0; i < 11; ++i) w.write_u32(0);
        // DDS_PIXELFORMAT
        w.write_u32(32);
        switch (format)
        {
            case DdsFormat::DXT1:
            case DdsFormat::DXT3:
            case DdsFormat::DXT5:
            case DdsFormat::ATI2:
            {
                w.write_u32(DDPF_FOURCC);
                const char* code = format == DdsFormat::DXT1   ? "DXT1"
                                   : format == DdsFormat::DXT3 ? "DXT3"
                                   : format == DdsFormat::DXT5 ? "DXT5"
                                                               : "ATI2";
                w.write_u32(fourcc(code));
                for (int i = 0; i < 5; ++i) w.write_u32(0);
                break;
            }
            case DdsFormat::A8R8G8B8:
                w.write_u32(DDPF_RGB | DDPF_ALPHAPIXELS);
                w.write_u32(0);
                w.write_u32(32);
                w.write_u32(0x00FF0000);
                w.write_u32(0x0000FF00);
                w.write_u32(0x000000FF);
                w.write_u32(0xFF000000);
                break;
            case DdsFormat::L8:
                w.write_u32(DDPF_LUMINANCE);
                w.write_u32(0);
                w.write_u32(8);
                w.write_u32(0xFF);
                for (int i = 0; i < 3; ++i) w.write_u32(0);
                break;
        }
        w.write_u32(DDSCAPS_TEXTURE | (mips.size() > 1 ? DDSCAPS_COMPLEX | DDSCAPS_MIPMAP : 0));
        for (int i = 0; i < 4; ++i) w.write_u32(0);
        for (const auto& mip : mips) w.write_bytes(mip);
        return w.take();
    }

    void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes)
    {
        std::error_code ec;
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
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
}
