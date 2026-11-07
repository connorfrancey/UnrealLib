#include "Printer.hpp"

#include <cctype>
#include <cmath>
#include <utility>

#include <fmt/format.h>

#include <UnrealLib/Names/NameTable.hpp>

namespace UnrealLib::Decompiler
{
    namespace
    {
        constexpr std::uint16_t default_case = 0xFFFF;

        /// Upstream CastToken: cast opcode -> type name. InterfaceToObject/ObjectToInterface are implicit.
        std::string_view cast_name(std::uint8_t code)
        {
            switch (code)
            {
            case 0x37: return "string";  // InterfaceToString
            case 0x38: return "bool";    // InterfaceToBool
            case 0x39: return "Vector";  // RotatorToVector
            case 0x3A: return "int";     // ByteToInt
            case 0x3B: return "bool";    // ByteToBool
            case 0x3C: return "float";   // ByteToFloat
            case 0x3D: return "byte";    // IntToByte
            case 0x3E: return "bool";    // IntToBool
            case 0x3F: return "float";   // IntToFloat
            case 0x40: return "byte";    // BoolToByte
            case 0x41: return "int";     // BoolToInt
            case 0x42: return "float";   // BoolToFloat
            case 0x43: return "byte";    // FloatToByte
            case 0x44: return "int";     // FloatToInt
            case 0x45: return "bool";    // FloatToBool
            case 0x47: return "bool";    // ObjectToBool
            case 0x48: return "bool";    // NameToBool
            case 0x49: return "byte";    // StringToByte
            case 0x4A: return "int";     // StringToInt
            case 0x4B: return "bool";    // StringToBool
            case 0x4C: return "float";   // StringToFloat
            case 0x4D: return "Vector";  // StringToVector
            case 0x4E: return "Rotator"; // StringToRotator
            case 0x4F: return "bool";    // VectorToBool
            case 0x50: return "Rotator"; // VectorToRotator
            case 0x51: return "bool";    // RotatorToBool
            case 0x52: return "string";  // ByteToString
            case 0x53: return "string";  // IntToString
            case 0x54: return "string";  // BoolToString
            case 0x55: return "string";  // FloatToString
            case 0x56: return "string";  // ObjectToString
            case 0x57: return "string";  // NameToString
            case 0x58: return "string";  // VectorToString
            case 0x59: return "string";  // RotatorToString
            case 0x5A: return "string";  // DelegateToString
            case 0x60: return "name";    // StringToName
            default: return {};
            }
        }

        bool is_call(TokenKind kind)
        {
            switch (kind)
            {
            case TokenKind::VirtualFunction:
            case TokenKind::FinalFunction:
            case TokenKind::GlobalFunction:
            case TokenKind::NativeFunction:
            case TokenKind::DelegateFunction:
                return true;
            default:
                return false;
            }
        }
    }

    Printer::Printer(ScriptDecompiler& decompiler, const Script& script, ObjectRef owner, DecompiledScript& out)
        : m_decompiler(decompiler), m_script(script), m_owner(owner), m_package(*owner.package), m_out(out),
          m_owner_name(owner.package->object_name(owner.index))
    {
    }

    std::vector<std::uint32_t> Printer::operands(const Token& t) const
    {
        std::vector<std::uint32_t> result;
        result.reserve(t.children.size());
        for (const std::uint32_t child : t.children)
        {
            if (tok(child).kind != TokenKind::DebugInfo)
            {
                result.push_back(child);
            }
        }
        return result;
    }

    std::string Printer::object_name(ObjectIndex index) const
    {
        if (index.is_null())
        {
            return "none";
        }
        return m_package.is_valid(index) ? m_package.object_name(index) : fmt::format("__object_{}__", index.value);
    }

    std::string Printer::name_text(const FName& name) const
    {
        return m_package.names().try_to_string(name).value_or(fmt::format("__name_{}__", name.index));
    }

