#pragma once

/// @file
/// @brief UnrealScript decompiler: functions and states back to UnrealScript-style source.
///
/// Pipeline: bytecode (UnrealLib::decode_script) -> token tree -> statement tree (structure(), Statements.hpp) ->
/// source text (this file). Each stage is usable on its own; only the last one produces text.
///
/// Output conventions and the known differences from upstream UELib are listed in docs/decompiler.md.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Core/Diagnostics.hpp>
#include <UnrealLib/Decompiler/NativeTable.hpp>
#include <UnrealLib/Decompiler/Statements.hpp>
#include <UnrealLib/Linker/Linker.hpp>

namespace UnrealLib::Decompiler
{
    /// @brief Result of decompiling one function or state.
    struct DecompiledScript
    {
        std::string source;             ///< Declaration and body (or only the body, for decompile_body()).
        StructuredScript structure;     ///< Statement tree the body was printed from.
        std::vector<std::string> default_values; ///< Default values of the optional parameters, in order.
        std::size_t unknown_natives = 0; ///< Native calls missing from the NativeTable (printed as `__NFUN_<n>__`).
        std::size_t placeholders = 0;   ///< Other placeholders (`__...__`): unknown casts, unexpected tokens.
        Diagnostics diagnostics;        ///< Details about placeholders and unresolved references.

        /// @brief Whether the output is free of placeholders, unstructured jumps and structure problems.
        [[nodiscard]] bool clean() const noexcept
        {
            return unknown_natives == 0 && placeholders == 0 && structure.gotos == 0 && structure.problems == 0;
        }
    };

    /// @brief Formatting options.
    struct DecompileOptions
    {
        std::string indent = "    ";    ///< One indentation level.
    };

    /// @brief Decompiles functions and states of linked packages.
    ///
    /// Holds the native table and caches what it learns about called functions (operator syntax, precedence, outer
    /// class), so one instance should be reused for many scripts. Not thread-safe.
    class ScriptDecompiler
    {
    public:
        /// @param linker  Resolves references across packages (callees, property types, supers).
        /// @param natives Native function table (NativeTable::from_packages()).
        /// @param options Output options.
        ScriptDecompiler(Linker& linker, NativeTable natives, DecompileOptions options = {});

        /// @brief Decompiles a Function or State export: declaration, locals and body.
        /// @throws Error (InvalidArgument) if @p object is not a function or state; SerializationError on bad data.
        [[nodiscard]] DecompiledScript decompile(ObjectRef object);

        /// @brief Decompiles a Class export to a complete `.uc` source: declaration, constants, enums, structs,
        ///        variables, replication, functions, states and `defaultproperties` (with subobject blocks).
        ///
        /// The counters of the result add up those of every function and state; `placeholders` also counts default
        /// property values that could not be decoded.
        /// @throws Error (InvalidArgument) if @p object is not a class; SerializationError on bad data.
        [[nodiscard]] DecompiledScript decompile_class(ObjectRef object);

        /// @brief Type information for `defaultproperties` and `structdefaultproperties` (normally a ReflectionSchema
        ///        over the game's script packages). Without one, script-declared structs and arrays print as raw.
        void set_schema(const PropertySchema* schema) noexcept { m_schema = schema; }

        /// @brief Decompiles the statements of @p script, which belongs to @p owner (a function or state export).
        ///        The source holds the body only, at indentation @p depth.
        [[nodiscard]] DecompiledScript decompile_body(const Script& script, ObjectRef owner,
                                                      const StructureOptions& structure_options = {},
                                                      int depth = 0);

        /// @brief The UnrealScript type of property export @p property ("int", "array<Actor>", "class<Pawn>",
        ///        ...). Unreadable types give "__unknown_type__".
        [[nodiscard]] std::string type_name(ObjectRef property);

        /// @brief Native function table in use.
        [[nodiscard]] const NativeTable& natives() const noexcept { return m_natives; }

        /// @brief What a call to a script function needs to know about it.
        struct FunctionInfo
        {
            std::string name;           ///< Object name (FriendlyName for operators).
            CallSyntax syntax = CallSyntax::Function; ///< Call syntax.
            std::uint8_t precedence = 0; ///< Operator precedence.
            ObjectRef target;           ///< The function export.
            std::string outer_name;     ///< Name of the class (or state) declaring it.
        };

        /// @brief Resolves function reference @p ref (import or export) and describes it (cached).
        [[nodiscard]] const FunctionInfo* function_info(ObjectRef ref);

    private:
        friend class Printer;

        /// @brief Super class (or super state) name of the struct that declares @p function ("" if none).
        [[nodiscard]] std::string super_of_outer(ObjectRef function);

        // Class-level output (ClassDecompiler.cpp).
        [[nodiscard]] std::vector<ObjectRef> children(ObjectRef owner);
        [[nodiscard]] std::string variable_declaration(ObjectRef property, std::string_view owner_name);
        [[nodiscard]] std::string struct_source(ObjectRef structure, int depth, DecompiledScript& totals);
        [[nodiscard]] std::string enum_source(ObjectRef enumeration, int depth);
        [[nodiscard]] std::string const_source(ObjectRef constant, int depth);
        [[nodiscard]] std::string class_header(ObjectRef object, const FieldObject& field);
        [[nodiscard]] std::string replication(ObjectRef object, const FieldObject& field,
                                              const std::vector<ObjectRef>& properties, DecompiledScript& totals);
        [[nodiscard]] std::string properties_block(ObjectRef object, std::string_view keyword, int depth,
                                                   DecompiledScript& totals);
        void property_lines(ObjectRef object, int depth, std::string& out, DecompiledScript& totals, int nesting = 0);
        void format_properties(const Package& package, const std::vector<Property>& properties,
                               const std::set<std::int32_t>& subobjects, int depth, std::string& out,
                               DecompiledScript& totals);
        [[nodiscard]] std::string indentation(int depth) const;
        static void accumulate(DecompiledScript& totals, const DecompiledScript& part);

        const PropertySchema* m_schema = nullptr;

        Linker& m_linker;
        NativeTable m_natives;
        DecompileOptions m_options;
        std::map<std::pair<const Package*, std::int32_t>, std::optional<FunctionInfo>> m_functions;
        std::map<std::pair<const Package*, std::int32_t>, std::string> m_types;
        std::map<std::pair<const Package*, std::int32_t>, std::string> m_supers;
    };

    /// @brief Formats a float the way UnrealLib's decompiler prints float constants: the shortest fixed-point
    ///        text that reads back to the same value, always with a decimal point ("1.0", "0.25", "-3.5").
    [[nodiscard]] std::string format_float(float value);

    /// @brief Quotes and escapes @p text as an UnrealScript string literal (`"`, `\`, newline, carriage return).
    [[nodiscard]] std::string quote_string(std::string_view text);
}
