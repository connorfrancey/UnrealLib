#pragma once

/// @file
/// @brief UnrealScript bytecode decoding (UE3): token map, token tree, decoder.
///
/// Decoding is separate from decompiling (UnrealLib::Decompiler): the decoder only turns bytes into a tree of
/// tokens with their operands, without interpreting control flow.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Package/ObjectTables.hpp>

namespace UnrealLib
{
    class BinaryReader;
    class BinaryWriter;
    struct GameProfile;

    /// @brief Kind of a bytecode token. Names follow upstream UELib token classes (without "Token").
    enum class TokenKind : std::uint8_t
    {
        Bad,                    ///< Invalid opcode; decoding stops with an error.
        LocalVariable,          ///< obj
        InstanceVariable,       ///< obj
        DefaultVariable,        ///< obj
        StateVariable,          ///< obj
        Return,                 ///< expr
        Switch,                 ///< [obj] type, expr
        Jump,                   ///< u16 target
        JumpIfNot,              ///< u16 target, expr
        Stop,                   ///< —
        Assert,                 ///< u16 line, [u8 debug], expr
        Case,                   ///< u16 next (0xFFFF = default), [expr]
        Nothing,                ///< —
        LabelTable,             ///< (name, int32)… until "None"
        GotoLabel,              ///< expr
        EatReturnValue,         ///< [obj]
        Let,                    ///< expr, expr
        DynamicArrayElement,    ///< expr (index), expr (array)
        New,                    ///< expr ×4 or ×5
        ClassContext,           ///< like Context
        MetaClassCast,          ///< obj, expr
        LetBool,                ///< expr, expr
        EndParmValue,           ///< —
        EndFunctionParms,       ///< —
        Self,                   ///< —
        Skip,                   ///< u16, expr
        Context,                ///< expr, u16 skip, [obj], type, expr
        ArrayElement,           ///< expr (index), expr (array)
        VirtualFunction,        ///< [u8 super], name, params…
        FinalFunction,          ///< obj, params…
        IntConst,               ///< i32
        FloatConst,             ///< f32
        StringConst,            ///< ANSI string
        ObjectConst,            ///< obj
        NameConst,              ///< name
        RotationConst,          ///< i32 ×3
        VectorConst,            ///< f32 ×3
        ByteConst,              ///< u8
        IntZero,                ///< —
        IntOne,                 ///< —
        True,                   ///< —
        False,                  ///< —
        NativeParameter,        ///< obj
        NoObject,               ///< —
        IntConstByte,           ///< u8
        BoolVariable,           ///< expr
        DynamicCast,            ///< obj, expr
        Iterator,               ///< expr, u16 end
        IteratorPop,            ///< —
        IteratorNext,           ///< —
        StructCmpEq,            ///< obj, expr, expr
        StructCmpNe,            ///< obj, expr, expr
        UnicodeStringConst,     ///< UTF-16 string
        StructMember,           ///< obj, [obj], [u8], [u8], expr
        DynamicArrayLength,     ///< expr
        GlobalFunction,         ///< name, params…
        PrimitiveCast,          ///< u8 cast, expr
        DynamicArrayInsert,     ///< expr, expr, expr
        ReturnNothing,          ///< [obj]
        DelegateCmpEq,          ///< params…
        DelegateCmpNe,          ///< params…
        DelegateFunctionCmpEq,  ///< params…
        DelegateFunctionCmpNe,  ///< params…
        EmptyDelegate,          ///< —
        DynamicArrayRemove,     ///< expr, expr, expr
        DebugInfo,              ///< i32 version, i32 line, i32 pos, u8 op
        DelegateFunction,       ///< [u8 local], obj, name, params…
        DelegateProperty,       ///< name, [obj]
        LetDelegate,            ///< expr, expr
        Conditional,            ///< expr, u16, expr, u16, expr
        DynamicArrayFind,       ///< expr, [u16], expr
        DynamicArrayFindStruct, ///< expr, [u16], expr, expr
        OutVariable,            ///< obj
        DefaultParameter,       ///< u16 size, expr…, EndParmValue
        EmptyParm,              ///< —
        InstanceDelegate,       ///< name
        Unresolved,             ///< — (game-specific opcode of unknown meaning, e.g. Mirror's Edge 0x4F)
        UndefinedVariable,      ///< —
        InterfaceContext,       ///< expr
        InterfaceCast,          ///< obj, expr
        EndOfScript,            ///< —
        DynamicArrayAdd,        ///< expr, expr, EndFunctionParms
        DynamicArrayAddItem,    ///< expr, [u16], expr
        DynamicArrayRemoveItem, ///< expr, [u16], expr
        DynamicArrayInsertItem, ///< expr, [u16], expr, expr
        DynamicArrayIterator,   ///< expr, expr, u8, expr, u16 end
        DynamicArraySort,       ///< expr, [u16], expr
        FilterEditorOnly,       ///< u16 target
        NativeFunction,         ///< params… (opcode >= 0x60)
    };

    /// @brief Stable name of @p kind (e.g. "VirtualFunction").
    [[nodiscard]] std::string_view to_string(TokenKind kind) noexcept;

    /// @brief Parses a token name (case-sensitive). Returns TokenKind::Bad for unknown names.
    [[nodiscard]] TokenKind parse_token_kind(std::string_view name) noexcept;

