#pragma once
/// @file
/// @brief UnrealScript compiler: function and state bodies to bytecode, the inverse of the Decompiler.
///
/// Pipeline: source ──parse──▶ syntax tree ──bind + generate──▶ token tree (Script) ──assemble_script()──▶ bytecode.
///
/// The binder resolves names against the linked packages: parameters and locals of the owner, properties and
/// functions of its class (and state) and their supers, operators by their operand types among every operator the
/// game's script declares. Object operands are indices of the owner's package (an export, or the import that refers to
/// the object), so a compiled body can be encoded into that package as it is.
///
/// Scope: the statements and expressions the decompiler prints, which is the language of function and state bodies.
/// Declarations (class, variables, function signatures) are read from the package, not compiled.
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Linker/Linker.hpp>

namespace UnrealLib::Compiler
{
    /// @brief Result of compiling one body.
    struct CompiledScript
    {
        Script script;                     ///< Token tree (jumps by Token::target), for assemble_script()
        std::vector<std::string> errors;   ///< "line 12: unknown identifier 'Foo'"; empty when it compiled
        [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
    };

    /// @brief Compiles function and state bodies against linked packages.
    ///
    /// Reuse one instance for many bodies: it caches what it reads about classes, functions, properties and
    /// operators. Not thread-safe.
    class ScriptCompiler
    {
    public:
        /// @param linker  Resolves references across packages.
        /// @param scripts The game's script packages (Core, Engine, ...): where operators and native functions are
        ///                declared.
        ScriptCompiler(Linker& linker, std::span<const Package* const> scripts);
        ~ScriptCompiler();
        ScriptCompiler(const ScriptCompiler&) = delete;
        ScriptCompiler& operator=(const ScriptCompiler&) = delete;

        /// @brief Compiles @p source, the body of @p owner (a Function or State export), to a token tree.
        /// @param default_values The default value of each optional parameter of @p owner, in order, as source ("" for
        ///        none): DecompiledScript::default_values.
        [[nodiscard]] CompiledScript compile_body(std::string_view source, ObjectRef owner,
                                                  std::span<const std::string> default_values = {});

        struct Impl;    ///< The model of the game's classes (opaque)

    private:
        std::unique_ptr<Impl> m_impl;
    };
}