    std::string Printer::object_literal(ObjectIndex index) const
    {
        if (index.is_null() || !m_package.is_valid(index))
        {
            return "none";
        }
        const std::string path = index.is_export() ? m_package.name() + "." + m_package.object_path(index)
                                                   : m_package.object_path(index);
        return fmt::format("{}'{}'", m_package.class_name(index), path);
    }

    std::string Printer::placeholder(std::string_view what, const Token& t)
    {
        ++m_out.placeholders;
        m_out.diagnostics.warning("decompiler.placeholder",
                                  fmt::format("{}: {} (token {} at memory offset 0x{:X})", m_owner_name, what,
                                              to_string(t.kind), t.memory_offset));
        return fmt::format("__{}__", what);
    }

    std::string Printer::parenthesize(const Expr& e, bool needed)
    {
        return needed ? "(" + e.text + ")" : e.text;
    }

    std::string Printer::text(std::uint32_t token)
    {
        return token == no_token ? std::string() : expr(token).text;
    }

    std::string Printer::arguments(const std::vector<std::uint32_t>& args)
    {
        std::vector<std::string> texts;
        texts.reserve(args.size());
        for (const std::uint32_t a : args)
        {
            texts.push_back(text(a)); // EmptyParm (a skipped optional argument) prints as nothing
        }
        while (!texts.empty() && texts.back().empty())
        {
            texts.pop_back();
        }
        std::string result;
        for (std::size_t i = 0; i < texts.size(); ++i)
        {
            if (i != 0)
            {
                result += texts[i].empty() ? "," : ", ";
            }
            result += texts[i];
        }
        return result;
    }

    Printer::Expr Printer::call(std::string name, const std::vector<std::uint32_t>& args)
    {
        return {std::move(name) + "(" + arguments(args) + ")", 0};
    }

    Printer::Expr Printer::operator_call(CallSyntax syntax, const std::string& name, std::uint8_t precedence,
                                         const std::vector<std::uint32_t>& args)
    {
        const bool word = !name.empty() && std::isalpha(static_cast<unsigned char>(name[0])) != 0;
        switch (syntax)
        {
        case CallSyntax::Operator:
            if (args.size() == 2)
            {
                const Expr left = expr(args[0]);
                const Expr right = expr(args[1]);
                // Left-associative: an equally binding right operand needs parentheses, a left one does not.
                return {parenthesize(left, left.precedence > precedence) + " " + name + " " +
                            parenthesize(right, right.precedence != 0 && right.precedence >= precedence),
                        precedence};
            }
            break;
        case CallSyntax::PreOperator:
        case CallSyntax::PostOperator:
            if (args.size() == 1)
            {
                const Expr operand = expr(args[0]);
                bool needed = operand.precedence > unary;
                if (!word && syntax == CallSyntax::PreOperator && !operand.text.empty() &&
                    std::string_view("-+!~").find(operand.text.front()) != std::string_view::npos)
                {
                    needed = true; // `-(-a)`, never `--a`
                }
                const std::string inner = parenthesize(operand, needed);
                const std::string space = word ? " " : "";
                return {syntax == CallSyntax::PreOperator ? name + space + inner : inner + space + name, unary};
            }
            break;
        case CallSyntax::Function:
            break;
        }
        return call(name, args);
    }

    Printer::Expr Printer::native_call(const Token& t)
    {
        const NativeFunction* native = m_decompiler.natives().find(t.native_index);
        if (native == nullptr)
        {
            ++m_out.unknown_natives;
            m_out.diagnostics.warning("decompiler.unknown-native",
                                      fmt::format("{}: native function {} is not in the native table", m_owner_name,
                                                  t.native_index));
            return call(fmt::format("__NFUN_{}__", t.native_index), operands(t));
        }
        return operator_call(native->syntax, native->name, native->precedence, operands(t));
    }

