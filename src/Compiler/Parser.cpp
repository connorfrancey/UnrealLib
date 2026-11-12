#include "Parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <vector>

namespace UnrealLib::Compiler
{
    namespace
    {
        // ---- Lexer --------------------------------------------------------------------------------------------------
        enum class Tok : std::uint8_t { Ident, Int, Float, String, Name, ObjectLit, Punct, End };

        struct Lexeme
        {
            Tok kind = Tok::End;
            std::string text;     ///< Identifier, punctuation, string/name contents, number text, object path
            std::string klass;    ///< ObjectLit: its class
            int line = 1;
            bool space_before = false;
        };

        struct Failure
        {
            int line;
            std::string message;
        };

        bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
        bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

        std::vector<Lexeme> lex(std::string_view s, const OperatorSet& ops)
        {
            // Punctuation, longest first: the structural ones and every symbolic operator the game declares
            std::vector<std::string> puncts = {"(", ")", "[", "]", "{", "}", ",", ";", ".", "?", ":", "=", "<", ">"};
            const auto add = [&](const std::string& op) {
                if (!op.empty() && !ident_start(op[0])) puncts.push_back(op);
            };
            for (const auto& [op, p] : ops.binary) add(op);
            for (const auto& op : ops.prefix) add(op);
            for (const auto& op : ops.postfix) add(op);
            std::sort(puncts.begin(), puncts.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
            puncts.erase(std::unique(puncts.begin(), puncts.end()), puncts.end());

            std::vector<Lexeme> out;
            int line = 1;
            bool space = false;
            std::size_t i = 0;
            while (i < s.size())
            {
                const char c = s[i];
                if (c == '\n') { ++line; ++i; space = true; continue; }
                if (std::isspace(static_cast<unsigned char>(c))) { ++i; space = true; continue; }
                if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
                {
                    while (i < s.size() && s[i] != '\n') ++i;
                    continue;
                }
                Lexeme l;
                l.line = line;
                l.space_before = space;
                space = false;
                if (ident_start(c))
                {
                    std::size_t j = i;
                    while (j < s.size() && ident_char(s[j])) ++j;
                    l.text = std::string(s.substr(i, j - i));
                    i = j;
                    if (i < s.size() && s[i] == '\'')    // Class'Package.Path'
                    {
                        const std::size_t end = s.find('\'', i + 1);
                        if (end == std::string_view::npos) throw Failure{line, "unterminated object literal"};
                        l.kind = Tok::ObjectLit;
                        l.klass = l.text;
                        l.text = std::string(s.substr(i + 1, end - i - 1));
                        i = end + 1;
                    }
                    else
                    {
                        l.kind = Tok::Ident;
                    }
                    out.push_back(std::move(l));
                    continue;
                }
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    std::size_t j = i;
                    bool dot = false;
                    while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) ||
                                            (s[j] == '.' && !dot && j + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[j + 1])))))
                    {
                        dot = dot || s[j] == '.';
                        ++j;
                    }
                    if (j < s.size() && (s[j] == 'e' || s[j] == 'E'))
                    {
                        std::size_t k = j + 1;
                        if (k < s.size() && (s[k] == '-' || s[k] == '+')) ++k;
                        if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k])))
                        {
                            while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
                            j = k;
                            dot = true;
                        }
                    }
                    l.kind = dot ? Tok::Float : Tok::Int;
                    l.text = std::string(s.substr(i, j - i));
                    i = j;
                    out.push_back(std::move(l));
                    continue;
                }
                if (c == '"')
                {
                    std::string text;
                    std::size_t j = i + 1;
                    for (; j < s.size() && s[j] != '"'; ++j)
                    {
                        if (s[j] == '\\' && j + 1 < s.size())
                        {
                            ++j;
                            text += s[j] == 'n' ? '\n' : s[j] == 'r' ? '\r' : s[j];
                        }
                        else
                        {
                            if (s[j] == '\n') ++line;
                            text += s[j];
                        }
                    }
                    if (j >= s.size()) throw Failure{l.line, "unterminated string"};
                    l.kind = Tok::String;
                    l.text = std::move(text);
                    i = j + 1;
                    out.push_back(std::move(l));
                    continue;
                }
                if (c == '\'')
                {
                    const std::size_t end = s.find('\'', i + 1);
                    if (end == std::string_view::npos) throw Failure{line, "unterminated name"};
                    l.kind = Tok::Name;
                    l.text = std::string(s.substr(i + 1, end - i - 1));
                    i = end + 1;
                    out.push_back(std::move(l));
                    continue;
                }
                bool matched = false;
                for (const std::string& p : puncts)
                    if (s.substr(i, p.size()) == p)
                    {
                        l.kind = Tok::Punct;
                        l.text = p;
                        i += p.size();
                        matched = true;
                        break;
                    }
                if (!matched) throw Failure{line, std::string("unexpected character '") + c + "'"};
                out.push_back(std::move(l));
            }
            Lexeme end;
            end.line = line;
            out.push_back(end);
            return out;
        }

        // ---- Parser -------------------------------------------------------------------------------------------------
        constexpr int kTernary = 50;
        constexpr int kAssign = 60;

        class Parser
        {
        public:
            Parser(std::vector<Lexeme> tokens, const OperatorSet& ops) : m_t(std::move(tokens)), m_ops(ops) {}

            Block body()
            {
                Block out;
                while (!at_end()) out.push_back(statement());
                return out;
            }

        private:
            const Lexeme& peek(std::size_t n = 0) const { return m_t[std::min(m_pos + n, m_t.size() - 1)]; }
            bool at_end() const { return peek().kind == Tok::End; }
            const Lexeme& next() { return m_t[std::min(m_pos++, m_t.size() - 1)]; }
            bool is(std::string_view text, std::size_t n = 0) const
            {
                const Lexeme& l = peek(n);
                return (l.kind == Tok::Punct || l.kind == Tok::Ident) && l.text == text;
            }
            bool is_word(std::string_view word, std::size_t n = 0) const
            {
                const Lexeme& l = peek(n);
                if (l.kind != Tok::Ident || l.text.size() != word.size()) return false;
                for (std::size_t i = 0; i < word.size(); ++i)
                    if (std::tolower(static_cast<unsigned char>(l.text[i])) != word[i]) return false;
                return true;
            }
            bool accept(std::string_view text)
            {
                if (!is(text)) return false;
                ++m_pos;
                return true;
            }
            bool accept_word(std::string_view word)
            {
                if (!is_word(word)) return false;
                ++m_pos;
                return true;
            }
            void expect(std::string_view text)
            {
                if (!accept(text)) fail("expected '" + std::string(text) + "', found '" + peek().text + "'");
            }
            [[noreturn]] void fail(const std::string& message) const { throw Failure{peek().line, message}; }

            ExprPtr make(ExprKind kind, int line)
            {
                auto e = std::make_unique<Expr>();
                e->kind = kind;
                e->line = line;
                return e;
            }

            // ---- Statements ---------------------------------------------------------------------------------------
            Block braced()
            {
                expect("{");
                Block out;
                while (!is("}"))
                {
                    if (at_end()) fail("expected '}'");
                    out.push_back(statement());
                }
                expect("}");
                return out;
            }

            ExprPtr paren_expr()
            {
                expect("(");
                ExprPtr e = expression();
                expect(")");
                return e;
            }

            StmtPtr statement()
            {
                auto s = std::make_unique<Stmt>();
                s->line = peek().line;
                if (peek().kind == Tok::Ident && is(":", 1) && !is_word("default"))   // A label
                {
                    s->kind = StmtKind::Label;
                    s->label = next().text;
                    next();
                    return s;
                }
                if (accept_word("if"))
                {
                    s->kind = StmtKind::If;
                    s->expr = paren_expr();
                    if (is_word("goto"))                  // if (!(c)) goto L;
                    {
                        auto g = std::make_unique<Stmt>();
                        g->line = peek().line;
                        next();
                        g->kind = StmtKind::Goto;
                        g->label = next().text;
                        expect(";");
                        s->body.push_back(std::move(g));
                        return s;
                    }
                    s->body = braced();
                    if (accept_word("else"))
                    {
                        s->has_else = true;
                        if (is_word("if")) s->else_body.push_back(statement());
                        else s->else_body = braced();
                    }
                    return s;
                }
                if (accept_word("while"))
                {
                    s->kind = StmtKind::While;
                    expect("(");
                    if (!(is_word("true") && is(")", 1))) s->expr = expression();
                    else next();
                    expect(")");
                    s->body = braced();
                    return s;
                }
                if (accept_word("do"))
                {
                    s->kind = StmtKind::DoUntil;
                    s->body = braced();
                    if (!accept_word("until")) fail("expected 'until'");
                    s->expr = paren_expr();
                    expect(";");
                    return s;
                }
                if (accept_word("for"))
                {
                    s->kind = StmtKind::For;
                    expect("(");
                    if (!is(";")) s->init = expression();
                    expect(";");
                    if (!is(";")) s->expr = expression();
                    expect(";");
                    if (!is(")")) s->increment = expression();
                    expect(")");
                    s->body = braced();
                    return s;
                }
                if (accept_word("foreach"))
                {
                    s->kind = StmtKind::ForEach;
                    s->expr = postfix_expr();     // F(args), or Array(item[, index]) parsed as a call on the array
                    s->body = braced();
                    return s;
                }
                if (accept_word("switch"))
                {
                    s->kind = StmtKind::Switch;
                    s->expr = paren_expr();
                    expect("{");
                    while (!is("}"))
                    {
                        if (at_end()) fail("expected '}'");
                        auto c = std::make_unique<Stmt>();
                        c->line = peek().line;
                        if (accept_word("case"))
                        {
                            c->kind = StmtKind::Case;
                            c->expr = expression();
                        }
                        else if (accept_word("default"))
                        {
                            c->kind = StmtKind::Default;
                        }
                        else
                        {
                            fail("expected 'case' or 'default'");
                        }
                        expect(":");
                        while (!is_word("case") && !(is_word("default") && is(":", 1)) && !is("}"))
                        {
                            if (at_end()) fail("expected '}'");
                            c->body.push_back(statement());
                        }
                        s->body.push_back(std::move(c));
                    }
                    expect("}");
                    return s;
                }
                if (accept_word("return"))
                {
                    s->kind = StmtKind::Return;
                    if (!is(";")) s->expr = expression();
                    expect(";");
                    return s;
                }
                if (accept_word("break")) { s->kind = StmtKind::Break; expect(";"); return s; }
                if (accept_word("continue")) { s->kind = StmtKind::Continue; expect(";"); return s; }
                if (is_word("stop") && is(";", 1))
                {
                    next();
                    next();
                    s->kind = StmtKind::Stop;
                    return s;
                }
                if (is_word("goto") && peek(1).kind == Tok::Ident && is(";", 2))
                {
                    next();
                    s->kind = StmtKind::Goto;
                    s->label = next().text;
                    expect(";");
                    return s;
                }
                if (is_word("goto"))                       // goto 'Label' (state code): an expression
                {
                    next();
                    s->kind = StmtKind::Goto;
                    s->expr = expression();
                    expect(";");
                    return s;
                }
                if (is_word("assert") && is("(", 1))
                {
                    next();
                    s->kind = StmtKind::Assert;
                    s->expr = paren_expr();
                    expect(";");
                    return s;
                }
                if (accept_word("filtereditoronly"))
                {
                    s->kind = StmtKind::FilterEditorOnly;
                    s->body = braced();
                    return s;
                }
                s->kind = StmtKind::Expression;
                s->expr = expression();
                expect(";");
                return s;
            }

            // ---- Expressions --------------------------------------------------------------------------------------
            ExprPtr expression() { return assignment(); }

            ExprPtr assignment()
            {
                ExprPtr left = conditional();
                if (is("="))
                {
                    const int line = next().line;
                    auto e = make(ExprKind::Assign, line);
                    e->left = std::move(left);
                    e->right = assignment();
                    return e;
                }
                return left;
            }

            ExprPtr conditional()
            {
                ExprPtr c = binary(kTernary - 1);
                if (is("?"))
                {
                    const int line = next().line;
                    auto e = make(ExprKind::Conditional, line);
                    e->args.push_back(std::move(c));
                    e->args.push_back(binary(kTernary - 1));
                    expect(":");
                    e->args.push_back(conditional());
                    return e;
                }
                return c;
            }

            // The binary operator at the cursor and its precedence (0: none)
            int binary_here(std::string& op) const
            {
                const Lexeme& l = peek();
                if (l.kind != Tok::Punct && l.kind != Tok::Ident) return 0;
                const auto it = m_ops.binary.find(l.text);
                if (it == m_ops.binary.end()) return 0;
                op = l.text;
                return it->second;
            }

            // Left-associative precedence climbing; `limit`: the loosest operator allowed here
            ExprPtr binary(int limit)
            {
                ExprPtr left = unary();
                for (;;)
                {
                    std::string op;
                    const int p = binary_here(op);
                    if (p == 0 || p > limit) return left;
                    const int line = next().line;
                    ExprPtr right = binary(p - 1);
                    auto e = make(ExprKind::Binary, line);
                    e->text = op;
                    e->left = std::move(left);
                    e->right = std::move(right);
                    left = std::move(e);
                }
            }

            ExprPtr unary()
            {
                const Lexeme& l = peek();
                if ((l.kind == Tok::Punct || l.kind == Tok::Ident) && m_ops.prefix.count(l.text) &&
                    !(l.kind == Tok::Ident && (is("(", 1) || is(".", 1) || is(")", 1) || is(";", 1) || is(",", 1))))
                {
                    const std::string op = l.text;
                    const int line = next().line;
                    // A literal with its sign is a constant (IntConst / FloatConst), as the printer wrote it
                    if (op == "-" && (peek().kind == Tok::Int || peek().kind == Tok::Float) && !peek().space_before)
                    {
                        ExprPtr lit = primary();
                        if (lit->kind == ExprKind::Int) lit->int_value = -lit->int_value;
                        else lit->floats[0] = -lit->floats[0];
                        return postfix_ops(std::move(lit));
                    }
                    auto e = make(ExprKind::Unary, line);
                    e->text = op;
                    e->left = unary();
                    return e;
                }
                return postfix_ops(postfix_expr());
            }

            ExprPtr postfix_ops(ExprPtr e)
            {
                while ((peek().kind == Tok::Punct || peek().kind == Tok::Ident) && m_ops.postfix.count(peek().text) &&
                       !m_ops.binary.count(peek().text))
                {
                    auto u = make(ExprKind::Unary, peek().line);
                    u->text = next().text;
                    u->postfix = true;
                    u->left = std::move(e);
                    e = std::move(u);
                }
                return e;
            }

            void arguments(Expr& call)
            {
                expect("(");
                if (accept(")")) return;
                for (;;)
                {
                    if (is(",") || is(")")) call.args.push_back(make(ExprKind::Empty, peek().line));
                    else call.args.push_back(expression());
                    if (accept(")")) return;
                    expect(",");
                }
            }

            ExprPtr postfix_expr()
            {
                ExprPtr e = primary();
                for (;;)
                {
                    if (is("("))
                    {
                        auto c = make(ExprKind::Call, peek().line);
                        c->left = std::move(e);
                        arguments(*c);
                        e = std::move(c);
                    }
                    else if (is("["))
                    {
                        const int line = next().line;
                        auto x = make(ExprKind::Index, line);
                        x->left = std::move(e);
                        x->args.push_back(expression());
                        expect("]");
                        e = std::move(x);
                    }
                    else if (is(".") && (peek(1).kind == Tok::Int || peek(1).kind == Tok::Float))
                    {
                        const int line = next().line;
                        auto m = make(ExprKind::Member, line);
                        m->left = std::move(e);
                        m->right = primary();
                        e = std::move(m);
                    }
                    else if (is(".") && peek(1).kind == Tok::Ident)
                    {
                        const int line = next().line;
                        if (is_word("static") && is(".", 1))
                        {
                            next();
                            next();
                            auto m = make(ExprKind::StaticMember, line);
                            m->left = std::move(e);
                            m->text = next().text;
                            e = std::move(m);
                            continue;
                        }
                        auto m = make(ExprKind::Member, line);
                        m->left = std::move(e);
                        m->text = next().text;
                        e = std::move(m);
                    }
                    else
                    {
                        return e;
                    }
                }
            }

            ExprPtr primary()
            {
                const Lexeme& l = peek();
                const int line = l.line;
                switch (l.kind)
                {
                case Tok::Int:
                {
                    auto e = make(ExprKind::Int, line);
                    const std::string text = next().text;
                    std::from_chars(text.data(), text.data() + text.size(), e->int_value);
                    return e;
                }
                case Tok::Float:
                {
                    auto e = make(ExprKind::Float, line);
                    e->floats[0] = std::stof(next().text);
                    return e;
                }
                case Tok::String:
                {
                    auto e = make(ExprKind::String, line);
                    e->text = next().text;
                    return e;
                }
                case Tok::Name:
                {
                    auto e = make(ExprKind::Name, line);
                    e->text = next().text;
                    return e;
                }
                case Tok::ObjectLit:
                {
                    auto e = make(ExprKind::Object, line);
                    e->type_name = l.klass;
                    e->text = next().text;
                    return e;
                }
                case Tok::Punct:
                    if (l.text == "(")
                    {
                        next();
                        ExprPtr e = expression();
                        expect(")");
                        return e;
                    }
                    fail("unexpected '" + l.text + "'");
                case Tok::Ident:
                    break;
                case Tok::End:
                    fail("unexpected end");
                }

                // Words with a meaning of their own
                if (accept_word("true")) return make(ExprKind::True, line);
                if (accept_word("false")) return make(ExprKind::False, line);
                if (accept_word("none")) return make(ExprKind::None, line);
                if (accept_word("self")) return make(ExprKind::Self, line);
                if (is_word("vect") && is("(", 1)) return vector_literal(ExprKind::Vector);
                if (is_word("rot") && is("(", 1)) return vector_literal(ExprKind::Rotator);
                if (is_word("default") && is(".", 1))
                {
                    next();
                    next();
                    auto e = make(ExprKind::Default, line);
                    e->text = next().text;
                    return e;
                }
                if (is_word("super") && (is(".", 1) || is("(", 1)))
                {
                    next();
                    auto e = make(ExprKind::Super, line);
                    if (accept("("))
                    {
                        e->type_name = next().text;
                        expect(")");
                    }
                    expect(".");
                    e->text = next().text;
                    return e;
                }
                if (is_word("global") && is(".", 1))
                {
                    next();
                    next();
                    auto e = make(ExprKind::Global, line);
                    e->text = next().text;
                    return e;
                }
                if (is_word("class") && is("<", 1))
                {
                    next();
                    next();
                    auto e = make(ExprKind::MetaCast, line);
                    e->type_name = next().text;
                    expect(">");
                    expect("(");
                    e->left = expression();
                    expect(")");
                    return e;
                }
                if (is_word("new")) return new_expr();
                auto e = make(ExprKind::Identifier, line);
                e->text = next().text;
                return e;
            }

            // new [(outer[, name[, flags]])] Class [(template)]
            ExprPtr new_expr()
            {
                const int line = next().line;
                auto e = make(ExprKind::New, line);
                e->args.resize(5);
                if (accept("("))
                {
                    for (std::size_t i = 0; i < 3 && !is(")"); ++i)
                    {
                        e->args[i] = expression();
                        if (!accept(",")) break;
                    }
                    expect(")");
                }
                e->args[3] = postfix_new_class();
                if (accept("("))
                {
                    e->args[4] = expression();
                    expect(")");
                }
                return e;
            }

            // The class operand of `new`: a primary with members, but not a call (its parenthesis is the template)
            ExprPtr postfix_new_class()
            {
                ExprPtr e = primary();
                while (is(".") && peek(1).kind == Tok::Ident)
                {
                    const int line = next().line;
                    auto m = make(ExprKind::Member, line);
                    m->left = std::move(e);
                    m->text = next().text;
                    e = std::move(m);
                }
                while (is("["))
                {
                    const int line = next().line;
                    auto x = make(ExprKind::Index, line);
                    x->left = std::move(e);
                    x->args.push_back(expression());
                    expect("]");
                    e = std::move(x);
                }
                return e;
            }

            ExprPtr vector_literal(ExprKind kind)
            {
                const int line = next().line;
                auto e = make(kind, line);
                expect("(");
                for (int i = 0; i < 3; ++i)
                {
                    bool negative = accept("-");
                    const Lexeme& n = next();
                    if (n.kind != Tok::Int && n.kind != Tok::Float) fail("expected a number in vect/rot");
                    if (kind == ExprKind::Vector) e->floats[i] = std::stof(n.text) * (negative ? -1.f : 1.f);
                    else e->ints[i] = std::stoi(n.text) * (negative ? -1 : 1);
                    if (i < 2) expect(",");
                }
                expect(")");
                return e;
            }

            std::vector<Lexeme> m_t;
            std::size_t m_pos = 0;
            const OperatorSet& m_ops;
        };
    }

    bool parse_body(std::string_view source, const OperatorSet& operators, Block& out, ParseError& error)
    {
        try
        {
            Parser parser(lex(source, operators), operators);
            out = parser.body();
            return true;
        }
        catch (const Failure& f)
        {
            error = {f.line, f.message};
            return false;
        }
        catch (const std::exception& e)
        {
            error = {0, e.what()};
            return false;
        }
    }
}