    /// @brief Mapping from the 0x00–0x5F opcodes to token kinds; 0x60+ are native function calls.
    struct TokenMap
    {
        std::array<TokenKind, 0x60> kinds{}; ///< Kind per opcode.
        std::uint8_t extended_native = 0x60; ///< First extended-native opcode (index = ((op - 0x60) << 8) | next).
        std::uint8_t first_native = 0x70;    ///< First single-byte native opcode.

        /// @brief The UE3 default map for @p file_version (upstream `DefaultEngineBranch.BuildTokenMap` with
        ///        `UnshiftTokens3`), with @p profile's bytecode_overrides applied when given.
        /// @throws UnsupportedError for file versions < 178 (pre-UE3 maps are not implemented).
        [[nodiscard]] static TokenMap ue3(std::uint16_t file_version, const GameProfile* profile = nullptr);
    };

    /// @brief One label of a LabelTable token.
    struct LabelEntry
    {
        FName name;              ///< Label name.
        std::int32_t offset = 0; ///< Code offset.
    };

    /// @brief One decoded token. Operands that a kind does not use keep their defaults.
    struct Token
    {
        TokenKind kind = TokenKind::Bad;   ///< Kind.
        std::uint8_t opcode = 0;           ///< First byte.
        std::uint16_t native_index = 0;    ///< Native function index (NativeFunction).
        std::uint32_t offset = 0;          ///< Storage offset within the script.
        std::uint32_t size = 0;            ///< Storage size including children.
        std::uint32_t memory_offset = 0;   ///< In-memory offset (the unit of jump targets and ScriptSize).
        ObjectIndex object;                ///< First object operand.
        ObjectIndex object2;               ///< Second object operand (StructMember's struct, Context's property).
        FName name;                        ///< Name operand; LabelTable: its "None" terminator.
        std::int32_t int_value = 0;        ///< IntConst; DebugInfo line; LabelTable: its "None" terminator's offset.
        std::array<std::int32_t, 3> ints{}; ///< RotationConst; DebugInfo (version, line, text pos).
        std::array<float, 3> floats{};     ///< FloatConst (floats[0]); VectorConst.
        std::string string_value;          ///< StringConst / UnicodeStringConst (UTF-8).
        std::uint16_t word = 0;            ///< Jump target, skip size, Assert line, Context skip, Case next, size.
        std::uint16_t word2 = 0;           ///< Context/Switch property type; Conditional second skip.
        std::uint8_t byte_value = 0;       ///< ByteConst, IntConstByte, cast opcode, flags (super/local/debug/…).
        std::uint8_t byte_value2 = 0;      ///< StructMember IsModification.
        std::vector<LabelEntry> labels;    ///< LabelTable entries (without the terminating "None").
        std::vector<std::uint32_t> children; ///< Indices of operand tokens in Script::tokens, in stream order.
    };

    /// @brief A decoded script.
    struct Script
    {
        std::vector<Token> tokens;            ///< All tokens; children follow their parent.
        std::vector<std::uint32_t> statements; ///< Indices of top-level tokens, in order.
        std::size_t storage_size = 0;          ///< Bytes consumed.
        std::size_t memory_size = 0;           ///< In-memory size (equals the serialized ScriptSize).
    };

    /// @brief Options for decode_script().
    struct BytecodeOptions
    {
        const TokenMap* token_map = nullptr; ///< Token map (required).
        std::int32_t name_count = 0;         ///< Size of the name table (name operands are range-checked).
        std::int32_t none_index = -1;        ///< Name-table index of "None" (terminates LabelTable tokens).
        std::size_t max_depth = 256;         ///< Maximum expression nesting.
        std::size_t max_tokens = 1u << 20;   ///< Maximum token count.
    };

    /// @brief Decodes @p memory_size bytes (in-memory units, i.e. the struct's ScriptSize) of bytecode.
    ///
    /// Object references occupy 4 bytes in memory before version 587 and 8 from then on; names 8 bytes from
    /// version 343. The serialized size can therefore differ from @p memory_size; Script::storage_size reports it.
    /// @throws SerializationError on bad opcodes, malformed operands, or when the token stream does not end exactly
    ///         at @p memory_size.
    [[nodiscard]] Script decode_script(BinaryReader& reader, std::size_t memory_size, const BytecodeOptions& options);

    /// @brief Writes @p script back as bytecode (the inverse of decode_script()), in @p writer's file version.
    ///
    /// Operands are written as they are, jump targets and skip sizes included: a script decoded and encoded unchanged
    /// comes out byte-identical. Terminators the decoder consumes without keeping as children (EndFunctionParms,
    /// EndParmValue, the LabelTable's "None" entry) are written from the token kinds; a DebugInfo after a call's
    /// operands is its last child. Opcodes come from Token::opcode when @p token_map agrees with Token::kind, else
    /// from the map (so a new token only needs its kind).
    /// @return The in-memory size of what was written (the ScriptSize unit).
    /// @throws Error (InvalidArgument) for a token missing operands, a kind the map has no opcode for, or text that
    ///         doesn't fit the string constant's encoding.
    std::size_t encode_script(BinaryWriter& writer, const Script& script, const TokenMap& token_map);
}