    Printer::Expr Printer::final_call(const Token& t, bool as_member)
    {
        const auto args = operands(t);
        const ScriptDecompiler::FunctionInfo* info = m_decompiler.function_info(ObjectRef{&m_package, t.object});
        if (info == nullptr)
        {
            m_out.diagnostics.info("decompiler.unresolved-function",
                                   fmt::format("{}: could not resolve called function '{}'", m_owner_name,
                                               object_name(t.object)));
            return call(object_name(t.object), args);
        }
        if (info->syntax != CallSyntax::Function)
        {
            return operator_call(info->syntax, info->name, info->precedence, args);
        }
        // A non-virtual call to a function of the same name as the one being decompiled is a super call (unless it
        // is a call through a context, or a recursive call to itself).
        const bool recursive = info->target.package == m_owner.package && info->target.index == m_owner.index;
        if (!as_member && !recursive && info->name == m_owner_name)
        {
            const std::string super = m_decompiler.super_of_outer(m_owner);
            const std::string prefix = info->outer_name == super ? "super." : "super(" + info->outer_name + ").";
            return call(prefix + info->name, args);
        }
        return call(info->name, args);
    }

    Printer::Expr Printer::method(const Token& t, std::string_view name)
    {
        const auto ops = operands(t);
        if (ops.empty())
        {
            return {placeholder("array_method", t), 0};
        }
        const Expr array = expr(ops[0]);
        const std::vector<std::uint32_t> args(ops.begin() + 1, ops.end());
        return {parenthesize(array, array.precedence != 0) + "." + std::string(name) + "(" + arguments(args) + ")", 0};
    }

    Printer::Expr Printer::primitive_cast(const Token& t)
    {
        const auto ops = operands(t);
        const std::string value = ops.empty() ? std::string() : text(ops[0]);
        if (t.byte_value == 0x36 || t.byte_value == 0x46) // InterfaceToObject, ObjectToInterface: implicit
        {
            return {value, ops.empty() ? std::uint8_t{0} : expr(ops[0]).precedence};
        }
        const std::string_view name = cast_name(t.byte_value);
        if (name.empty())
        {
            return {placeholder(fmt::format("CAST_0x{:02X}", t.byte_value), t) + "(" + value + ")", 0};
        }
        return {std::string(name) + "(" + value + ")", 0};
    }

    Printer::Expr Printer::new_object(const Token& t)
    {
        // new [(outer [, name [, flags]])] class [(template)]
        const auto ops = operands(t);
        std::vector<std::string> parts;
        for (const std::uint32_t op : ops)
        {
            parts.push_back(text(op));
        }
        parts.resize(5);
        std::string inner;
        for (std::size_t i = 0; i < 3; ++i)
        {
            if (!parts[i].empty())
            {
                inner += (inner.empty() ? "" : ", ") + parts[i];
            }
        }
        std::string result = "new";
        if (!inner.empty())
        {
            result += " (" + inner + ")";
        }
        if (!parts[3].empty())
        {
            result += " " + parts[3];
        }
        if (!parts[4].empty())
        {
            result += " (" + parts[4] + ")";
        }
        return {result, 0};
    }

