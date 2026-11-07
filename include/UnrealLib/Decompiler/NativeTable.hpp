#pragma once

/// @file
/// @brief Native function table: how a native call (opcode >= 0x60, or an extended native index) is printed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#include <UnrealLib/Linker/Linker.hpp>

namespace UnrealLib::Decompiler
{
    /// @brief How a function call is written.
    enum class CallSyntax : std::uint8_t
    {
        Function,     ///< `Name(a, b)`
        Operator,     ///< `a op b` (infix)
        PreOperator,  ///< `op a`
        PostOperator, ///< `a op`
    };

    /// @brief One native function.
    struct NativeFunction
    {
        std::string name;                 ///< Name as written in source (friendly name: "+", "Normal", ...).
        CallSyntax syntax = CallSyntax::Function; ///< Call syntax.
        std::uint8_t precedence = 0;      ///< Operator precedence (larger binds looser, e.g. `*` 16, `+` 20).
    };

    /// @brief Classifies a function from its flags and precedence (upstream UFunction.IsPre/IsPost/IsOperator, with
    ///        PreOperator checked first so that a zero-precedence pre-operator is not taken for a post-operator).
    [[nodiscard]] CallSyntax call_syntax(std::uint32_t function_flags, std::uint8_t precedence) noexcept;

    /// @brief Native index -> NativeFunction.
    ///
    /// UELib reads this from a separate `.NTL` file; UnrealLib builds it from the native UFunction exports of the
    /// game's own script packages (iNative, FunctionFlags, OperPrecedence, FriendlyName), so no external table is
    /// needed and it always matches the game.
    class NativeTable
    {
    public:
        /// @brief Adds or replaces the entry for @p index.
        void add(std::uint16_t index, NativeFunction function);

        /// @brief The entry for @p index, or nullptr.
        [[nodiscard]] const NativeFunction* find(std::uint16_t index) const;

        /// @brief Number of entries.
        [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }

        /// @brief Collects every native function with a native index from @p packages (usually the script packages
        ///        Core, Engine, ...). Exports that cannot be read are skipped (reported to @p diagnostics).
        [[nodiscard]] static NativeTable from_packages(Linker& linker, std::span<const Package* const> packages,
                                                      Diagnostics* diagnostics = nullptr);

    private:
        std::unordered_map<std::uint16_t, NativeFunction> m_entries;
    };
}
