#pragma once

/// @file
/// @brief UnrealScript reflection objects (UField and subclasses): classes, states, functions, structs, enums,
///        constants, properties and text buffers.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Objects/ObjectHeader.hpp>
#include <UnrealLib/Package/ObjectTables.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/FString.hpp>

namespace UnrealLib
{
    class BinaryReader;
    class NameTable;

    /// @brief Which UField subclass an export is.
    enum class FieldKind : std::uint8_t
    {
        Class,        ///< UClass.
        State,        ///< UState.
        Function,     ///< UFunction.
        ScriptStruct, ///< UScriptStruct.
        Struct,       ///< UStruct (plain; rare).
        Enum,         ///< UEnum.
        Const,        ///< UConst.
        Property,     ///< One of the U*Property classes (see FieldObject::property->type).
        TextBuffer,   ///< UTextBuffer (not a UField, but part of class data).
    };

    /// @brief "Class", "State", … for @p kind.
    [[nodiscard]] std::string_view to_string(FieldKind kind) noexcept;

    /// @brief Classifies a class name ("Class", "Function", "IntProperty", "TextBuffer", …).
    /// @param class_name Name of the export's class.
    /// @param property_type Receives the property type for FieldKind::Property.
    [[nodiscard]] std::optional<FieldKind> field_kind_of(std::string_view class_name,
                                                         PropertyType* property_type = nullptr) noexcept;