    Printer::Expr Printer::expr(std::uint32_t token, bool as_member)
    {
        const Token& t = tok(token);
        const auto ops = [&]() { return operands(t); };
        const auto operand = [&](std::size_t n) -> std::uint32_t {
            const auto all = operands(t);
            return n < all.size() ? all[n] : no_token;
        };

        switch (t.kind)
        {
        // Variables
        case TokenKind::LocalVariable:
        case TokenKind::InstanceVariable:
        case TokenKind::StateVariable:
        case TokenKind::OutVariable:
            return {object_name(t.object), 0};
        case TokenKind::NativeParameter:
            return {}; // The parameter list of a native function's script: no source (as upstream release builds).
        case TokenKind::DefaultVariable:
            return {"default." + object_name(t.object), 0};
        case TokenKind::DelegateProperty:
        case TokenKind::InstanceDelegate:
            return {name_text(t.name), 0};
        case TokenKind::Self:
            return {"self", 0};

        // Constants
        case TokenKind::IntConst:
            return {std::to_string(t.int_value), t.int_value < 0 ? unary : std::uint8_t{0}};
        case TokenKind::FloatConst:
            if (!std::isfinite(t.floats[0]))
            {
                return {placeholder("non_finite_float", t), 0};
            }
            return {format_float(t.floats[0]), t.floats[0] < 0 ? unary : std::uint8_t{0}};
        case TokenKind::StringConst:
        case TokenKind::UnicodeStringConst:
            return {quote_string(t.string_value), 0};
        case TokenKind::NameConst:
            return {"'" + name_text(t.name) + "'", 0};
        case TokenKind::ObjectConst:
            return {object_literal(t.object), 0};
        case TokenKind::RotationConst:
            return {fmt::format("rot({}, {}, {})", t.ints[0], t.ints[1], t.ints[2]), 0};
        case TokenKind::VectorConst:
            for (const float f : t.floats)
            {
                if (!std::isfinite(f))
                {
                    return {placeholder("non_finite_float", t), 0};
                }
            }
            return {fmt::format("vect({}, {}, {})", format_float(t.floats[0]), format_float(t.floats[1]),
                                format_float(t.floats[2])),
                    0};
        case TokenKind::ByteConst:
        case TokenKind::IntConstByte:
            return {std::to_string(t.byte_value), 0};
        case TokenKind::IntZero:
            return {"0", 0};
        case TokenKind::IntOne:
            return {"1", 0};
        case TokenKind::True:
            return {"true", 0};
        case TokenKind::False:
            return {"false", 0};
        case TokenKind::NoObject:
        case TokenKind::EmptyDelegate:
            return {"none", 0};

        // Tokens that print nothing
        case TokenKind::Nothing:
        case TokenKind::EmptyParm:
        case TokenKind::UndefinedVariable:
        case TokenKind::DebugInfo:
        case TokenKind::EatReturnValue:
            return {};
        case TokenKind::ReturnNothing:
            return {t.object.is_null() ? std::string() : object_name(t.object), 0};

        // Wrappers
        case TokenKind::BoolVariable:
        case TokenKind::Skip:
        case TokenKind::InterfaceContext:
        case TokenKind::DefaultParameter:
            return operand(0) == no_token ? Expr{} : expr(operand(0), as_member);

        // Assignment and access
        case TokenKind::Let:
        case TokenKind::LetBool:
        case TokenKind::LetDelegate:
        {
            const auto o = ops();
            if (o.size() != 2)
            {
                return {placeholder("let", t), 0};
            }
            return {text(o[0]) + " = " + text(o[1]), assignment};
        }
        case TokenKind::ArrayElement:
        case TokenKind::DynamicArrayElement:
        {
            const auto o = ops();
            if (o.size() != 2)
            {
                return {placeholder("array_element", t), 0};
            }
            const Expr array = expr(o[1]);
            return {parenthesize(array, array.precedence != 0) + "[" + text(o[0]) + "]", 0};
        }
        case TokenKind::Context:
        case TokenKind::ClassContext:
        {
            const auto o = ops();
            if (o.size() != 2)
            {
                return {placeholder("context", t), 0};
            }
            const Expr object = expr(o[0]);
            std::string member = expr(o[1], true).text;
            if (t.kind == TokenKind::ClassContext && is_call(tok(o[1]).kind))
            {
                member = "static." + member;
            }
            return {parenthesize(object, object.precedence != 0) + "." + member, 0};
        }
        case TokenKind::StructMember:
        {
            if (operand(0) == no_token)
            {
                return {placeholder("struct_member", t), 0};
            }
            const Expr structure = expr(operand(0));
            return {parenthesize(structure, structure.precedence != 0) + "." + object_name(t.object), 0};
        }
        case TokenKind::DynamicArrayLength:
        {
            if (operand(0) == no_token)
            {
                return {placeholder("array_length", t), 0};
            }
            const Expr array = expr(operand(0));
            return {parenthesize(array, array.precedence != 0) + ".Length", 0};
        }

        // Calls
        case TokenKind::VirtualFunction:
        case TokenKind::DelegateFunction:
            return call(name_text(t.name), ops());
        case TokenKind::GlobalFunction:
            return call("global." + name_text(t.name), ops());
        case TokenKind::FinalFunction:
            return final_call(t, as_member);
        case TokenKind::NativeFunction:
            return native_call(t);

        // Casts
        case TokenKind::PrimitiveCast:
            return primitive_cast(t);
        case TokenKind::DynamicCast:
        case TokenKind::InterfaceCast:
            return {object_name(t.object) + "(" + text(operand(0)) + ")", 0};
        case TokenKind::MetaClassCast:
            return {"class<" + object_name(t.object) + ">(" + text(operand(0)) + ")", 0};

        // Comparisons
        case TokenKind::StructCmpEq:
        case TokenKind::StructCmpNe:
        case TokenKind::DelegateCmpEq:
        case TokenKind::DelegateCmpNe:
        case TokenKind::DelegateFunctionCmpEq:
        case TokenKind::DelegateFunctionCmpNe:
        {
            const auto o = ops();
            if (o.size() != 2)
            {
                return {placeholder("comparison", t), 0};
            }
            const bool equal = t.kind == TokenKind::StructCmpEq || t.kind == TokenKind::DelegateCmpEq ||
                               t.kind == TokenKind::DelegateFunctionCmpEq;
            const Expr left = expr(o[0]);
            const Expr right = expr(o[1]);
            return {parenthesize(left, left.precedence > comparison) + (equal ? " == " : " != ") +
                        parenthesize(right, right.precedence >= comparison),
                    comparison};
        }
        case TokenKind::Conditional:
        {
            const auto o = ops();
            if (o.size() != 3)
            {
                return {placeholder("conditional", t), 0};
            }
            const Expr c = expr(o[0]);
            const Expr a = expr(o[1]);
            const Expr b = expr(o[2]);
            return {parenthesize(c, c.precedence >= ternary) + " ? " + parenthesize(a, a.precedence >= ternary) +
                        " : " + parenthesize(b, b.precedence > ternary),
                    ternary};
        }

        // Dynamic array methods
        case TokenKind::DynamicArrayFind:
        case TokenKind::DynamicArrayFindStruct:
            return method(t, "Find");
        case TokenKind::DynamicArrayInsert:
            return method(t, "Insert");
        case TokenKind::DynamicArrayRemove:
            return method(t, "Remove");
        case TokenKind::DynamicArrayAdd:
            return method(t, "Add");
        case TokenKind::DynamicArrayAddItem:
            return method(t, "AddItem");
        case TokenKind::DynamicArrayRemoveItem:
            return method(t, "RemoveItem");
        case TokenKind::DynamicArrayInsertItem:
            return method(t, "InsertItem");
        case TokenKind::DynamicArraySort:
            return method(t, "Sort");

        case TokenKind::New:
            return new_object(t);

        // Statement-level tokens that can stand as an expression statement
        case TokenKind::Assert:
            return {"assert(" + text(operand(0)) + ")", 0};
        case TokenKind::GotoLabel:
            return {"goto " + text(operand(0)), 0};

        default:
            return {placeholder(fmt::format("unexpected_{}", to_string(t.kind)), t), 0};
        }
    }

