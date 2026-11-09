#pragma once

/// @file
/// @brief Statement IR of the decompiler and the control-flow structuring pass.
///
/// structure() turns the flat statement list of a decoded Script into a tree of statements (if/else, while,
/// do-until, foreach, switch, break/continue, labels and gotos) by matching the jump patterns the UnrealScript compiler
/// emits. It works on memory offsets (the unit of jump targets) and knows nothing about text: printing is
/// separate (Decompiler.hpp). Anything that does not match a pattern is kept as an explicit goto and counted, never
/// dropped.
///
/// Patterns (UE3 compiler output, as also assumed by upstream UELib's ByteCodeDecompiler):
///   if (c) A            JumpIfNot(c) -> E; A; E:
///   if (c) A else B     JumpIfNot(c) -> L; A; Jump -> E; L: B; E:
///   while (c) A         T: JumpIfNot(c) -> E; A; Jump -> T; E:
///   for (i; c; n) A     i; T: JumpIfNot(c) -> E; A; C: n; Jump -> T; E:  (recognised when a `continue` jumps to
///                                                                         C; otherwise printed as a while loop)
///   while (true) A      T: A; Jump -> T; E:
///   do A until (c)      T: A; JumpIfNot(c) -> T
///   foreach F(...) A    Iterator(F) -> E; A; IteratorNext; IteratorPop; E:   (E before or after IteratorPop)
///   switch (x)          Switch(x); Case(v) -> next; A; Jump -> E (break); ...; Case(default); B; E:
///   break/continue      Jump to the innermost loop's (or switch's) end / continue point

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <UnrealLib/Bytecode/Bytecode.hpp>

namespace UnrealLib
{
    class NameTable;
}

namespace UnrealLib::Decompiler
{
    /// @brief Kind of a structured statement.
    enum class StatementKind : std::uint8_t
    {
        Expression,     ///< `token;` (calls, assignments, assert, goto 'Label', ...).
        Return,         ///< `return [expr];` (`token` is the Return token).
        Stop,           ///< `stop;` (state code).
        If,             ///< `if (condition) body [else else_body]`.
        IfNotGoto,      ///< `if (!(condition)) goto target;` (a conditional jump that matched no pattern).
        While,          ///< `while (condition) body`; `condition == none` for `while (true)`.
        For,            ///< `for (init; condition; increment) body` (`init` may be none).
        DoUntil,        ///< `do body until (condition);`.
        ForEach,        ///< `foreach iterator body` (`token` is Iterator or DynamicArrayIterator).
        Switch,         ///< `switch (expr)`; `body` holds Case statements.
        Case,           ///< `case value:` / `default:` (`token` is the Case token) with its `body`.
        Break,          ///< `break;`.
        Continue,       ///< `continue;`.
        Goto,           ///< `goto target;` (an unconditional jump that matched no pattern).
        Label,          ///< `name:` (`text`), from the LabelTable or a goto target.
        FilterEditorOnly, ///< `filtereditoronly body`.
        Comment,        ///< `// text` (structure problems, unresolved tokens).
    };

    inline constexpr std::uint32_t no_token = 0xFFFFFFFFu; ///< "No token" marker.

    /// @brief One structured statement.
    struct Statement
    {
        StatementKind kind = StatementKind::Expression; ///< Kind.
        std::uint32_t token = no_token;      ///< Main token (index into Script::tokens), see StatementKind.
        std::uint32_t condition = no_token;  ///< Condition expression token (If, IfNotGoto, While, For, DoUntil).
        std::uint32_t init = no_token;       ///< For: initialization statement token (or none).
        std::uint32_t increment = no_token;  ///< For: increment statement token.
        std::uint32_t offset = 0;            ///< Memory offset of the statement's first token.
        std::uint32_t target = 0;            ///< Jump target (Goto, IfNotGoto).
        std::string text;                    ///< Label name (Label), comment text (Comment).
        std::vector<Statement> body;         ///< Nested statements (If, While, DoUntil, ForEach, Switch, Case, ...).
        std::vector<Statement> else_body;    ///< `else` branch (If).
        bool has_else = false;               ///< Whether `else_body` is printed (it may be empty).
    };

    /// @brief Inputs of structure().
    struct StructureOptions
    {
        /// Named labels (state code LabelTable): memory offset -> label name.
        std::vector<LabelEntry> labels;
        const NameTable* names = nullptr;    ///< Resolves label names (required when `labels` is not empty).
        bool drop_trailing_return = true;    ///< Omit the compiler-generated `return;` ending a function.
    };

    /// @brief Result of structure().
    struct StructuredScript
    {
        std::vector<Statement> statements;   ///< Top-level statements.
        std::vector<std::uint32_t> default_parameters; ///< DefaultParameter tokens of the prologue, in order.
        std::size_t gotos = 0;               ///< Goto/IfNotGoto statements emitted (unstructured jumps).
        std::size_t problems = 0;            ///< Structure problems (comments, goto targets without a label).
        std::size_t unresolved = 0;          ///< Unresolved (game-specific, unknown) tokens kept as comments.
    };

    /// @brief Builds the statement tree of @p script.
    [[nodiscard]] StructuredScript structure(const Script& script, const StructureOptions& options = {});

    /// @brief Name of a generated label for @p offset ("J0x1A"), as used for goto targets.
    [[nodiscard]] std::string jump_label(std::uint32_t offset);
}