    /// @name Flag constants (UE3, upstream `UnrealFlags.cs`)
    /// @{
    namespace StructFlag
    {
        inline constexpr std::uint32_t Native = 0x00000001u;              ///< C++ struct.
        inline constexpr std::uint32_t Atomic = 0x00000010u;              ///< Atomic.
        inline constexpr std::uint32_t Immutable = 0x00000020u;           ///< Always serialized in binary form.
        inline constexpr std::uint32_t ImmutableWhenCooked = 0x00000080u; ///< Binary in cooked packages.
        inline constexpr std::uint32_t AtomicWhenCooked = 0x00000100u;    ///< Atomic in cooked packages.
    }
    namespace FunctionFlag
    {
        inline constexpr std::uint32_t Final = 0x00000001u;       ///< final.
        inline constexpr std::uint32_t Defined = 0x00000002u;     ///< Has a body.
        inline constexpr std::uint32_t Iterator = 0x00000004u;    ///< iterator.
        inline constexpr std::uint32_t Latent = 0x00000008u;      ///< latent.
        inline constexpr std::uint32_t PreOperator = 0x00000010u; ///< preoperator.
        inline constexpr std::uint32_t Singular = 0x00000020u;    ///< singular.
        inline constexpr std::uint32_t Net = 0x00000040u;         ///< Replicated (RepOffset follows).
        inline constexpr std::uint32_t NetReliable = 0x00000080u; ///< reliable.
        inline constexpr std::uint32_t Simulated = 0x00000100u;   ///< simulated.
        inline constexpr std::uint32_t Exec = 0x00000200u;        ///< exec.
        inline constexpr std::uint32_t Native = 0x00000400u;      ///< native.
        inline constexpr std::uint32_t Event = 0x00000800u;       ///< event.
        inline constexpr std::uint32_t Operator = 0x00001000u;    ///< operator.
        inline constexpr std::uint32_t Static = 0x00002000u;      ///< static.
        inline constexpr std::uint32_t HasOptionalParms = 0x00004000u; ///< Has optional parameters.
        inline constexpr std::uint32_t Const = 0x00008000u;       ///< const.
        inline constexpr std::uint32_t Public = 0x00020000u;      ///< public.
        inline constexpr std::uint32_t Private = 0x00040000u;     ///< private.
        inline constexpr std::uint32_t Protected = 0x00080000u;   ///< protected.
        inline constexpr std::uint32_t Delegate = 0x00100000u;    ///< delegate.
        inline constexpr std::uint32_t NetServer = 0x00200000u;   ///< server.
        inline constexpr std::uint32_t HasOutParms = 0x00400000u; ///< Has out parameters.
        inline constexpr std::uint32_t HasDefaults = 0x00800000u; ///< Has default parameter values.
        inline constexpr std::uint32_t NetClient = 0x01000000u;   ///< client.
        inline constexpr std::uint32_t DLLImport = 0x02000000u;   ///< dllimport.
    }
    namespace PropertyFlag
    {
        inline constexpr std::uint64_t Edit = 0x00000001ull;        ///< Editable.
        inline constexpr std::uint64_t Const = 0x00000002ull;       ///< const.
        inline constexpr std::uint64_t Input = 0x00000004ull;       ///< input.
        inline constexpr std::uint64_t ExportObject = 0x00000008ull; ///< export.
        inline constexpr std::uint64_t OptionalParm = 0x00000010ull; ///< optional parameter.
        inline constexpr std::uint64_t Net = 0x00000020ull;         ///< Replicated (RepOffset follows).
        inline constexpr std::uint64_t EditFixedSize = 0x00000040ull; ///< editfixedsize.
        inline constexpr std::uint64_t Parm = 0x00000080ull;        ///< Function parameter.
        inline constexpr std::uint64_t OutParm = 0x00000100ull;     ///< out parameter.
        inline constexpr std::uint64_t SkipParm = 0x00000200ull;    ///< skip parameter.
        inline constexpr std::uint64_t ReturnParm = 0x00000400ull;  ///< Return value.
        inline constexpr std::uint64_t CoerceParm = 0x00000800ull;  ///< coerce parameter.
        inline constexpr std::uint64_t Native = 0x00001000ull;      ///< native.
        inline constexpr std::uint64_t Transient = 0x00002000ull;   ///< transient.
        inline constexpr std::uint64_t Config = 0x00004000ull;      ///< config.
        inline constexpr std::uint64_t Localized = 0x00008000ull;   ///< localized.
        inline constexpr std::uint64_t EditConst = 0x00020000ull;   ///< editconst.
        inline constexpr std::uint64_t GlobalConfig = 0x00040000ull; ///< globalconfig.
        inline constexpr std::uint64_t Component = 0x00080000ull;   ///< Component reference.
        inline constexpr std::uint64_t AlwaysInit = 0x00100000ull;  ///< init.
        inline constexpr std::uint64_t DuplicateTransient = 0x00200000ull; ///< duplicatetransient.
        inline constexpr std::uint64_t NeedCtorLink = 0x00400000ull; ///< Needs construction.
        inline constexpr std::uint64_t NoExport = 0x00800000ull;    ///< noexport.
        inline constexpr std::uint64_t NoImport = 0x01000000ull;    ///< noimport.
        inline constexpr std::uint64_t NoClear = 0x02000000ull;     ///< noclear.
        inline constexpr std::uint64_t EditInline = 0x04000000ull;  ///< editinline.
        inline constexpr std::uint64_t EditInlineUse = 0x10000000ull; ///< editinlineuse.
        inline constexpr std::uint64_t Deprecated = 0x20000000ull;  ///< deprecated.
        inline constexpr std::uint64_t DataBinding = 0x40000000ull; ///< databinding.
        inline constexpr std::uint64_t SerializeText = 0x80000000ull; ///< serializetext.
        inline constexpr std::uint64_t RepNotify = 0x0000000100000000ull; ///< repnotify.
        inline constexpr std::uint64_t Interp = 0x0000000200000000ull;   ///< interp.
        inline constexpr std::uint64_t NonTransactional = 0x0000000400000000ull; ///< nontransactional.
        inline constexpr std::uint64_t EditorOnly = 0x0000000800000000ull; ///< editoronly.
        inline constexpr std::uint64_t PrivateWrite = 0x0000080000000000ull; ///< privatewrite.
        inline constexpr std::uint64_t ProtectedWrite = 0x0000100000000000ull; ///< protectedwrite.
        inline constexpr std::uint64_t ArchetypeProperty = 0x0000200000000000ull; ///< archetype.
        inline constexpr std::uint64_t EditHide = 0x0000400000000000ull; ///< edithide.
        inline constexpr std::uint64_t EditTextBox = 0x0000800000000000ull; ///< edittextbox.
        inline constexpr std::uint64_t CrossLevelPassive = 0x0001000000000000ull; ///< crosslevelpassive.
        inline constexpr std::uint64_t CrossLevelActive = 0x0002000000000000ull; ///< crosslevelactive.
    }
    namespace ClassFlag
    {
        inline constexpr std::uint32_t Abstract = 0x00000001u;     ///< abstract.
        inline constexpr std::uint32_t Compiled = 0x00000002u;     ///< Compiled.
        inline constexpr std::uint32_t Config = 0x00000004u;       ///< config.
        inline constexpr std::uint32_t Transient = 0x00000008u;    ///< transient.
        inline constexpr std::uint32_t Parsed = 0x00000010u;       ///< Parsed.
        inline constexpr std::uint32_t Localized = 0x00000020u;    ///< localized.
        inline constexpr std::uint32_t SafeReplace = 0x00000040u;  ///< safereplace.
        inline constexpr std::uint32_t Native = 0x00000080u;       ///< native.
        inline constexpr std::uint32_t NoExport = 0x00000100u;     ///< noexport.
        inline constexpr std::uint32_t Placeable = 0x00000200u;    ///< placeable.
        inline constexpr std::uint32_t PerObjectConfig = 0x00000400u; ///< perobjectconfig.
        inline constexpr std::uint32_t NativeReplication = 0x00000800u; ///< nativereplication.
        inline constexpr std::uint32_t EditInlineNew = 0x00001000u; ///< editinlinenew.
        inline constexpr std::uint32_t CollapseCategories = 0x00002000u; ///< collapsecategories.
        inline constexpr std::uint32_t Interface = 0x00004000u;    ///< interface.
        inline constexpr std::uint32_t HasInstancedProps = 0x00200000u; ///< Has instanced (component) properties.
        inline constexpr std::uint32_t NeedsDefProps = 0x00400000u; ///< Needs default properties.
        inline constexpr std::uint32_t HasComponents = 0x00800000u; ///< Has components.
        inline constexpr std::uint32_t Hidden = 0x01000000u;       ///< hidden.
        inline constexpr std::uint32_t Deprecated = 0x02000000u;   ///< deprecated.
        inline constexpr std::uint32_t HideDropDown = 0x04000000u; ///< hidedropdown.
        inline constexpr std::uint32_t Exported = 0x08000000u;     ///< Exported.
        inline constexpr std::uint32_t Intrinsic = 0x10000000u;    ///< Intrinsic.
        inline constexpr std::uint32_t NativeOnly = 0x20000000u;   ///< nativeonly.
    }
    namespace StateFlag
    {
        inline constexpr std::uint32_t Editable = 0x00000001u; ///< Editable state.
        inline constexpr std::uint32_t Auto = 0x00000002u;     ///< auto state.
        inline constexpr std::uint32_t Simulated = 0x00000004u; ///< simulated state.
    }
    /// @}

