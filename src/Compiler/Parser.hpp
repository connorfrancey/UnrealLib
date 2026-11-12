#pragma once
/// @file
/// @brief Lexer and parser for UnrealScript function and state bodies.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include "Ast.hpp"

namespace UnrealLib::Compiler
{
    /// @brief The operators the game declares (Object.uc and friends): their text and how they are written.
    ///        Precedence: a larger number binds looser (`*` 16, `+` 20, `==` 24, `&&` 30, `||` 32).
    struct OperatorSet
    {
        std::map<std::string, std::uint8_t, std::less<>> binary;
        std::set<std::string, std::less<>> prefix;
        std::set<std::string, std::less<>> postfix;
    };

    /// @brief A parse failure: the line and what was expected.
    struct ParseError
    {
        int line = 0;
        std::string message;
    };

    /// @brief Parses a body (statements and labels). Returns false and fills @p error on the first syntax error.
    bool parse_body(std::string_view source, const OperatorSet& operators, Block& out, ParseError& error);
}
