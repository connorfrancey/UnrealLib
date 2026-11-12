#pragma once
/// @file
/// @brief Syntax tree of UnrealScript function and state bodies (the statement and expression grammar the decompiler
///        prints, which is the language's own).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace UnrealLib::Compiler
{
    struct Expr;
    using ExprPtr = std::unique_ptr<Expr>;

    enum class ExprKind : std::uint8_t
    {
        Int,          ///< 12, -3 (sign folded only for literals the printer wrote with one)
        Float,        ///< 1.0
        String,       ///< "text"
        Name,         ///< 'Name'
        Object,       ///< Class'Package.Path'
        Vector,       ///< vect(x, y, z)
        Rotator,      ///< rot(p, y, r)
        True,
        False,
        None,         ///< none
        Self,         ///< self
        Identifier,   ///< text
        Default,      ///< default.text
        Member,       ///< a.text (a is `left`)
        StaticMember, ///< a.static.F(...)
        Super,        ///< super.F(...) / super(Class).F(...): text = function, type_name = class ("" for plain super)
        Global,       ///< global.F(...)
        Call,         ///< left(args): left is Identifier / Member / Super / Global / StaticMember / Default
        Index,        ///< left[args[0]]
        Unary,        ///< op left (prefix) or left op (postfix: postfix == true)
        Binary,       ///< left op right
        Assign,       ///< left = right
        Conditional,  ///< args[0] ? args[1] : args[2]
        New,          ///< new (args[0], args[1], args[2]) args[3] (args[4]); absent ones are null
        MetaCast,     ///< class<type_name>(left)
        Empty,        ///< A skipped optional argument
    };

    struct Expr
    {
        ExprKind kind = ExprKind::Empty;
        std::string text;            ///< Identifier, member or operator text; String/Name/Object literal contents
        std::string type_name;       ///< Object literal class, MetaCast class, Super class
        std::int64_t int_value = 0;
        float floats[3]{};
        std::int32_t ints[3]{};
        bool postfix = false;
        ExprPtr left, right;
        std::vector<ExprPtr> args;
        int line = 0;
    };

    struct Stmt;
    using StmtPtr = std::unique_ptr<Stmt>;
    using Block = std::vector<StmtPtr>;

    enum class StmtKind : std::uint8_t
    {
        Expression, Return, If, While, DoUntil, For, ForEach, Switch, Case, Default, Break, Continue, Goto, Label,
        Stop, Assert, FilterEditorOnly,
    };

    struct Stmt
    {
        StmtKind kind = StmtKind::Expression;
        ExprPtr expr;                ///< Expression, Return value, If/While/DoUntil/For condition, ForEach iterator, Switch value, Case value, Assert
        ExprPtr init, increment;     ///< For
        ExprPtr item, index;         ///< ForEach over an array: foreach Array(item, index)
        Block body;
        Block else_body;
        bool has_else = false;
        std::string label;           ///< Goto / Label
        int line = 0;
    };
}