    /// @brief UField data.
    struct FieldData
    {
        ObjectIndex super; ///< Super struct (UField before version 756, UStruct from 756).
        ObjectIndex next;  ///< Next field in the owner's children list.
    };

    /// @brief UStruct data.
    struct StructData
    {
        ObjectIndex script_text;            ///< UTextBuffer with the source (null when stripped).
        ObjectIndex children;               ///< First child field.
        ObjectIndex cpp_text;               ///< UTextBuffer with cpptext (version >= 120).
        std::int32_t line = 0;              ///< Source line.
        std::int32_t text_pos = 0;          ///< Source text position.
        std::int32_t script_size = 0;       ///< Bytecode size in memory units (ScriptSize).
        std::size_t script_offset = 0;      ///< Offset of the bytecode in the export data.
        std::size_t script_storage_size = 0; ///< Serialized size of the bytecode.
    };

    /// @brief UFunction data.
    struct FunctionData
    {
        std::uint16_t native_token = 0;      ///< iNative.
        std::uint8_t oper_precedence = 0;    ///< Operator precedence.
        std::uint32_t flags = 0;             ///< FunctionFlag bits.
        std::optional<std::uint16_t> rep_offset; ///< Replication offset (FunctionFlag::Net).
        std::optional<FName> friendly_name;  ///< Friendly name (absent in console-cooked packages).
    };

    /// @brief UState data.
    struct StateData
    {
        std::uint64_t probe_mask = 0;           ///< Probe mask (64-bit before 691).
        std::optional<std::uint64_t> ignore_mask; ///< Ignore mask (before 691).
        std::uint16_t label_table_offset = 0;   ///< LabelTable offset in the bytecode.
        std::uint32_t flags = 0;                ///< StateFlag bits.
        std::vector<std::pair<FName, ObjectIndex>> function_map; ///< FuncMap (name → function).
    };

    /// @brief UClass data.
    struct ClassData
    {
        std::uint32_t flags = 0;                       ///< ClassFlag bits.
        std::optional<std::uint8_t> platform_flags;    ///< ClassPlatformFlags (276 <= v < 547).
        ObjectIndex within;                            ///< Within class.
        FName config_name;                             ///< Config file name.
        std::vector<FName> hide_categories;            ///< hidecategories.
        std::vector<std::pair<FName, ObjectIndex>> component_defaults; ///< ComponentDefaultObjectMap.
        std::vector<std::pair<ObjectIndex, ObjectIndex>> interfaces; ///< Implemented interfaces (class, property).
        std::vector<FName> auto_expand_categories;     ///< autoexpandcategories.
        ObjectIndex default_object;                    ///< Class default object (Default__X).
    };

