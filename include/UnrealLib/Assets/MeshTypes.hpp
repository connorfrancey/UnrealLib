#pragma once

/// @file
/// @brief Plain math and vertex types shared by the mesh and animation readers.

#include <array>
#include <cstdint>
#include <vector>

namespace UnrealLib::Assets
{
    /// @brief 3D vector (Unreal units; Z up, left-handed).
    struct Vec3
    {
        float x = 0; ///< X component.
        float y = 0; ///< Y component.
        float z = 0; ///< Z component.
        friend bool operator==(const Vec3&, const Vec3&) = default; ///< Memberwise comparison.
    };

    /// @brief 2D vector (texture coordinates).
    struct Vec2
    {
        float u = 0; ///< U coordinate.
        float v = 0; ///< V coordinate.
        friend bool operator==(const Vec2&, const Vec2&) = default; ///< Memberwise comparison.
    };

    /// @brief Quaternion (X, Y, Z, W).
    struct Quat
    {
        float x = 0; ///< X component.
        float y = 0; ///< Y component.
        float z = 0; ///< Z component.
        float w = 1; ///< W component.
        friend bool operator==(const Quat&, const Quat&) = default; ///< Memberwise comparison.
    };

    /// @brief 8-bit color, stored on disk as B, G, R, A.
    struct Color
    {
        std::uint8_t r = 0; ///< Red.
        std::uint8_t g = 0; ///< Green.
        std::uint8_t b = 0; ///< Blue.
        std::uint8_t a = 0; ///< Alpha.
        friend bool operator==(const Color&, const Color&) = default; ///< Memberwise comparison.
    };

    /// @brief FBoxSphereBounds.
    struct BoxSphereBounds
    {
        Vec3 origin;        ///< Center.
        Vec3 box_extent;    ///< Half size of the box.
        float sphere_radius = 0; ///< Radius of the bounding sphere.
    };

    /// @brief Decodes an FPackedNormal (4 bytes, each `b / 127.5 - 1`); returns X, Y, Z (W is dropped).
    [[nodiscard]] Vec3 unpack_normal(std::uint32_t packed) noexcept;
}
