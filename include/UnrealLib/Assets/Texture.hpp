#pragma once

/// @file
/// @brief UE3 textures (Texture2D and subclasses, TextureCube): metadata, mip maps, pixel formats and decoding.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <UnrealLib/Assets/AssetObject.hpp>
#include <UnrealLib/Assets/BulkData.hpp>
#include <UnrealLib/Assets/Image.hpp>
#include <UnrealLib/Core/Guid.hpp>

namespace UnrealLib::Assets
{
    /// @brief UE3 EPixelFormat values that occur in texture `Format` properties (stored by name, e.g. `PF_DXT1`).
    enum class PixelFormat : std::uint8_t
    {
        Unknown,
        A32B32G32R32F,
        A8R8G8B8,
        G8,
        G16,
        DXT1,
        DXT3,
        DXT5,
        UYVY,
        FloatRGB,
        FloatRGBA,
        DepthStencil,
        ShadowDepth,
        FilteredShadowDepth,
        R32F,
        G16R16,
        G16R16F,
        G32R32F,
        A2B10G10R10,
        A16B16G16R16,
        D24,
        R16F,
        BC5,
        V8U8,
        A1,
    };

    /// @brief Parses a `PF_*` enumerator name (case-insensitive). Unknown names give PixelFormat::Unknown.
    [[nodiscard]] PixelFormat pixel_format_from_name(std::string_view name) noexcept;

    /// @brief The `PF_*` name of @p format.
    [[nodiscard]] std::string_view to_string(PixelFormat format) noexcept;

    /// @brief Whether decode_pixels() supports @p format.
    [[nodiscard]] bool can_decode(PixelFormat format) noexcept;

    /// @brief Bytes needed for one @p width x @p height surface in @p format (block formats round up to 4x4 blocks).
    /// @return std::nullopt for formats without a fixed size (Unknown, depth formats).
    [[nodiscard]] std::optional<std::uint64_t> surface_size(PixelFormat format, std::uint32_t width,
                                                            std::uint32_t height) noexcept;

    /// @brief Decodes one surface to 8-bit RGBA.
    ///
    /// Supported: DXT1, DXT3, DXT5, BC5 (normal maps; Z reconstructed), A8R8G8B8, G8, G16, V8U8 (Z reconstructed),
    /// A32B32G32R32F / FloatRGBA / FloatRGB (clamped to [0, 1]), A1.
    /// @throws UnsupportedError for other formats; SerializationError if @p data is smaller than surface_size().
    [[nodiscard]] Image decode_pixels(std::span<const std::byte> data, PixelFormat format, std::uint32_t width,
                                      std::uint32_t height);

    /// @brief One mip level of a 2D texture.
    struct TextureMip
    {
        BulkData data;           ///< Pixel payload.
        std::int32_t size_x = 0; ///< Width in pixels.
        std::int32_t size_y = 0; ///< Height in pixels.
    };

    /// @brief A Texture2D (or subclass: LightMapTexture2D, ShadowMapTexture2D, TerrainWeightMapTexture, ...).
    ///
    /// Native layout (file version 536, verified on every Mirror's Edge texture): UTexture `SourceArt` bulk data,
    /// then the `Mips` array (bulk data, int32 SizeX, int32 SizeY each); from version 567 a TextureFileCacheGuid.
    /// The subclasses add no native data at version 536.
    struct Texture2D
    {
        std::string name;                 ///< Object name.
        std::string class_name;           ///< Class (Texture2D, LightMapTexture2D, ...).
        std::int32_t size_x = 0;          ///< Width (SizeX property).
        std::int32_t size_y = 0;          ///< Height (SizeY property).
        PixelFormat format = PixelFormat::Unknown; ///< Pixel format (Format property).
        std::string format_name;          ///< The Format enumerator as stored.
        bool srgb = true;                 ///< SRGB property.
        std::string compression_settings; ///< CompressionSettings enumerator ("TC_Default", "TC_Normalmap", ...).
        std::string lod_group;            ///< LODGroup enumerator.
        std::string texture_file_cache;   ///< TextureFileCacheName (empty: none).
        std::string storage_package;      ///< Outermost package of the object: holds `StoreInSeparateFile` mips.
        BulkData source_art;              ///< Uncooked source art (empty in cooked packages).
        std::vector<TextureMip> mips;     ///< Mip levels, largest first.
        std::optional<Guid> texture_file_cache_guid; ///< Version >= 567.
        AssetProperties properties;       ///< All tagged properties.

        /// @brief Index of the largest mip whose payload is stored in this package, if any.
        [[nodiscard]] std::optional<std::size_t> first_inline_mip() const noexcept;

        /// @brief Index of the largest mip that has a payload (inline or in a separate file), if any.
        [[nodiscard]] std::optional<std::size_t> first_mip_with_payload() const noexcept;
    };

    /// @brief Whether class @p class_name is read by read_texture2d().
    [[nodiscard]] bool is_texture2d_class(std::string_view class_name) noexcept;

    /// @brief Reads a Texture2D (or subclass) export. The native data must be consumed exactly.
    /// @throws Error (InvalidArgument) for other classes; SerializationError on malformed data.
    [[nodiscard]] Texture2D read_texture2d(Linker& linker, ObjectRef object, const PropertySchema* schema = nullptr);

    /// @brief Loads the pixel bytes of mip @p mip (in the texture's own format, e.g. DXT blocks) of @p texture, which
    ///        was read from @p package. Mips stored in a separate file are read from the file of
    ///        `texture.storage_package`, located through @p linker's search paths (pass nullptr to allow inline mips
    ///        only).
    /// @throws UnsupportedError if the mip has no payload or its file is unknown; IoError / SerializationError /
    ///         CompressionError on bad data.
    [[nodiscard]] std::vector<std::byte> load_mip(const Package& package, const Texture2D& texture, std::size_t mip,
                                                  const Linker* linker = nullptr);

    /// @brief Loads and decodes mip @p mip to RGBA (see load_mip()).
    /// @throws UnsupportedError also if the pixel format is not decodable.
    [[nodiscard]] Image decode_mip(const Package& package, const Texture2D& texture, std::size_t mip,
                                   const Linker* linker = nullptr);

    /// @brief A TextureCube: six face textures (properties FacePosX, FaceNegX, FacePosY, FaceNegY, FacePosZ, FaceNegZ).
    struct TextureCube
    {
        std::string name;                    ///< Object name.
        std::array<ObjectIndex, 6> faces{}; ///< +X, -X, +Y, -Y, +Z, -Z (null if missing).
        BulkData source_art;                 ///< UTexture source art.
        AssetProperties properties;          ///< All tagged properties.
    };

    /// @brief Reads a TextureCube export.
    [[nodiscard]] TextureCube read_texture_cube(Linker& linker, ObjectRef object,
                                                const PropertySchema* schema = nullptr);
}