    void Printer::line(std::string& out, int depth, std::string_view text) const
    {
        for (int i = 0; i < depth; ++i)
        {
            out += m_decompiler.m_options.indent;
        }
        out += text;
        out += '\n';
    }

    void Printer::braced(const std::vector<Statement>& body, int depth, std::string& out)
    {
        line(out, depth, "{");
        print(body, depth + 1, out);
        line(out, depth, "}");
    }

    std::string Printer::foreach_header(const Token& t)
    {
        const auto ops = operands(t);
        if (t.kind == TokenKind::Iterator)
        {
            return "foreach " + (ops.empty() ? placeholder("iterator", t) : text(ops[0]));
        }
        // DynamicArrayIterator: array, item, index (used when byte_value != 0)
        if (ops.size() != 3)
        {
            return "foreach " + placeholder("array_iterator", t);
        }
        const Expr array = expr(ops[0]);
        std::string header = "foreach " + parenthesize(array, array.precedence != 0) + "(" + text(ops[1]);
        if (t.byte_value != 0)
        {
            header += ", " + text(ops[2]);
        }
        return header + ")";
    }

    void Printer::statement(const Statement& s, int depth, std::string& out)
    {
        switch (s.kind)
        {
        case StatementKind::Expression:
        {
            const std::string code = text(s.token);
            if (!code.empty())
            {
                line(out, depth, code + ";");
            }
            break;
        }
        case StatementKind::Return:
        {
            const auto ops = operands(tok(s.token));
            const std::string value = tok(s.token).kind == TokenKind::Return && !ops.empty() ? text(ops[0]) : "";
            line(out, depth, value.empty() ? "return;" : "return " + value + ";");
            break;
        }
        case StatementKind::Stop:
            line(out, depth, "stop;");
            break;
        case StatementKind::If:
        {
            line(out, depth, "if (" + text(s.condition) + ")");
            braced(s.body, depth, out);
            const Statement* chained = &s;
            while (chained->has_else)
            {
                const auto& else_body = chained->else_body;
                if (else_body.size() == 1 && else_body.front().kind == StatementKind::If)
                {
                    chained = &else_body.front();
                    line(out, depth, "else if (" + text(chained->condition) + ")");
                    braced(chained->body, depth, out);
                    continue;
                }
                line(out, depth, "else");
                braced(else_body, depth, out);
                break;
            }
            break;
        }
        case StatementKind::IfNotGoto:
            line(out, depth, "if (!(" + text(s.condition) + ")) goto " + jump_label(s.target) + ";");
            break;
        case StatementKind::While:
            line(out, depth, s.condition == no_token ? std::string("while (true)") : "while (" + text(s.condition) + ")");
            braced(s.body, depth, out);
            break;
        case StatementKind::For:
            line(out, depth, "for (" + text(s.init) + "; " + text(s.condition) + "; " + text(s.increment) + ")");
            braced(s.body, depth, out);
            break;
        case StatementKind::DoUntil:
            line(out, depth, "do");
            braced(s.body, depth, out);
            line(out, depth, "until (" + text(s.condition) + ");");
            break;
        case StatementKind::ForEach:
            line(out, depth, foreach_header(tok(s.token)));
            braced(s.body, depth, out);
            break;
        case StatementKind::Switch:
        {
            const auto ops = operands(tok(s.token));
            line(out, depth, "switch (" + (ops.empty() ? placeholder("switch", tok(s.token)) : text(ops[0])) + ")");
            line(out, depth, "{");
            for (const Statement& c : s.body)
            {
                statement(c, depth + 1, out);
            }
            line(out, depth, "}");
            break;
        }
        case StatementKind::Case:
        {
            const Token& t = tok(s.token);
            const auto ops = operands(t);
            line(out, depth, t.word == default_case ? std::string("default:")
                                                     : "case " + (ops.empty() ? placeholder("case", t) : text(ops[0])) + ":");
            print(s.body, depth + 1, out);
            break;
        }
        case StatementKind::Break:
            line(out, depth, "break;");
            break;
        case StatementKind::Continue:
            line(out, depth, "continue;");
            break;
        case StatementKind::Goto:
            line(out, depth, "goto " + jump_label(s.target) + ";");
            break;
        case StatementKind::Label:
            line(out, depth > 0 ? depth - 1 : 0, s.text + ":");
            break;
        case StatementKind::FilterEditorOnly:
            line(out, depth, "filtereditoronly");
            braced(s.body, depth, out);
            break;
        case StatementKind::Comment:
            line(out, depth, "// " + s.text);
            break;
        }
    }

    void Printer::print(const std::vector<Statement>& statements, int depth, std::string& out)
    {
        for (const Statement& s : statements)
        {
            statement(s, depth, out);
        }
    }
}
