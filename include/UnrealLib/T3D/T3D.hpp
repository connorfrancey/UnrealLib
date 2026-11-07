#pragma once

/// @file
/// @brief T3D ("Text 3D"): the editor's text format for maps, actors and objects — document model, parser,
///        serializer, validation, and export of package objects.
///
/// Grammar (line based, as written by UnrealEd):
///
/// ```text
/// Begin <Type> [Key=Value ...]      ; opens a block (Map, Level, Actor, Object, ...)
///    Name=Value                      ; property
///    Name(Index)=Value | Name[Index]=Value
///    Begin <Type> ...                ; nested block
///    End <Type>
/// End <Type>                         ; must match the innermost open block (case-insensitive)
/// ```
///
/// Values are kept as text (`(X=1.0,Y=2.0,Z=3.0)`, `"string"`, `Class'Package.Object'`, `None`, ...). A `=` inside
/// parentheses, quotes or a `Class'...'` reference does not split a property. Blank lines are ignored.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <UnrealLib/Core/Diagnostics.hpp>
#include <UnrealLib/Linker/Linker.hpp>

namespace UnrealLib::T3D
{
    /// @brief A property line.
    struct T3DProperty
    {
        std::string name;                 ///< Property name.
        std::optional<std::int32_t> index; ///< Array index (`Name(i)=` or `Name[i]=`).
        bool bracket_index = false;       ///< Index written with brackets (`Name[i]`) instead of parentheses.
        std::string value;                ///< Value text.
        std::size_t line = 0;             ///< Source line (1-based; 0 for constructed properties).

        /// @brief Memberwise comparison, ignoring the source line.
        friend bool operator==(const T3DProperty& a, const T3DProperty& b) noexcept
        {
            return a.name == b.name && a.index == b.index && a.bracket_index == b.bracket_index && a.value == b.value;
        }
    };

    /// @brief A `Begin <Type> ... End <Type>` block.
    struct T3DBlock
    {
        std::string type;                 ///< Map, Level, Actor, Object, PolyList, Polygon, ...
        std::vector<std::pair<std::string, std::string>> attributes; ///< Key=Value pairs of the Begin line.
        std::vector<T3DProperty> properties; ///< Properties, in order.
        std::vector<T3DBlock> children;   ///< Nested blocks, in order.
        std::vector<std::string> lines;   ///< Other lines (e.g. polygon `Vertex ...` data), in order.
        /// Order of the entries as parsed: 'p' property, 'l' line, 'c' child. serialize() follows it when it is
        /// consistent with the three lists, and writes properties, lines, then children otherwise.
        std::string layout;
        std::size_t line = 0;             ///< Line of the Begin statement (0 for constructed blocks).

        /// @brief Value of attribute @p key (case-insensitive), if present.
        [[nodiscard]] std::optional<std::string> attribute(std::string_view key) const;

        /// @brief The first property named @p name with index @p index (nullopt = unindexed), if present.
        [[nodiscard]] const T3DProperty* property(std::string_view name,
                                                  std::optional<std::int32_t> index = std::nullopt) const;

        /// @brief Memberwise comparison, ignoring source lines and layout.
        friend bool operator==(const T3DBlock& a, const T3DBlock& b) noexcept
        {
            return a.type == b.type && a.attributes == b.attributes && a.properties == b.properties &&
                   a.children == b.children && a.lines == b.lines;
        }
    };

    /// @brief A T3D document: top-level blocks.
    struct T3DDocument
    {
        std::vector<T3DBlock> blocks; ///< Top-level blocks (normally one Map).

        /// @brief Memberwise comparison.
        friend bool operator==(const T3DDocument&, const T3DDocument&) = default;
    };

    /// @brief Parses T3D text.
    /// @throws Error (InvalidData) naming the line for unbalanced blocks, mismatched End, properties outside a
    ///         block, malformed Begin lines or unterminated quotes.
    [[nodiscard]] T3DDocument parse(std::string_view text);

    /// @brief Formats @p document; nested blocks are indented by @p indent per level (UnrealEd uses 3 spaces).
    ///        parse(serialize(d)) == d for every document parse() can produce.
    [[nodiscard]] std::string serialize(const T3DDocument& document, std::string_view indent = "   ");

    /// @brief Semantic checks: Actor blocks have Class and Name; sibling blocks have unique names; a Map contains
    ///        Level blocks; references of the form `Name` or `Class'Name'` to subobjects of the same actor resolve.
    ///        Problems are reported as warnings ("t3d.*" codes) with their lines.
    [[nodiscard]] Diagnostics validate(const T3DDocument& document);

    /// @brief Options for export_object() / export_level().
    struct ExportOptions
    {
        const PropertySchema* schema = nullptr; ///< Type information (pass a ReflectionSchema for script structs).
        int max_subobject_depth = 8;            ///< Nesting limit for Begin Object blocks.
    };

    /// @brief Exports export @p object as a block of type @p block_type (`Actor` or `Object`): `Class=`, `Name=`
    ///        and `Archetype=` attributes, one `Begin Object` block per subobject (exports whose outer is the
    ///        object), then its properties. Values kept as raw bytes (no type information) are written in
    ///        format_property_value()'s raw notation and reported to @p diagnostics ("t3d.raw-value").
    [[nodiscard]] T3DBlock export_object(Linker& linker, ObjectRef object, std::string_view block_type,
                                         const ExportOptions& options = {}, Diagnostics* diagnostics = nullptr);

    /// @brief Exports the actors of every Level export of @p package (normally `TheWorld.PersistentLevel`) as
    ///        `Begin Map / Begin Level / Begin Actor ...`. Actors are the level's exports whose class derives from
    ///        Engine.Actor.
    /// @throws Error (NotFound) if the package has no Level export.
    [[nodiscard]] T3DDocument export_level(Linker& linker, const Package& package, const ExportOptions& options = {},
                                           Diagnostics* diagnostics = nullptr);
}
