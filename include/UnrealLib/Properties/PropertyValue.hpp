#pragma once

/// @file
/// @brief Typed script property values, the type information needed to decode them, and property lists.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <UnrealLib/Core/Diagnostics.hpp>
#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Package/ObjectTables.hpp>
#include <UnrealLib/Properties/PropertyTag.hpp>
#include <UnrealLib/Serialization/FString.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    class BinaryReader;
    class BinaryWriter;
    class NameTable;

    struct Property;
    struct PropertyValue;

    /// @brief An enum value stored by name (tagged ByteProperty with an enum).
    struct EnumValue
    {
        FName value; ///< Enumerator name.
        /// @brief Memberwise comparison.
        friend bool operator==(const EnumValue&, const EnumValue&) = default;
    };

    /// @brief A NameProperty value.
    struct NameValue
    {
        FName value; ///< The name.
        /// @brief Memberwise comparison.
        friend bool operator==(const NameValue&, const NameValue&) = default;
    };

    /// @brief An object reference (Object/Class/Component/Interface properties).
    struct ObjectValue
    {
        ObjectIndex value; ///< Referenced object (null = none).
        /// @brief Memberwise comparison.
        friend bool operator==(const ObjectValue&, const ObjectValue&) = default;
    };

    /// @brief A DelegateProperty value (UE3: object then function name).
    struct DelegateValue
    {
        ObjectIndex object; ///< Object the function is bound to (may be null).
        FName function;     ///< Function name.
        /// @brief Memberwise comparison.
        friend bool operator==(const DelegateValue&, const DelegateValue&) = default;
    };

    /// @brief Bytes whose structure is unknown, kept verbatim.
    struct RawValue
    {
        std::vector<std::byte> bytes; ///< Serialized bytes.
        /// @brief Memberwise comparison.
        friend bool operator==(const RawValue&, const RawValue&) = default;
    };

    /// @brief How a struct value is serialized.
    enum class StructEncoding : std::uint8_t
    {
        Tagged, ///< A property-tag list terminated by "None".
        Binary, ///< Fields serialized back to back without tags (immutable / atomic structs).
    };

    /// @brief A struct value.
    struct StructValue
    {
        std::string struct_name;                    ///< Struct name, if known (empty otherwise).
        StructEncoding encoding = StructEncoding::Tagged; ///< Serialization form.
        std::vector<Property> fields;               ///< Fields in serialized order.
        /// @brief Memberwise comparison.
        friend bool operator==(const StructValue&, const StructValue&) = default;
    };

    /// @brief A dynamic array value.
    ///
    /// When the element type is known, `elements` holds one value per element. Otherwise `raw` holds the bytes after
    /// the element count and `elements` is empty.
    struct ArrayValue
    {
        std::int32_t count = 0;                 ///< Element count as serialized.
        std::vector<PropertyValue> elements;    ///< Decoded elements (empty when undecoded).
        std::optional<std::vector<std::byte>> raw; ///< Undecoded element bytes.
        /// @brief Whether the elements were decoded.
        [[nodiscard]] bool decoded() const noexcept { return !raw.has_value(); }
        /// @brief Memberwise comparison.
        friend bool operator==(const ArrayValue&, const ArrayValue&) = default;
    };

    /// @brief A decoded property value.
    struct PropertyValue
    {
        /// @brief Value alternatives. `std::monostate` = no value (e.g. an empty value of an unknown type).
        using Variant = std::variant<std::monostate, bool, std::uint8_t, EnumValue, std::int32_t, float, NameValue,
                                     FString, ObjectValue, DelegateValue, StructValue, ArrayValue, RawValue>;
        Variant value; ///< The value.

        /// @brief Memberwise comparison.
        friend bool operator==(const PropertyValue&, const PropertyValue&) = default;
    };

    /// @brief One property: a tagged property of an object / tagged struct, or a field of a binary struct.
    struct Property
    {
        std::string name;               ///< Property name.
        std::int32_t array_index = 0;   ///< Static-array element index.
        PropertyValue value;            ///< Value.
        std::optional<PropertyTag> tag; ///< On-disk tag (tagged properties only).

        /// @brief Memberwise comparison.
        friend bool operator==(const Property&, const Property&) = default;
    };

    /// @brief Description of a value type, as needed to decode untagged data (array elements, binary struct fields).
    struct TypeDesc
    {
        PropertyType type = PropertyType::Unknown;   ///< Kind.
        std::string struct_name;                     ///< Struct name (Struct).
        bool is_enum = false;                        ///< Byte with an enum.
        std::shared_ptr<const TypeDesc> inner;       ///< Element type (Array, FixedArray) or key type (Map).
        std::shared_ptr<const TypeDesc> value;       ///< Value type (Map).
        std::int32_t fixed_count = 0;                ///< Element count (FixedArray).
    };

    /// @brief A struct field for binary struct decoding.
    struct StructField
    {
        std::string name;            ///< Field name.
        TypeDesc type;               ///< Field type.
        std::int32_t array_dim = 1;  ///< Static array dimension.
    };

    /// @brief Serialization layout of a struct.
    struct StructLayout
    {
        std::string name;                 ///< Struct name.
        bool binary = false;              ///< Serialized without tags (immutable, or immutable-when-cooked in a cooked package).
        std::vector<StructField> fields;  ///< All fields including inherited ones, in serialization order.
    };

    /// @brief Type information source for decoding property values.
    ///
    /// Tags only say "ArrayProperty" or "StructProperty"; decoding array elements and telling binary from tagged
    /// structs needs the declaring class/struct. Implementations: BuiltinPropertySchema (core math structs, no
    /// package data) and the reflection-based schema built from UClass/UStruct exports.
    class PropertySchema
    {
    public:
        virtual ~PropertySchema() = default;

        /// @brief Layout of struct @p name, or nullptr if unknown.
        [[nodiscard]] virtual const StructLayout* find_struct(std::string_view name) const = 0;

        /// @brief Type of property @p property declared by class/struct @p owner or its supers, if known.
        [[nodiscard]] virtual std::optional<TypeDesc> find_property(std::string_view owner,
                                                                    std::string_view property) const = 0;
    };

    /// @brief Schema with the UE3 Core immutable structs (Vector, Vector2D, Vector4, Rotator, Color, LinearColor,
    ///        Guid, Plane, Sphere, Quat, Matrix, Box, IntPoint, TwoVectors) and no property information.
    class BuiltinPropertySchema final : public PropertySchema
    {
    public:
        BuiltinPropertySchema();

        /// @brief The shared instance.
        [[nodiscard]] static const BuiltinPropertySchema& instance();

        [[nodiscard]] const StructLayout* find_struct(std::string_view name) const override;
        [[nodiscard]] std::optional<TypeDesc> find_property(std::string_view owner,
                                                            std::string_view property) const override;

    private:
        std::unordered_map<std::string, StructLayout, Text::CaseInsensitiveHash, Text::CaseInsensitiveEqual> m_structs;
    };

    /// @brief Inputs for reading a property list.
    struct PropertyReadOptions
    {
        const NameTable* names = nullptr;           ///< Name table of the package (required).
        const PropertySchema* schema = nullptr;     ///< Type information; nullptr = BuiltinPropertySchema.
        std::string owner;                          ///< Class (or struct) name declaring the properties.
        Diagnostics* diagnostics = nullptr;         ///< Receives findings about undecodable values (optional).
        std::size_t max_depth = 32;                 ///< Maximum struct/array nesting.
    };

    /// @brief Reads a tagged property list up to and including the terminating "None" tag.
    ///
    /// Tags are mandatory and must be well formed (otherwise SerializationError). Values are decoded with the schema;
    /// a value that cannot be decoded, or whose decoding does not consume exactly the tag's size, is kept as a
    /// RawValue (and reported to diagnostics as "property.raw-value") — the list itself still parses, because every
    /// tag carries its value size.
    [[nodiscard]] std::vector<Property> read_properties(BinaryReader& reader, const PropertyReadOptions& options);

    /// @brief Writes a property list read by read_properties() (tags, values, then "None").
    ///
    /// Tag sizes are recomputed from the encoded values, so unmodified lists are written byte-identically.
    /// @param writer     Destination.
    /// @param properties The list to write.
    /// @param none       FName of "None" in the package's name table.
    void write_properties(BinaryWriter& writer, const std::vector<Property>& properties, const FName& none);

    /// @brief Writes a single value as it is serialized after a tag or as an array element / binary struct field.
    void write_property_value(BinaryWriter& writer, const PropertyValue& value, const FName& none);

    /// @brief Formatting inputs for format_property_value().
    struct PropertyFormatOptions
    {
        const NameTable* names = nullptr;                       ///< Resolves FNames (required).
        std::function<std::string(ObjectIndex)> object_name;    ///< Formats object references (default: "#index").
    };

    /// @brief Formats a value in UnrealScript defaultproperties syntax (e.g. `(X=1.0,Y=2.0,Z=0.0)`, `"text"`).
    [[nodiscard]] std::string format_property_value(const PropertyValue& value, const PropertyFormatOptions& options);

    /// @brief Inputs for parsing property text (parse_property_value()).
    struct PropertyParseOptions
    {
        /// Returns the FName for a name text, adding it to the package's name table if needed (required).
        std::function<FName(std::string_view)> make_name;
        /// Resolves an object reference as written by format_property_value() (anything but `None`); nullopt if the
        /// object is unknown (required for object and delegate values).
        std::function<std::optional<ObjectIndex>(std::string_view)> resolve_object;
        /// Struct layouts for values that have no template (new struct fields, elements of empty arrays);
        /// nullptr = BuiltinPropertySchema.
        const PropertySchema* schema = nullptr;
    };

    /// @brief Parses text in format_property_value() syntax into a value shaped like @p like.
    ///
    /// The template decides the type: its variant alternative, struct encoding, field tags, string encoding and the
    /// type of array elements (taken from the existing elements). Struct fields are matched by name and index;
    /// a tagged struct keeps only the fields present in the text (new fields need a struct layout in the schema),
    /// a binary struct keeps the template value of omitted fields. Raw values only accept their own raw notation.
    /// parse_property_value(format_property_value(v), v) serializes like v for every value, NaN payloads included
    /// (format_property_value() writes NaNs other than the default quiet ones as `nan(0xBITS)`).
    /// @param text Value text (`1.5`, `"str"`, `(X=1.0,Y=2.0,Z=3.0)`, `Class'Pkg.Obj'`, ...).
    /// @param like Template value.
    /// @param type Type of @p like, if known: used for elements of an empty array and new struct fields.
    /// @param options Name and object resolution.
    /// @throws Error (InvalidData) if the text does not match the type, or a name/reference cannot be resolved.
    [[nodiscard]] PropertyValue parse_property_value(std::string_view text, const PropertyValue& like,
                                                     const PropertyParseOptions& options,
                                                     const TypeDesc* type = nullptr);

    /// @brief Parses text into a value of type @p type (see the template overload).
    /// @throws Error (InvalidData) as the template overload; UnsupportedError for Map, FixedArray, Pointer and
    ///         Unknown types.
    [[nodiscard]] PropertyValue parse_property_value(std::string_view text, const TypeDesc& type,
                                                     const PropertyParseOptions& options);

    /// @brief The default value of @p type (zero, False, None, empty string/array; binary structs get every
    ///        field of their layout, tagged structs no fields).
    /// @throws UnsupportedError for Map, FixedArray, Pointer and Unknown types.
    [[nodiscard]] PropertyValue default_property_value(const TypeDesc& type, const PropertyParseOptions& options);

    /// @brief A property tag for a new property or tagged struct field of type @p type (size is recomputed when
    ///        written).
    /// @throws UnsupportedError for Unknown types.
    [[nodiscard]] PropertyTag make_property_tag(std::string_view name, std::int32_t array_index, const TypeDesc& type,
                                                const std::function<FName(std::string_view)>& make_name);

    /// @brief Finds the first property named @p name (case-insensitive) with array index @p array_index.
    [[nodiscard]] const Property* find_property(const std::vector<Property>& properties, std::string_view name,
                                                std::int32_t array_index = 0);
}
