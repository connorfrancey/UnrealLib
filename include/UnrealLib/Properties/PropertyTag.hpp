#pragma once

/// @file
/// @brief Script property tags (UE3 FPropertyTag): the header in front of every tagged property value.

#include <cstdint>
#include <optional>
#include <string_view>

#include <UnrealLib/Names/FName.hpp>

namespace UnrealLib
{
    class BinaryReader;
    class BinaryWriter;
    class NameTable;

    /// @brief Kind of a script property, as named by a property tag's type field or a UProperty class.
    enum class PropertyType : std::uint8_t
    {
        Unknown,           ///< Type name not recognised (value kept as raw bytes).
        Byte,              ///< ByteProperty (plain byte or enum).
        Int,               ///< IntProperty.
        Bool,              ///< BoolProperty.
        Float,             ///< FloatProperty.
        Object,            ///< ObjectProperty.
        Name,              ///< NameProperty.
        Delegate,          ///< DelegateProperty.
        Class,             ///< ClassProperty.
        Array,             ///< ArrayProperty (dynamic array).
        Struct,            ///< StructProperty.
        Str,               ///< StrProperty (FString).
        Map,               ///< MapProperty.
        Interface,         ///< InterfaceProperty.
        Component,         ///< ComponentProperty.
        String,            ///< StringProperty (fixed-size pre-UE3 string).
        FixedArray,        ///< FixedArrayProperty.
        Pointer,           ///< PointerProperty.
    };

    /// @brief The UnrealScript class name of @p type (e.g. "IntProperty"); "Unknown" for PropertyType::Unknown.
    [[nodiscard]] std::string_view to_string(PropertyType type) noexcept;

    /// @brief Parses a property class name ("IntProperty", case-insensitive). Unrecognised names give Unknown.
    [[nodiscard]] PropertyType parse_property_type(std::string_view name) noexcept;

    /// @brief A UE3 property tag.
    ///
    /// Layout for file versions >= 223 (`RefactoredPropertyTags`, upstream `UDefaultProperty.DeserializeTagUE3`):
    /// Name (FName; "None" terminates the list), Type (FName), Size (int32, size of the value that follows),
    /// ArrayIndex (int32), then type data: StructName (FName) for structs, EnumName (FName) for bytes (>= 633),
    /// BoolValue (int32 before 673, byte from 673) for bools. Bool tags carry their value in the tag and have Size 0.
    ///
    /// The on-disk FNames are kept so that writing a tag reproduces the original bytes.
    struct PropertyTag
    {
        FName name;                         ///< Property name.
        FName type_name;                    ///< Type name as stored ("IntProperty", …).
        PropertyType type = PropertyType::Unknown; ///< Parsed type.
        std::int32_t size = 0;              ///< Size in bytes of the value following the tag.
        std::int32_t array_index = 0;       ///< Static-array element index.
        std::optional<FName> struct_name;   ///< Struct name (StructProperty).
        std::optional<FName> enum_name;     ///< Enum name (ByteProperty, version >= 633).
        std::optional<bool> bool_value;     ///< Value (BoolProperty).

        /// @brief Memberwise comparison.
        friend bool operator==(const PropertyTag&, const PropertyTag&) = default;
    };

    /// @brief Package versions that change the tag layout. Values from upstream `PackageObjectLegacyVersion`.
    namespace PropertyTagVersion
    {
        inline constexpr std::uint16_t RefactoredPropertyTags = 223;          ///< UE3 tag layout (supported from here).
        inline constexpr std::uint16_t EnumNameAddedToBytePropertyTag = 633;  ///< Byte tags carry EnumName.
        inline constexpr std::uint16_t BoolValueToByteForBoolPropertyTag = 673; ///< Bool value shrinks to one byte.
    }

    /// @brief Reads one property tag.
    /// @return std::nullopt for the terminating "None" tag (only its name is consumed).
    /// @throws UnsupportedError for versions < 223; SerializationError for invalid name indices, negative sizes or
    ///         sizes larger than the remaining data.
    [[nodiscard]] std::optional<PropertyTag> read_property_tag(BinaryReader& reader, const NameTable& names);

    /// @brief Writes a tag (inverse of read_property_tag()). The caller writes the value afterwards.
    void write_property_tag(BinaryWriter& writer, const PropertyTag& tag);

    /// @brief Writes the terminating "None" tag. @p none is the FName of "None" in the target name table.
    void write_property_terminator(BinaryWriter& writer, const FName& none);
}
