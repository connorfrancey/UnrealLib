#pragma once

/// @file
/// @brief 8-bit RGBA images and image file encoders (PNG, TGA, DDS).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace UnrealLib::Assets
{
    /// @brief An 8-bit RGBA image, rows top to bottom.
    struct Image
    {
        std::uint32_t width = 0;          ///< Width in pixels.
        std::uint32_t height = 0;         ///< Height in pixels.
        std::vector<std::uint8_t> rgba;   ///< width * height * 4 bytes.

        /// @brief Creates a zero-filled image.
        [[nodiscard]] static Image create(std::uint32_t width, std::uint32_t height);

        /// @brief Pointer to pixel (x, y) (4 bytes: R, G, B, A). Precondition: in range.
        [[nodiscard]] std::uint8_t* at(std::uint32_t x, std::uint32_t y) noexcept
        {
            return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
        }

        /// @brief Encodes as PNG (8-bit RGBA, zlib-compressed).
        [[nodiscard]] std::vector<std::byte> to_png() const;

        /// @brief Encodes as uncompressed 32-bit TGA.
        [[nodiscard]] std::vector<std::byte> to_tga() const;

        /// @brief Writes the PNG encoding to @p path. @throws IoError on failure.
        void save_png(const std::filesystem::path& path) const;
    };

    /// @brief Surface formats a DDS file can carry without conversion.
    enum class DdsFormat : std::uint8_t
    {
        DXT1,     ///< FourCC DXT1.
        DXT3,     ///< FourCC DXT3.
        DXT5,     ///< FourCC DXT5.
        ATI2,     ///< FourCC ATI2 (BC5).
        A8R8G8B8, ///< 32-bit BGRA in memory.
        L8,       ///< 8-bit luminance.
    };

    /// @brief Builds a DDS file from already-encoded surfaces (largest mip first, each exactly surface-sized).
    [[nodiscard]] std::vector<std::byte> encode_dds(DdsFormat format, std::uint32_t width, std::uint32_t height,
                                                    std::span<const std::vector<std::byte>> mips);

    /// @brief Writes @p bytes to @p path, creating parent directories. @throws IoError on failure.
    void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes);
}
