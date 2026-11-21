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
#include <functional>
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

    /// @brief What a body needs that its package doesn't have yet, for a package being built: imports and names.
    struct PackageEditor
    {
        std::function<ObjectIndex(ObjectRef)> reference;   ///< The package's index for an object (adds an import)
        std::function<FName(std::string_view)> name;        ///< The FName for a text (adds it to the name table)
    };

    /// @brief A script package built from UnrealScript source (ScriptCompiler::build_class_package()).
    struct BuiltPackage
    {
        std::vector<std::byte> bytes;      ///< The package file
        std::vector<std::string> errors;   ///< "line 12: ..."; empty when it built
        std::vector<std::string> warnings;
        [[nodiscard]] bool ok() const noexcept { return errors.empty() && !bytes.empty(); }
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
                                                  std::span<const std::string> default_values = {},
                                                  const PackageEditor* editor = nullptr);

        /// @brief Builds a script package named @p package_name holding the class @p source declares (a `.uc` file:
        ///        `class X extends Y ...;`, variables, functions and events, `defaultproperties`), laid out like the
        ///        packages the game loads: the class, its properties and functions (bodies compiled), its default
        ///        object. The parent and every type it uses must be in the linked packages.
        [[nodiscard]] BuiltPackage build_class_package(std::string_view package_name, std::string_view source);

        struct Impl;    ///< The model of the game's classes (opaque)

    private:
        std::unique_ptr<Impl> m_impl;
    };
}
