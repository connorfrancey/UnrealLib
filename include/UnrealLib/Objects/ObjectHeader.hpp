#pragma once

/// @file
/// @brief The UObject serialization prefix that precedes an export's script properties.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Package/ObjectTables.hpp>

namespace UnrealLib
{
    class BinaryReader;
    class BinaryWriter;

    /// @brief One entry of a state frame's state stack (FPushedState).
    struct PushedState
    {
        ObjectIndex state;      ///< Pushed state.
        ObjectIndex node;       ///< Code node.
        std::int32_t offset = 0; ///< Code offset.

        /// @brief Memberwise comparison.
        friend bool operator==(const PushedState&, const PushedState&) = default;
    };

    /// @brief UStateFrame, serialized for objects with ObjectFlag::HasStack.
    ///
    /// Layout per upstream `UStateFrame.Deserialize`: Node, StateNode, ProbeMask (uint64 before v691, uint32 after),
    /// LatentAction (uint32 before v566, uint16 after), StateStack (>= v189), Offset (index, only if Node is not
    /// null).
    struct StateFrame
    {
        ObjectIndex node;                   ///< Executing struct.
        ObjectIndex state_node;             ///< Current state.
        std::uint64_t probe_mask = 0;       ///< Probe mask.
        std::uint32_t latent_action = 0;    ///< Latent action.
        std::vector<PushedState> state_stack; ///< Pushed states.
        std::optional<std::int32_t> offset; ///< Code offset (present when node is not null).

        /// @brief Memberwise comparison.
        friend bool operator==(const StateFrame&, const StateFrame&) = default;
    };

    /// @brief Template data serialized by UComponent instances (version >= 224).
    struct ComponentTemplate
    {
        ObjectIndex owner_class;           ///< TemplateOwnerClass.
        std::optional<FName> template_name; ///< TemplateName (see ObjectHeaderInput::template_name_present).

        /// @brief Memberwise comparison.
        friend bool operator==(const ComponentTemplate&, const ComponentTemplate&) = default;
    };

    /// @brief Facts about the export that decide which header fields are present.
    ///
    /// Determining these requires the class hierarchy (is the class a UComponent subclass?) and the outer chain's
    /// flags, which the caller — normally the Linker — supplies.
    struct ObjectHeaderInput
    {
        std::uint64_t object_flags = 0;      ///< The export's object flags.
        bool is_component = false;           ///< The export's class derives from Core.Component.
        bool is_class_default_template = false; ///< The export or one of its outers has ObjectFlag::ClassDefaultObject
                                                ///< (decides TemplateName for component sub-objects of CDOs).
        bool is_class = false;               ///< The export is a UClass (null class index); no properties follow.
    };

    /// @brief The deserialized UObject prefix of an export.
    struct ObjectHeader
    {
        std::optional<StateFrame> state_frame;     ///< Present when ObjectFlag::HasStack is set.
        std::optional<ComponentTemplate> component; ///< Present for components (version >= 224).
        std::optional<std::int32_t> net_index;     ///< NetIndex (version >= 322).
        std::size_t properties_offset = 0;         ///< Offset of the first property tag, relative to the export data.

        /// @brief Memberwise comparison.
        friend bool operator==(const ObjectHeader&, const ObjectHeader&) = default;
    };

    /// @brief Whether the component template carries a TemplateName for the given version/input.
    ///
    /// Upstream: present when `version < 267 || IsTemplate(ClassDefaultObject)`.
    [[nodiscard]] bool component_template_has_name(std::uint16_t file_version, const ObjectHeaderInput& input) noexcept;

    /// @brief Reads the UObject prefix from the start of an export's data.
    ///
    /// Class default objects (ObjectFlag::ClassDefaultObject): NetIndex (>= 322) only — upstream
    /// `UObject.Load` → `DeserializeClassDefault`, verified on Mirror's Edge component CDOs.
    /// Other objects (upstream `UObject.Deserialize`): StateFrame (HasStack), component template (components,
    /// >= 224), NetIndex (>= 322). In cooked Mirror's Edge packages NetIndex equals the export's 0-based index.
    /// Script properties follow at `properties_offset` unless `input.is_class`.
    [[nodiscard]] ObjectHeader read_object_header(BinaryReader& reader, const ObjectHeaderInput& input);

    /// @brief Writes the UObject prefix (inverse of read_object_header()).
    void write_object_header(BinaryWriter& writer, const ObjectHeader& header, const ObjectHeaderInput& input);
}