    /// @brief UScriptStruct data.
    struct ScriptStructData
    {
        std::uint32_t flags = 0;            ///< StructFlag bits.
        std::vector<Property> defaults;     ///< Struct default properties (tagged).
    };

    /// @brief U*Property data.
    struct PropertyData
    {
        PropertyType type = PropertyType::Unknown; ///< Property class.
        std::int32_t array_dim = 1;         ///< Static array dimension (low 16 bits; high bits = element size).
        std::uint64_t flags = 0;            ///< PropertyFlag bits.
        std::optional<FName> category;      ///< Editor category (absent in console-cooked packages).
        ObjectIndex array_enum;             ///< Enum sizing a static array (version >= 401).
        std::optional<std::uint16_t> rep_offset; ///< Replication offset (PropertyFlag::Net).
        /// Type reference: Byte → enum, Object/Component/Interface → class, Class → class (meta class in
        /// `reference2`), Struct → struct, Array/FixedArray → inner property, Map → key property (value in
        /// `reference2`), Delegate → function (delegate source in `reference2`).
        ObjectIndex reference;
        ObjectIndex reference2;             ///< See `reference`.
        std::int32_t fixed_count = 0;       ///< FixedArray element count.
        /// @brief Static array dimension without element-size bits.
        [[nodiscard]] std::int32_t dimension() const noexcept { return array_dim & 0xFFFF; }
    };

    /// @brief UTextBuffer data.
    struct TextBufferData
    {
        std::uint32_t top = 0; ///< Top line.
        std::uint32_t pos = 0; ///< Cursor position.
        FString text;          ///< Text.
    };

    /// @brief A deserialized reflection object.
    struct FieldObject
    {
        FieldKind kind = FieldKind::Property;    ///< Kind.
        ObjectHeader header;                     ///< UObject header.
        std::vector<Property> properties;        ///< Tagged properties preceding the UField data (usually none).
        FieldData field;                         ///< UField data (not for TextBuffer).
        std::optional<StructData> structure;     ///< Struct, ScriptStruct, Function, State, Class.
        std::optional<FunctionData> function;    ///< Function.
        std::optional<StateData> state;          ///< State, Class.
        std::optional<ClassData> class_data;     ///< Class.
        std::optional<ScriptStructData> script_struct; ///< ScriptStruct.
        std::vector<FName> enum_names;           ///< Enum.
        std::optional<FString> const_value;      ///< Const.
        std::optional<PropertyData> property;    ///< Property.
        std::optional<TextBufferData> text_buffer; ///< TextBuffer.
        std::vector<Property> struct_defaults;   ///< Plain UStruct default properties.
        std::size_t end_offset = 0;              ///< Bytes consumed from the export data.
    };

    /// @brief Inputs of read_field().
    struct FieldReadOptions
    {
        const NameTable* names = nullptr;       ///< Name table (required).
        bool console_cooked = false;            ///< Console-cooked package: editor-only fields are stripped.
        bool cooked = false;                    ///< Package is cooked (PackageFlag::Cooked).
        std::string owner_name;                 ///< Object name (owner of ScriptStruct defaults).
        const PropertySchema* schema = nullptr; ///< Type information for struct defaults.
        Diagnostics* diagnostics = nullptr;     ///< Receives property decoding findings.
    };

    /// @brief Reads a reflection object of kind @p kind from the start of its export data.
    ///
    /// Layout (upstream `UObject`/`UField`/`UStruct`/`UState`/`UClass`/`UFunction`/`U*Property`/`UEnum`/`UConst`/
    /// `UTextBuffer` `Deserialize`, default UE3 branch). Classes have no tagged properties before the UField data;
    /// every other kind does. Bytecode is skipped using ScriptSize (in-memory size); for versions where memory and
    /// storage sizes differ (>= 587, or < 343) the bytecode is decoded to find its end.
    /// @param property_type For FieldKind::Property, the concrete property class.
    [[nodiscard]] FieldObject read_field(BinaryReader& reader, FieldKind kind, PropertyType property_type,
                                         const ObjectHeaderInput& input, const FieldReadOptions& options);
}
