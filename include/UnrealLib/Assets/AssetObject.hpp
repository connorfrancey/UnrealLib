#pragma once

/// @file
/// @brief Common base of asset readers: an export's tagged properties plus a reader over its native data.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>

namespace UnrealLib::Assets
{
    /// @brief An export's script properties, with typed accessors.
    ///
    /// Asset readers take their metadata (sizes, formats, references) from here: UE3 moved most asset fields into
    /// script properties, so only the payload (mips, sound data, vertex buffers) is native data.
    class AssetProperties
    {
    public:
        AssetProperties() = default;

        /// @param package    Package the properties belong to (resolves names).
        /// @param properties The export's tagged properties.
        AssetProperties(const Package& package, std::vector<Property> properties)
            : m_package(&package), m_properties(std::move(properties))
        {
        }

        /// @brief All properties, in serialized order.
        [[nodiscard]] const std::vector<Property>& all() const noexcept { return m_properties; }

        /// @brief The property named @p name, or nullptr.
        [[nodiscard]] const Property* find(std::string_view name, std::int32_t array_index = 0) const;

        [[nodiscard]] std::optional<std::int32_t> get_int(std::string_view name, std::int32_t index = 0) const; ///< IntProperty.
        [[nodiscard]] std::optional<float> get_float(std::string_view name, std::int32_t index = 0) const; ///< FloatProperty.
        [[nodiscard]] std::optional<bool> get_bool(std::string_view name, std::int32_t index = 0) const; ///< BoolProperty.
        [[nodiscard]] std::optional<std::uint8_t> get_byte(std::string_view name, std::int32_t index = 0) const; ///< Plain ByteProperty.
        /// @brief Name of a NameProperty, or enumerator name of an enum ByteProperty.
        [[nodiscard]] std::optional<std::string> get_name(std::string_view name, std::int32_t index = 0) const;
        [[nodiscard]] std::optional<std::string> get_string(std::string_view name, std::int32_t index = 0) const; ///< StrProperty.
        /// @brief Referenced object of an Object/Class/Component property (null references included).
        [[nodiscard]] std::optional<ObjectIndex> get_object(std::string_view name, std::int32_t index = 0) const;
        /// @brief Elements of an ArrayProperty (nullptr if absent or not decoded).
        [[nodiscard]] const ArrayValue* get_array(std::string_view name) const;
        /// @brief A StructProperty value (nullptr if absent).
        [[nodiscard]] const StructValue* get_struct(std::string_view name, std::int32_t index = 0) const;

        /// @brief Resolves an FName with the owning package's name table.
        [[nodiscard]] std::string to_string(const FName& name) const;

    private:
        const Package* m_package = nullptr;
        std::vector<Property> m_properties;
    };

    /// @brief An export opened for asset deserialization.
    struct AssetSource
    {
        ObjectRef object;              ///< The export.
        std::string class_name;        ///< Its class name.
        AssetProperties properties;    ///< Its tagged properties.
        std::size_t native_offset = 0; ///< Offset of the native data (just past the "None" tag), in the export data.

        /// @brief A reader over the export data, positioned at the native data. Its absolute positions are package
        ///        offsets, as bulk data records expect.
        [[nodiscard]] BinaryReader native_reader() const;
    };

    /// @brief Reads the properties of @p object and locates its native data.
    /// @throws Error (InvalidArgument) for imports and class default objects (`Default__X`, which have no native data).
    /// @param schema Type information (nullptr: BuiltinPropertySchema; arrays of script structs stay raw).
    [[nodiscard]] AssetSource open_asset(Linker& linker, ObjectRef object, const PropertySchema* schema = nullptr);

    /// @brief Throws SerializationError unless @p reader has consumed all of the export data of @p what.
    void expect_end(const BinaryReader& reader, std::string_view what);
}
