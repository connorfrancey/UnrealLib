#pragma once

// Internal: prints decoded tokens as UnrealScript expressions and the statement tree as source.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <UnrealLib/Decompiler/Decompiler.hpp>

namespace UnrealLib::Decompiler
{
    class Printer
    {
    public:
        Printer(ScriptDecompiler& decompiler, const Script& script, ObjectRef owner, DecompiledScript& out);

        /// A printed expression and how loosely it binds: 0 = atomic (names, calls, literals), 1 = unary operator,
        /// otherwise the infix operator precedence (larger binds looser, as in UnrealScript's `operator(N)`).
        struct Expr
        {
            std::string text;
            std::uint8_t precedence = 0;
        };

        static constexpr std::uint8_t unary = 1;
        static constexpr std::uint8_t comparison = 24;  ///< `==`/`!=` of structs and delegates (as Object.uc).
        static constexpr std::uint8_t ternary = 50;     ///< `c ? a : b`: looser than every operator.
        static constexpr std::uint8_t assignment = 60;  ///< `a = b`.

        [[nodiscard]] Expr expr(std::uint32_t token, bool as_member = false);
        [[nodiscard]] std::string text(std::uint32_t token);

        /// Prints @p statements at indentation @p depth.
        void print(const std::vector<Statement>& statements, int depth, std::string& out);

    private:
        const Token& tok(std::uint32_t index) const { return m_script.tokens[index]; }
        std::vector<std::uint32_t> operands(const Token& t) const;
        std::string object_name(ObjectIndex index) const;
        std::string name_text(const FName& name) const;
        std::string object_literal(ObjectIndex index) const;
        std::string placeholder(std::string_view what, const Token& t);
        static std::string parenthesize(const Expr& e, bool needed);

        Expr call(std::string name, const std::vector<std::uint32_t>& args);
        std::string arguments(const std::vector<std::uint32_t>& args);
        Expr operator_call(CallSyntax syntax, const std::string& name, std::uint8_t precedence,
                           const std::vector<std::uint32_t>& args);
        Expr native_call(const Token& t);
        Expr final_call(const Token& t, bool as_member);
        Expr method(const Token& t, std::string_view name);
        Expr primitive_cast(const Token& t);
        Expr new_object(const Token& t);

        void line(std::string& out, int depth, std::string_view text) const;
        void braced(const std::vector<Statement>& body, int depth, std::string& out);
        void statement(const Statement& s, int depth, std::string& out);
        std::string foreach_header(const Token& t);

        ScriptDecompiler& m_decompiler;
        const Script& m_script;
        ObjectRef m_owner;
        const Package& m_package;
        DecompiledScript& m_out;
        std::string m_owner_name;
    };
}
