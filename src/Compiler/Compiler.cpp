#include <UnrealLib/Compiler/Compiler.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Reflection/Fields.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Model.hpp"
#include "Parser.hpp"

namespace UnrealLib::Compiler
{
    using namespace detail;

    // ==== Code generation =================================================================================================
    namespace
    {
        struct Value
        {
            std::uint32_t token = 0;
            Type type;
            bool lvalue = false;
            bool literal = false;            ///< An int literal whose token can still change to fit its use
            std::int64_t literal_value = 0;
            ObjectRef returns;               ///< A call: the callee's ReturnValue property
            const Function* callee = nullptr; ///< A call: the function called
        };

        class Generator
        {
        public:
            Generator(ScriptCompiler::Impl& m, ObjectRef owner, Script& out, const PackageEditor* editor = nullptr)
                : M(m), m_owner(owner), m_pkg(*owner.package), S(out), m_editor(editor)
            {
                const std::string k = M.class_of(owner);
                m_is_state = k == "State";
                // The class (and state) around the owner
                ObjectRef cur = owner;
                for (int guard = 0; cur.valid() && guard < 16; ++guard)
                {
                    const std::string c = M.class_of(cur);
                    if (c == "Class") { m_class = cur; break; }
                    if (c == "State" && !m_state.valid()) m_state = cur;
                    cur = M.outer_of(cur);
                }
                if (!m_is_state) m_function = M.function(owner);
            }

            // One statement per optional parameter: its default value (DefaultParameter) or Nothing
            void prologue(std::span<const std::string> defaults)
            {
                if (!m_function) return;
                if (m_function->flags & FunctionFlag::Native)
                {
                    m_native = true;
                    for (const Param& p : m_function->params)
                    {
                        Token t;
                        t.kind = TokenKind::NativeParameter;
                        t.object = reference(p.property);
                        emit_statement(add(t));
                    }
                    emit_statement(nothing());
                    return;
                }
                std::size_t optional = 0;
                for (const Param& p : m_function->params)
                {
                    if (!p.optional()) continue;
                    const std::string value = optional < defaults.size() ? defaults[optional] : std::string();
                    ++optional;
                    if (value.empty())
                    {
                        emit_statement(nothing());
                        continue;
                    }
                    Block parsed;
                    ParseError pe;
                    if (!parse_body(value + ";", M.operator_set, parsed, pe) || parsed.size() != 1 || !parsed[0]->expr)
                        fail(0, "the default value of " + p.name + " doesn't parse: " + value);
                    Token d;
                    d.kind = TokenKind::DefaultParameter;
                    d.children = {convert(expression(*parsed[0]->expr, &p.type), &p.type, 0).token};
                    emit_statement(add(d));
                }
            }

            void body(const Block& block)
            {
                if (m_native)    // A native's script is its parameter list
                {
                    Token end;
                    end.kind = TokenKind::EndOfScript;
                    emit_statement(add(end));
                    return;
                }
                for (const auto& s : block) statement(*s);
                if (m_is_state && !m_named.empty()) label_table();
                // The compiler's trailing return (functions): `return ReturnValue` reads as ReturnNothing(ReturnValue)
                const bool ends_returning = !S.statements.empty() && S.tokens[S.statements.back()].kind == TokenKind::Return &&
                                            !S.tokens[S.statements.back()].children.empty() &&
                                            S.tokens[S.tokens[S.statements.back()].children[0]].kind == TokenKind::ReturnNothing;
                if (!m_is_state && !ends_returning)
                {
                    Token r;
                    r.kind = TokenKind::Return;
                    Token n;
                    if (m_function && m_function->ret)
                    {
                        n.kind = TokenKind::ReturnNothing;
                        n.object = reference(m_function->ret->property);
                    }
                    else
                    {
                        n.kind = TokenKind::Nothing;
                    }
                    r.children = {add(n)};
                    emit_statement(add(r));
                }
                Token end;
                end.kind = TokenKind::EndOfScript;
                emit_statement(add(end));
                resolve_gotos();
            }

        private:
            ScriptCompiler::Impl& M;
            ObjectRef m_owner, m_class, m_state;
            const Package& m_pkg;
            Script& S;
            Function* m_function = nullptr;
            const PackageEditor* m_editor = nullptr;
            bool m_is_state = false;
            bool m_native = false;
            int m_const_depth = 0;                      // Consts compiled inside consts
            std::vector<std::string> m_label_order;     // State labels, as they appear

            // Jump targets: a label is bound to the next statement emitted
            std::vector<std::vector<std::uint32_t*>> m_label_users;   // Unused
            struct Label
            {
                std::int32_t token = -1;                    // The statement it is bound to
                std::vector<std::uint32_t> jumps;           // Jump tokens waiting for it
            };
            std::vector<Label> m_labels;
            std::vector<int> m_pending;                     // Labels bound to the next statement
            std::map<std::string, int, std::less<>> m_named;
            struct Loop
            {
                int brk = -1, cont = -1;
                bool foreach = false;
            };
            std::vector<Loop> m_loops;

            [[noreturn]] void fail(int line, const std::string& message) const { throw Error{line, message}; }

            std::uint32_t add(Token t)
            {
                if (t.kind != TokenKind::NativeFunction && t.opcode == 0)
                {
                    const auto& kinds = scripts_map().kinds;
                    for (std::size_t i = 0; i < kinds.size(); ++i)
                        if (kinds[i] == t.kind) { t.opcode = static_cast<std::uint8_t>(i); break; }
                }
                S.tokens.push_back(std::move(t));
                return static_cast<std::uint32_t>(S.tokens.size() - 1);
            }
            // The package's index for `d`: its export or import, or one the editor adds (a package being built)
            ObjectIndex reference(ObjectRef d)
            {
                ObjectIndex i = M.index_in(m_pkg, d);
                if (i.is_null() && m_editor && m_editor->reference) i = m_editor->reference(d);
                return i;
            }

            const TokenMap& scripts_map() const
            {
                static const TokenMap map = TokenMap::ue3(m_pkg.summary().file_version, &m_pkg.profile());
                return map;
            }

            int new_label()
            {
                m_labels.emplace_back();
                return static_cast<int>(m_labels.size() - 1);
            }
            void bind(int label) { m_pending.push_back(label); }
            void jump_to(std::uint32_t jump, int label)
            {
                Label& l = m_labels[static_cast<std::size_t>(label)];
                if (l.token >= 0) S.tokens[jump].target = l.token;
                else l.jumps.push_back(jump);
            }
            void emit_statement(std::uint32_t token)
            {
                for (const int label : m_pending)
                {
                    Label& l = m_labels[static_cast<std::size_t>(label)];
                    l.token = static_cast<std::int32_t>(token);
                    for (const std::uint32_t j : l.jumps) S.tokens[j].target = l.token;
                    l.jumps.clear();
                }
                m_pending.clear();
                S.statements.push_back(token);
            }
            void resolve_gotos()
            {
                for (const Label& l : m_labels)
                    if (!l.jumps.empty()) fail(0, "a jump to a label that marks no statement");
            }
            std::uint32_t jump(TokenKind kind, int label, std::optional<std::uint32_t> condition = std::nullopt)
            {
                Token j;
                j.kind = kind;
                if (condition) j.children = {*condition};
                const std::uint32_t t = add(j);
                jump_to(t, label);
                return t;
            }

            // ---- Statements ---------------------------------------------------------------------------------------
            void block(const Block& b)
            {
                for (const auto& s : b) statement(*s);
            }

            int named_label(const std::string& name)
            {
                auto [it, first] = m_named.try_emplace(lower(name), -1);
                if (first) it->second = new_label();
                return it->second;
            }

            // State code's labels: Nothing up to the 4-byte boundary the table's entries start at, the LabelTable
            void label_table()
            {
                std::size_t at = 0;
                {
                    Script probe = S;
                    // Jumps still waiting for their label: any target gives the same size
                    for (Token& t : probe.tokens)
                        if ((t.kind == TokenKind::Jump || t.kind == TokenKind::JumpIfNot || t.kind == TokenKind::Iterator ||
                             t.kind == TokenKind::DynamicArrayIterator || t.kind == TokenKind::FilterEditorOnly ||
                             (t.kind == TokenKind::Case && t.word != 0xFFFF)) && t.target < 0)
                            t.target = 0;
                    BinaryWriter w(m_pkg.context());
                    at = assemble_script(w, probe, scripts_map());
                }
                for (; (at + 1) % 4 != 0; ++at) emit_statement(nothing());
                Token table;
                table.kind = TokenKind::LabelTable;
                for (auto it = m_label_order.rbegin(); it != m_label_order.rend(); ++it)    // Newest first, as the UE3 compiler lists them
                {
                    const std::string& name = *it;
                    const Label& l = m_labels[static_cast<std::size_t>(m_named.at(lower(name)))];
                    LabelEntry e;
                    e.name = name_of_text(name, 0);
                    e.target = l.token;
                    table.labels.push_back(e);
                }
                table.name = name_of_text("None", 0);
                table.int_value = 0xFFFF;
                emit_statement(add(table));
            }

            void statement(const Stmt& s)
            {
                switch (s.kind)
                {
                case StmtKind::Expression:
                {
                    const Value v = expression(*s.expr, nullptr);
                    if (v.returns.valid() && (M.property_flags(v.returns) & PropertyFlag::NeedCtorLink))
                    {
                        Token eat;
                        eat.kind = TokenKind::EatReturnValue;
                        eat.object = reference(v.returns);
                        emit_statement(add(eat));
                    }
                    emit_statement(v.token);
                    break;
                }
                case StmtKind::Return:
                {
                    for (auto it = m_loops.rbegin(); it != m_loops.rend(); ++it)
                        if (it->foreach)
                        {
                            Token pop;
                            pop.kind = TokenKind::IteratorPop;
                            emit_statement(add(pop));
                        }
                    Token r;
                    r.kind = TokenKind::Return;
                    if (s.expr && s.expr->kind == ExprKind::Identifier && m_function && m_function->ret &&
                        iequal(s.expr->text, m_function->ret->name))
                    {
                        Token n;
                        n.kind = TokenKind::ReturnNothing;
                        n.object = reference(m_function->ret->property);
                        r.children = {add(n)};
                    }
                    else if (s.expr)
                    {
                        const Type* want = m_function && m_function->ret ? &m_function->ret->type : nullptr;
                        r.children = {convert(expression(*s.expr, want), want, s.line).token};
                    }
                    else
                    {
                        Token n;
                        n.kind = TokenKind::Nothing;
                        r.children = {add(n)};
                    }
                    emit_statement(add(r));
                    break;
                }
                case StmtKind::If:
                {
                    // if (!(c)) goto L;  ->  JumpIfNot(c) -> L
                    if (s.body.size() == 1 && s.body[0]->kind == StmtKind::Goto && !s.body[0]->expr && s.expr->kind == ExprKind::Unary &&
                        s.expr->text == "!")
                    {
                        const Value c = condition(*s.expr->left);
                        emit_statement(jump(TokenKind::JumpIfNot, named_label(s.body[0]->label), c.token));
                        break;
                    }
                    const Value c = condition(*s.expr);
                    const int otherwise = new_label();
                    emit_statement(jump(TokenKind::JumpIfNot, otherwise, c.token));
                    block(s.body);
                    if (s.has_else)
                    {
                        const int end = new_label();
                        emit_statement(jump(TokenKind::Jump, end));
                        bind(otherwise);
                        block(s.else_body);
                        bind(end);
                    }
                    else
                    {
                        bind(otherwise);
                    }
                    break;
                }
                case StmtKind::While:
                {
                    const int top = new_label(), end = new_label();
                    bind(top);
                    if (s.expr)
                    {
                        const Value c = condition(*s.expr);
                        emit_statement(jump(TokenKind::JumpIfNot, end, c.token));
                    }
                    m_loops.push_back({end, top, false});
                    block(s.body);
                    m_loops.pop_back();
                    emit_statement(jump(TokenKind::Jump, top));
                    bind(end);
                    break;
                }
                case StmtKind::DoUntil:
                {
                    const int top = new_label(), end = new_label(), cont = new_label();
                    bind(top);
                    m_loops.push_back({end, cont, false});
                    block(s.body);
                    m_loops.pop_back();
                    bind(cont);
                    const Value c = condition(*s.expr);
                    emit_statement(jump(TokenKind::JumpIfNot, top, c.token));
                    bind(end);
                    break;
                }
                case StmtKind::For:
                {
                    if (s.init) emit_statement(expression(*s.init, nullptr).token);
                    const int top = new_label(), end = new_label(), cont = new_label();
                    bind(top);
                    if (s.expr) emit_statement(jump(TokenKind::JumpIfNot, end, condition(*s.expr).token));
                    m_loops.push_back({end, cont, false});
                    block(s.body);
                    m_loops.pop_back();
                    bind(cont);
                    if (s.increment) emit_statement(expression(*s.increment, nullptr).token);
                    emit_statement(jump(TokenKind::Jump, top));
                    bind(end);
                    break;
                }
                case StmtKind::ForEach:
                    foreach_statement(s);
                    break;
                case StmtKind::Switch:
                    switch_statement(s);
                    break;
                case StmtKind::Break:
                case StmtKind::Continue:
                {
                    if (m_loops.empty()) fail(s.line, "break/continue outside a loop");
                    const Loop& l = m_loops.back();
                    if (l.foreach && s.kind == StmtKind::Continue)
                    {
                        Token next;
                        next.kind = TokenKind::IteratorNext;
                        emit_statement(add(next));
                        break;
                    }
                    const int target = s.kind == StmtKind::Break ? l.brk : l.cont;
                    if (target < 0) fail(s.line, "continue outside a loop");
                    emit_statement(jump(TokenKind::Jump, target));
                    break;
                }
                case StmtKind::Goto:
                {
                    if (s.expr)    // goto 'Label' (state code)
                    {
                        Token g;
                        g.kind = TokenKind::GotoLabel;
                        g.children = {expression(*s.expr, nullptr).token};
                        emit_statement(add(g));
                        break;
                    }
                    emit_statement(jump(TokenKind::Jump, named_label(s.label)));
                    break;
                }
                case StmtKind::Label:
                    m_label_order.push_back(s.label);
                    bind(named_label(s.label));
                    break;
                case StmtKind::Stop:
                {
                    Token t;
                    t.kind = TokenKind::Stop;
                    emit_statement(add(t));
                    break;
                }
                case StmtKind::Assert:
                {
                    Token a;
                    a.kind = TokenKind::Assert;
                    a.word = static_cast<std::uint16_t>(s.line);
                    a.children = {condition(*s.expr).token};
                    emit_statement(add(a));
                    break;
                }
                case StmtKind::FilterEditorOnly:
                {
                    const int end = new_label();
                    emit_statement(jump(TokenKind::FilterEditorOnly, end));
                    block(s.body);
                    bind(end);
                    break;
                }
                case StmtKind::Case:
                case StmtKind::Default:
                    fail(s.line, "case outside a switch");
                }
            }

            void foreach_statement(const Stmt& s)
            {
                const Expr& call = *s.expr;
                if (call.kind != ExprKind::Call) fail(s.line, "foreach needs an iterator call");
                const int end = new_label();
                // An array: foreach Array(Item[, Index])
                std::optional<Value> array;
                if (call.left->kind != ExprKind::Global && call.left->kind != ExprKind::Super)
                {
                    try
                    {
                        Value v = expression(*call.left, nullptr);
                        if (v.type.kind == K::Array) array = v;
                    }
                    catch (const Error&)
                    {
                    }
                }
                std::uint32_t head;
                if (array)
                {
                    Token it;
                    it.kind = TokenKind::DynamicArrayIterator;
                    const Type* inner = array->type.inner.get();
                    const Value item = call.args.size() > 0 ? expression(*call.args[0], inner) : Value{};
                    Value index;
                    if (call.args.size() > 1)
                    {
                        it.byte_value = 1;
                        index = expression(*call.args[1], nullptr);
                    }
                    else
                    {
                        Token n;
                        n.kind = TokenKind::EmptyParm;
                        index.token = add(n);
                    }
                    it.children = {array->token, item.token, index.token};
                    head = add(it);
                }
                else
                {
                    Token it;
                    it.kind = TokenKind::Iterator;
                    it.children = {expression(call, nullptr).token};
                    head = add(it);
                }
                const int pop_label = new_label();
                jump_to(head, pop_label);
                emit_statement(head);
                m_loops.push_back({pop_label, -1, true});
                block(s.body);
                m_loops.pop_back();
                Token next;
                next.kind = TokenKind::IteratorNext;
                emit_statement(add(next));
                bind(pop_label);
                Token pop;
                pop.kind = TokenKind::IteratorPop;
                const std::uint32_t pop_token = add(pop);
                emit_statement(pop_token);
                {
                    // An iterator (or an array) reached through a context: the context skips the whole loop
                    const std::uint32_t first = S.tokens[head].children.at(0);
                    if (S.tokens[first].kind == TokenKind::Context || S.tokens[first].kind == TokenKind::ClassContext)
                        S.tokens[first].skip_to = static_cast<std::int32_t>(pop_token);
                }
                (void)end;
            }

            void switch_statement(const Stmt& s)
            {
                const Value v = expression(*s.expr, nullptr);
                Token sw;
                sw.kind = TokenKind::Switch;
                const ObjectRef sprop = v.returns.valid() ? v.returns : v.type.property;
                sw.word2 = (sprop.valid() && (M.property_flags(sprop) & PropertyFlag::NeedCtorLink)) || v.type.kind == K::String
                               ? 0 : static_cast<std::uint16_t>(M.size_of(v.type));
                sw.children = {v.token};
                emit_statement(add(sw));
                const int end = new_label();
                m_loops.push_back({end, m_loops.empty() ? -1 : m_loops.back().cont, false});
                bool has_default = false;
                std::optional<std::uint32_t> previous;     // The previous Case: its "next" is this one
                for (const auto& c : s.body)
                {
                    Token cs;
                    cs.kind = TokenKind::Case;
                    if (c->kind == StmtKind::Default)
                    {
                        has_default = true;
                        cs.word = 0xFFFF;
                    }
                    else
                    {
                        cs.children = {convert(expression(*c->expr, &v.type), &v.type, c->line).token};
                    }
                    const std::uint32_t t = add(cs);
                    if (previous) S.tokens[*previous].target = static_cast<std::int32_t>(t);
                    emit_statement(t);
                    previous = c->kind == StmtKind::Default ? std::nullopt : std::optional<std::uint32_t>(t);
                    block(c->body);
                }
                if (!has_default)    // The compiler ends every switch with a default case
                {
                    Token cs;
                    cs.kind = TokenKind::Case;
                    cs.word = 0xFFFF;
                    const std::uint32_t t = add(cs);
                    if (previous) S.tokens[*previous].target = static_cast<std::int32_t>(t);
                    emit_statement(t);
                }
                else if (previous)
                {
                    // The last real case's "next" is the end
                    const int after = new_label();
                    jump_to(*previous, after);
                    bind(after);
                }
                m_loops.pop_back();
                bind(end);
            }

            // ---- Expressions --------------------------------------------------------------------------------------
            Value condition(const Expr& e)
            {
                const Type b = make(K::Bool);
                return convert(expression(e, &b), &b, e.line);
            }

            std::uint32_t nothing()
            {
                Token n;
                n.kind = TokenKind::Nothing;
                return add(n);
            }

            // A literal int's token for its final type
            void settle_literal(Value& v, const Type* want)
            {
                if (!v.literal) return;
                Token& t = S.tokens[v.token];
                const std::int64_t n = v.literal_value;
                if (want && want->kind == K::Byte && n >= 0 && n <= 255)
                {
                    t = Token{};
                    t.kind = TokenKind::ByteConst;
                    t.byte_value = static_cast<std::uint8_t>(n);
                    v.type = make(K::Byte);
                }
                else if (want && want->kind == K::Float)
                {
                    t = Token{};
                    t.kind = TokenKind::FloatConst;
                    t.floats[0] = static_cast<float>(n);
                    v.type = make(K::Float);
                }
                else
                {
                    t = Token{};
                    if (n == 0) t.kind = TokenKind::IntZero;
                    else if (n == 1) t.kind = TokenKind::IntOne;
                    else if (n > 1 && n <= 255) { t.kind = TokenKind::IntConstByte; t.byte_value = static_cast<std::uint8_t>(n); }
                    else { t.kind = TokenKind::IntConst; t.int_value = static_cast<std::int32_t>(n); }
                    v.type = make(K::Int);
                }
                for (std::size_t i = 0; i < scripts_map().kinds.size(); ++i)
                    if (scripts_map().kinds[i] == t.kind) { t.opcode = static_cast<std::uint8_t>(i); break; }
                v.literal = false;
            }

            // `v` used where `want` is expected: implicit conversions only where the compiler makes them silently
            // UE3's automatic conversions, which the compiler makes with a cast token: numbers among themselves, and
            // numbers, bools and names to strings. Their cost ranks overloads (widening before narrowing); -1: none.
            static int auto_cost(K from, K to)
            {
                if (from == to) return 0;
                const auto numeric = [](K k) { return k == K::Byte || k == K::Int || k == K::Float; };
                if (numeric(from) && numeric(to))
                {
                    if ((from == K::Byte && to == K::Int) || (from == K::Int && to == K::Float)) return 1;
                    if (from == K::Byte && to == K::Float) return 2;
                    return from == K::Float && to == K::Byte ? 4 : 3;    // Int -> Byte, Float -> Int
                }
                if (to == K::String && (numeric(from) || from == K::Bool || from == K::Name)) return 5;
                return -1;
            }

            Value convert(Value v, const Type* want, int line)
            {
                settle_literal(v, want);
                (void)line;
                if (want && v.type.kind != want->kind && auto_cost(v.type.kind, want->kind) > 0)
                {
                    const std::uint8_t code = cast_code(v.type.kind, want->kind);
                    if (code != kNoCast) return cast_token(code, v, make(want->kind, want->kind == K::Byte ? want->ref : ObjectRef{}));
                }
                return v;
            }

            Value variable(TokenKind kind, ObjectRef prop)
            {
                Token t;
                t.kind = kind;
                t.object = reference(prop);
                if (t.object.is_null()) fail(0, "no reference to " + M.name_of(prop) + " in " + m_pkg.name());
                Value v;
                v.type = M.property_type(prop);
                v.token = add(t);
                v.lvalue = true;
                if (v.type.kind == K::Bool && M.array_dim(prop) <= 1)
                {
                    Token b;
                    b.kind = TokenKind::BoolVariable;
                    b.children = {v.token};
                    v.token = add(b);
                }
                return v;
            }

            // A name in scope: local, parameter, state or class property
            // The class this one is declared `within` (Outer's type), searching the supers
            ObjectRef within() { return within_of(m_class); }
            ObjectRef within_of(ObjectRef cls)
            {
                std::optional<ObjectRef> c = cls.valid() ? std::optional<ObjectRef>(cls) : std::nullopt;
                for (int guard = 0; c && guard < 64; ++guard)
                {
                    const FieldObject* f = M.field(*c);
                    if (f && f->class_data && !f->class_data->within.is_null())
                        if (const auto w = M.def(*c->package, f->class_data->within); w && !iequal(M.name_of(*w), "Object")) return *w;
                    c = M.super_of(*c);
                }
                return {};
            }

            std::optional<Value> name_value(std::string_view name)
            {
                if (m_function)
                    for (const ObjectRef c : M.children(m_owner))
                        if (M.is_property(c) && iequal(M.name_of(c), name))
                            return variable((M.property_flags(c) & PropertyFlag::OutParm) ? TokenKind::OutVariable : TokenKind::LocalVariable, c);
                if (m_state.valid())
                    if (const auto p = M.member(m_state, name, [&](ObjectRef r) { return M.is_property(r); }))
                        return variable(M.class_of(M.outer_of(*p)) == "State" ? TokenKind::StateVariable : TokenKind::InstanceVariable, *p);
                if (m_class.valid())
                    if (const auto p = M.member(m_class, name, [&](ObjectRef r) { return M.is_property(r); }))
                    {
                        Value v = variable(TokenKind::InstanceVariable, *p);
                        if (iequal(name, "Outer") && v.type.kind == K::Object)
                            if (const ObjectRef w = within(); w.valid()) v.type.ref = w;
                        return v;
                    }
                return std::nullopt;
            }

            std::optional<Value> const_value(std::string_view name, int line, const Type* want)
            {
                const auto is_const = [&](ObjectRef r) { return M.class_of(r) == "Const"; };
                std::optional<ObjectRef> c;
                if (m_state.valid()) c = M.member(m_state, name, is_const);
                if (!c && m_class.valid()) c = M.member(m_class, name, is_const);
                if (!c) return std::nullopt;
                const FieldObject* f = M.field(*c);
                if (!f || !f->const_value) return std::nullopt;
                if (m_const_depth > 8) fail(line, "const " + std::string(name) + " refers to itself");
                Block parsed;
                ParseError pe;
                if (!parse_body(f->const_value->text + ";", M.operator_set, parsed, pe) || parsed.size() != 1 || !parsed[0]->expr)
                    fail(line, "the value of const " + std::string(name) + " doesn't parse: " + f->const_value->text);
                ++m_const_depth;
                Value v = expression(*parsed[0]->expr, want);
                --m_const_depth;
                return v;
            }

            Function* find_function(ObjectRef scope, std::string_view name)
            {
                if (!scope.valid()) return nullptr;
                if (const auto f = M.member(scope, name, [&](ObjectRef r) { return M.is_function(r); })) return M.function(*f);
                return nullptr;
            }
            Function* scope_function(std::string_view name)
            {
                if (m_state.valid())
                    if (Function* f = find_function(m_state, name)) return f;
                return find_function(m_class, name);
            }

            Value expression(const Expr& e, const Type* want)
            {
                switch (e.kind)
                {
                case ExprKind::Int:
                {
                    Token t;
                    t.kind = TokenKind::IntConst;
                    Value v;
                    v.token = add(t);
                    v.literal = true;
                    v.literal_value = e.int_value;
                    v.type = make(K::Int);
                    if (want) settle_literal(v, want);
                    return v;
                }
                case ExprKind::Float:
                {
                    Token t;
                    t.kind = TokenKind::FloatConst;
                    t.floats[0] = e.floats[0];
                    return {add(t), make(K::Float)};
                }
                case ExprKind::String:
                {
                    Token t;
                    t.kind = TokenKind::StringConst;
                    t.string_value = e.text;
                    return {add(t), make(K::String)};
                }
                case ExprKind::Name:
                {
                    Token t;
                    t.kind = TokenKind::NameConst;
                    t.name = name_of_text(e.text, e.line);
                    return {add(t), make(K::Name)};
                }
                case ExprKind::Object:
                {
                    const auto d = M.find_path(e.text);
                    Token t;
                    t.kind = TokenKind::ObjectConst;
                    if (!d)
                    {
                        t.object = imported(e.text, e.type_name);
                        if (t.object.is_null()) fail(e.line, "no object " + e.text);
                        return {add(t), iequal(e.type_name, "Class") ? make(K::Class) : make(K::Object)};
                    }
                    t.object = reference(*d);
                    if (t.object.is_null()) fail(e.line, "no reference to " + e.text + " in " + m_pkg.name());
                    const bool is_class = iequal(e.type_name, "Class");
                    return {add(t), is_class ? make(K::Class, *d) : make(K::Object)};
                }
                case ExprKind::Vector:
                {
                    Token t;
                    t.kind = TokenKind::VectorConst;
                    std::copy(std::begin(e.floats), std::end(e.floats), t.floats.begin());
                    return {add(t), struct_type("Vector")};
                }
                case ExprKind::Rotator:
                {
                    Token t;
                    t.kind = TokenKind::RotationConst;
                    std::copy(std::begin(e.ints), std::end(e.ints), t.ints.begin());
                    return {add(t), struct_type("Rotator")};
                }
                case ExprKind::True:
                case ExprKind::False:
                {
                    Token t;
                    t.kind = e.kind == ExprKind::True ? TokenKind::True : TokenKind::False;
                    return {add(t), make(K::Bool)};
                }
                case ExprKind::None:
                {
                    Token t;
                    if (want && want->kind == K::Delegate)
                    {
                        t.kind = TokenKind::DelegateProperty;
                        t.name = name_of_text("None", e.line);
                        return {add(t), *want};
                    }
                    t.kind = TokenKind::NoObject;
                    return {add(t), make(K::Null)};
                }
                case ExprKind::Self:
                {
                    Token t;
                    t.kind = TokenKind::Self;
                    return {add(t), make(K::Object, m_class)};
                }
                case ExprKind::Identifier:
                {
                    if (auto v = name_value(e.text)) return *v;
                    // A const: its value, compiled in place (the UE3 compiler inlines them)
                    if (auto v = const_value(e.text, e.line, want)) return *v;
                    // A function used as a value: a delegate assignment's right side
                    if (Function* f = scope_function(e.text))
                    {
                        Token t;
                        t.kind = TokenKind::DelegateProperty;
                        t.name = name_of_text(f->name, e.line);
                        return {add(t), make(K::Delegate, f->def)};
                    }
                    // An enumerator (cooked bytecode keeps only its value)
                    if (const auto en = M.enumerator(e.text))
                    {
                        Token t;
                        t.kind = TokenKind::ByteConst;
                        t.byte_value = en->second;
                        return {add(t), make(K::Byte, en->first)};
                    }
                    fail(e.line, "unknown identifier '" + e.text + "'");
                }
                case ExprKind::Default:
                {
                    if (const auto p = M.member(m_class, e.text, [&](ObjectRef r) { return M.is_property(r); }))
                        return variable(TokenKind::DefaultVariable, *p);
                    fail(e.line, "unknown default property '" + e.text + "'");
                }
                case ExprKind::Member:
                    return member(e, want);
                case ExprKind::Call:
                    return call(e, want);
                case ExprKind::Index:
                {
                    const Value array = expression(*e.left, nullptr);
                    const Type in = make(K::Int);
                    const Value index = convert(expression(*e.args[0], &in), &in, e.line);
                    Token t;
                    t.kind = array.type.kind == K::Array ? TokenKind::DynamicArrayElement : TokenKind::ArrayElement;
                    t.children = {index.token, array.token};
                    Value v;
                    v.type = array.type.kind == K::Array && array.type.inner ? *array.type.inner : array.type;
                    v.lvalue = true;
                    // A static array of bools: the element is read through BoolVariable? (no: ArrayElement of the prop)
                    v.token = add(t);
                    return v;
                }
                case ExprKind::Unary:
                    return operator_call(e.text, {e.left.get()}, e.postfix ? 1 : -1, e.line, want);
                case ExprKind::Binary:
                    return operator_call(e.text, {e.left.get(), e.right.get()}, 0, e.line, want);
                case ExprKind::Assign:
                {
                    const Value left = expression(*e.left, nullptr);
                    if (!left.lvalue) fail(e.line, "the left side of '=' can't be assigned");
                    const Value right = convert(expression(*e.right, &left.type), &left.type, e.line);
                    modified(left.token);
                    Token t;
                    t.kind = left.type.kind == K::Bool ? TokenKind::LetBool
                           : left.type.kind == K::Delegate ? TokenKind::LetDelegate
                                                           : TokenKind::Let;
                    t.children = {left.token, right.token};
                    return {add(t), make(K::Void)};
                }
                case ExprKind::Conditional:
                {
                    const Value c = condition(*e.args[0]);
                    Value a = expression(*e.args[1], want);
                    Value b = expression(*e.args[2], want ? want : &a.type);
                    if (a.literal) settle_literal(a, &b.type);
                    Token t;
                    t.kind = TokenKind::Conditional;
                    t.children = {c.token, a.token, b.token};
                    return {add(t), a.type};
                }
                case ExprKind::New:
                {
                    Token t;
                    t.kind = TokenKind::New;
                    for (int i = 0; i < 5; ++i)
                        t.children.push_back(e.args[static_cast<std::size_t>(i)] ? expression(*e.args[static_cast<std::size_t>(i)], nullptr).token : nothing());
                    return {add(t), make(K::Object)};
                }
                case ExprKind::MetaCast:
                {
                    const auto c = class_named(e.type_name);
                    if (!c) fail(e.line, "unknown class " + e.type_name);
                    Token t;
                    t.kind = TokenKind::MetaClassCast;
                    t.object = reference(*c);
                    t.children = {expression(*e.left, nullptr).token};
                    return {add(t), make(K::Class, *c)};
                }
                case ExprKind::Super:
                case ExprKind::Global:
                case ExprKind::StaticMember:
                    fail(e.line, "a function reference needs a call");
                case ExprKind::Empty:
                {
                    Token t;
                    t.kind = TokenKind::EmptyParm;
                    return {add(t), make(K::Void)};
                }
                }
                fail(e.line, "unsupported expression");
            }

            FName name_of_text(std::string_view text, int line)
            {
                // "Name_3": the instance number is the suffix + 1
                const auto found = m_pkg.names().find(text);
                if (found) return FName{*found, 0};
                const auto us = text.rfind('_');
                if (us != std::string_view::npos && us + 1 < text.size())
                {
                    int n = 0;
                    bool digits = true;
                    for (char c : text.substr(us + 1)) digits = digits && std::isdigit(static_cast<unsigned char>(c));
                    if (digits)
                    {
                        n = std::stoi(std::string(text.substr(us + 1)));
                        if (const auto base = m_pkg.names().find(text.substr(0, us))) return FName{*base, n + 1};
                    }
                }
                if (m_editor && m_editor->name) return m_editor->name(text);
                fail(line, "the name '" + std::string(text) + "' isn't in " + m_pkg.name() + "'s name table");
            }

            Type struct_type(std::string_view name)
            {
                if (const auto d = M.find_path(std::string("Core.Object.") + std::string(name))) return make(K::Struct, *d);
                return make(K::Struct);
            }
            // An object the owner's package imports, by path, whether or not it resolves to an export
            ObjectIndex imported(std::string_view path, std::string_view klass = {})
            {
                for (std::size_t i = 0; i < m_pkg.imports().size(); ++i)
                {
                    const ObjectIndex idx = ObjectIndex::from_import(i);
                    if (!klass.empty() && !iequal(m_pkg.class_name(idx), klass)) continue;
                    if (iequal(m_pkg.object_path(idx), path) || (path.find('.') == std::string_view::npos && iequal(m_pkg.object_name(idx), path)))
                        return idx;
                }
                return {};
            }

            std::optional<ObjectRef> class_named(std::string_view name)
            {
                for (const Package* p : M.scripts)
                    for (std::size_t i = 0; i < p->exports().size(); ++i)
                    {
                        const ObjectIndex idx = ObjectIndex::from_export(i);
                        if (p->outer(idx).is_null() && p->class_name(idx) == "Class" && iequal(p->object_name(idx), name))
                            return ObjectRef{p, idx};
                    }
                return std::nullopt;
            }

            // a.b: a struct's member, an object's property (Context), an array's Length, a.default.b
            Value member(const Expr& e, const Type* want)
            {
                (void)want;
                // a.default.X
                if (e.left->kind == ExprKind::Member && iequal(e.left->text, "default"))
                {
                    const Value obj = expression(*e.left->left, nullptr);
                    const ObjectRef cls = obj.type.ref;
                    const auto p = M.member(cls.valid() ? cls : m_class, e.text, [&](ObjectRef r) { return M.is_property(r); });
                    if (!p) fail(e.line, "unknown default property '" + e.text + "'");
                    const Value inner = variable(TokenKind::DefaultVariable, *p);
                    return context(obj, inner, obj.type.kind == K::Class, e.line);
                }
                const Value obj = expression(*e.left, nullptr);
                if (e.right)    // obj.<constant>
                {
                    const Value c = expression(*e.right, nullptr);
                    return context(obj, c, false, e.line);
                }
                if (obj.type.kind == K::Array && iequal(e.text, "Length"))
                {
                    Token t;
                    t.kind = TokenKind::DynamicArrayLength;
                    t.children = {obj.token};
                    Value v;
                    v.token = add(t);
                    v.type = make(K::Int);
                    v.lvalue = true;
                    return v;
                }
                if (obj.type.kind == K::Struct)
                {
                    const auto p = M.member(obj.type.ref, e.text, [&](ObjectRef r) { return M.is_property(r); });
                    if (!p) fail(e.line, "no member '" + e.text + "' in the struct");
                    Token t;
                    t.kind = TokenKind::StructMember;
                    t.object = reference(*p);
                    t.object2 = reference(obj.type.ref);
                    t.children = {obj.token};
                    Value v;
                    v.type = M.property_type(*p);
                    v.token = add(t);
                    v.lvalue = obj.lvalue;
                    if (v.type.kind == K::Bool)
                    {
                        Token b;
                        b.kind = TokenKind::BoolVariable;
                        b.children = {v.token};
                        v.token = add(b);
                    }
                    return v;
                }
                if (obj.type.is_object_like())
                {
                    const ObjectRef cls = obj.type.ref.valid() ? obj.type.ref : m_class;
                    const auto p = M.member(cls, e.text, [&](ObjectRef r) { return M.is_property(r); });
                    if (!p) fail(e.line, "no property '" + e.text + "' in " + (cls.valid() ? M.name_of(cls) : std::string("?")));
                    Value inner = variable(TokenKind::InstanceVariable, *p);
                    if (iequal(e.text, "Outer") && inner.type.kind == K::Object)
                        if (const ObjectRef w = within_of(cls); w.valid()) inner.type.ref = w;
                    return context(obj, inner, false, e.line);
                }
                fail(e.line, "'." + e.text + "' on a value without members");
            }

            // A value written to: the struct members along its path are modifications (StructMember's second flag)
            void modified(std::uint32_t token)
            {
                std::optional<std::uint32_t> innermost;
                struct Flag
                {
                    Script& s;
                    std::optional<std::uint32_t>& at;
                    ~Flag()
                    {
                        if (at) s.tokens[*at].byte_value2 = 1;
                    }
                } flag{S, innermost};
                for (int guard = 0; guard < 64; ++guard)
                {
                    Token& t = S.tokens[token];
                    if (t.kind == TokenKind::StructMember)
                    {
                        innermost = token;
                        token = t.children.at(0);
                    }
                    else if (t.kind == TokenKind::BoolVariable && !t.children.empty())
                    {
                        token = t.children[0];
                    }
                    else if ((t.kind == TokenKind::ArrayElement || t.kind == TokenKind::DynamicArrayElement) && t.children.size() == 2)
                    {
                        token = t.children[1];
                    }
                    else if (t.kind == TokenKind::DynamicArrayLength && !t.children.empty())
                    {
                        token = t.children[0];
                    }
                    else if ((t.kind == TokenKind::Context || t.kind == TokenKind::ClassContext) && !t.children.empty())
                    {
                        token = t.children[0];    // The object the property is written on, and the struct it came from
                    }
                    else
                    {
                        return;
                    }
                }
            }

            Value context(Value obj, const Value& inner, bool class_context, int line)
            {
                (void)line;
                if (obj.type.kind == K::Interface)    // An interface value is reached through InterfaceContext
                {
                    Token ic;
                    ic.kind = TokenKind::InterfaceContext;
                    ic.children = {obj.token};
                    obj.token = add(ic);
                }
                Token t;
                t.kind = class_context ? TokenKind::ClassContext : TokenKind::Context;
                const ObjectRef prop = inner.returns.valid() ? inner.returns : inner.type.property;
                const bool constructed = inner.type.kind == K::Array || inner.type.kind == K::String;
                (void)prop;
                const std::uint32_t count = inner.type.property.valid() && !inner.returns.valid()
                                                ? static_cast<std::uint32_t>(std::max(1, M.array_dim(inner.type.property))) : 1u;
                t.word2 = constructed ? 0 : static_cast<std::uint16_t>(M.size_of(inner.type) * count);
                t.children = {obj.token, inner.token};
                // The UE3 compiler's skip for a context on Outer: the size of the Outer variable, whatever the member
                if (const Token& o = S.tokens[obj.token]; o.kind == TokenKind::InstanceVariable && m_pkg.is_valid(o.object) &&
                                                           iequal(m_pkg.object_name(o.object), "Outer"))
                    t.skip_size = 5;
                Value v;
                v.token = add(t);
                v.type = inner.type;
                v.lvalue = inner.lvalue;
                v.returns = inner.returns;
                return v;
            }

            // ---- Calls --------------------------------------------------------------------------------------------
            std::vector<std::uint32_t> arguments(const Function& f, const std::vector<ExprPtr>& args, int line)
            {
                std::vector<std::uint32_t> out;
                for (std::size_t i = 0; i < f.params.size(); ++i)
                {
                    const Param& p = f.params[i];
                    if (i >= args.size() || args[i]->kind == ExprKind::Empty)
                    {
                        if (i >= args.size() && !p.optional()) fail(line, "too few arguments for " + f.name);
                        Token t;
                        t.kind = TokenKind::EmptyParm;
                        out.push_back(add(t));
                        continue;
                    }
                    Value v = convert(expression(*args[i], &p.type), &p.type, line);
                    if (p.out()) modified(v.token);
                    if (p.skip())
                    {
                        Token s;
                        s.kind = TokenKind::Skip;
                        s.children = {v.token};
                        v.token = add(s);
                    }
                    out.push_back(v.token);
                }
                if (args.size() > f.params.size()) fail(line, "too many arguments for " + f.name);
                return out;
            }

            Value call_token(Function& f, const std::vector<ExprPtr>& args, int line, bool force_final, bool global)
            {
                Token t;
                if (global)
                {
                    t.kind = TokenKind::GlobalFunction;
                    t.name = name_of_text(f.name, line);
                }
                else if (f.native != 0 && !force_final)
                {
                    t.kind = TokenKind::NativeFunction;
                    t.native_index = f.native;
                }
                else if (f.is_final() || force_final)
                {
                    t.kind = TokenKind::FinalFunction;
                    t.object = reference(f.def);
                    if (t.object.is_null()) fail(line, "no reference to " + f.name + " in " + m_pkg.name());
                }
                else
                {
                    t.kind = TokenKind::VirtualFunction;
                    t.name = name_of_text(f.name, line);
                }
                t.children = arguments(f, args, line);
                Value v;
                v.token = add(t);
                v.type = f.ret ? f.ret->type : make(K::Void);
                v.type.property = {};
                if (f.ret) v.returns = f.ret->property;
                v.callee = &f;
                return v;
            }

            Value call(const Expr& e, const Type* want)
            {
                const Expr& callee = *e.left;
                switch (callee.kind)
                {
                case ExprKind::Identifier:
                {
                    // A cast?
                    K to;
                    std::string sname;
                    if (e.args.size() == 1 && primitive_cast_name(callee.text, to, sname)) return primitive_cast(to, sname, *e.args[0], e.line);
                    const auto as_variable = name_value(callee.text);
                    if (e.args.size() == 1 && (!as_variable || as_variable->type.kind != K::Delegate) && !scope_function(callee.text))
                    {
                        if (const auto c = class_named(callee.text)) return dynamic_cast_to(*c, *e.args[0], e.line);
                        if (const ObjectIndex c = imported(callee.text, "Class"); !c.is_null())
                        {
                            const Value v = expression(*e.args[0], nullptr);
                            Token t;
                            t.kind = TokenKind::DynamicCast;
                            t.object = c;
                            t.children = {v.token};
                            return {add(t), make(K::Object)};
                        }
                    }
                    // A delegate held in a variable (a parameter, a local, a property) called
                    if (const auto var = name_value(callee.text); var && var->type.kind == K::Delegate && var->type.ref.valid())
                    {
                        Function* d = M.function(var->type.ref);
                        const Token& vt = S.tokens[var->token];
                        Token t;
                        t.kind = TokenKind::DelegateFunction;
                        t.byte_value = vt.kind == TokenKind::LocalVariable || vt.kind == TokenKind::OutVariable ? 1 : 0;
                        t.object = vt.object;
                        t.name = name_of_text(d->name, e.line);
                        t.children = arguments(*d, e.args, e.line);
                        Value v;
                        v.token = add(t);
                        v.type = d->ret ? d->ret->type : make(K::Void);
                        if (d->ret) v.returns = d->ret->property;
                        return v;
                    }
                    if (Function* f = scope_function(callee.text))
                    {
                        if (f->is_delegate()) return delegate_call(*f, callee.text, e, nullptr);
                        return call_token(*f, e.args, e.line, false, false);
                    }
                    fail(e.line, "unknown function '" + callee.text + "'");
                }
                case ExprKind::Super:
                {
                    ObjectRef scope;
                    if (!callee.type_name.empty())
                    {
                        const auto c = class_named(callee.type_name);
                        if (!c) fail(e.line, "unknown class " + callee.type_name);
                        scope = *c;
                    }
                    else
                    {
                        const ObjectRef inner = m_state.valid() && !m_is_state && M.class_of(M.outer_of(m_owner)) == "State" ? m_state : m_class;
                        scope = M.super_of(inner).value_or(ObjectRef{});
                    }
                    Function* f = find_function(scope, callee.text);
                    if (!f) fail(e.line, "no super function " + callee.text);
                    return call_token(*f, e.args, e.line, true, false);
                }
                case ExprKind::Global:
                {
                    Function* f = find_function(m_class, callee.text);
                    if (!f) fail(e.line, "no global function " + callee.text);
                    return call_token(*f, e.args, e.line, false, true);
                }
                case ExprKind::Member:
                {
                    // Array methods
                    const Value obj = expression(*callee.left, nullptr);
                    if (obj.type.kind == K::Array) return array_method(obj, callee.text, e, want);
                    ObjectRef cls = obj.type.ref.valid() ? obj.type.ref : m_class;
                    if (obj.type.kind == K::Class) cls = obj.type.ref.valid() ? obj.type.ref : m_class;
                    Function* f = find_function(cls, callee.text);
                    if (!f)
                    {
                        // A delegate property of the object?
                        fail(e.line, "no function " + callee.text + " in " + (cls.valid() ? M.name_of(cls) : std::string("?")));
                    }
                    const Value inner = f->is_delegate() ? delegate_call(*f, callee.text, e, &cls)
                                                         : call_token(*f, e.args, e.line, false, false);
                    return context(obj, inner, false, e.line);
                }
                case ExprKind::StaticMember:
                {
                    const Value obj = expression(*callee.left, nullptr);
                    const ObjectRef cls = obj.type.ref.valid() ? obj.type.ref : m_class;
                    Function* f = find_function(cls, callee.text);
                    if (!f) fail(e.line, "no static function " + callee.text);
                    const Value inner = call_token(*f, e.args, e.line, false, false);
                    return context(obj, inner, true, e.line);
                }
                default:
                    fail(e.line, "this can't be called");
                }
            }

            Value delegate_call(Function& f, std::string_view name, const Expr& e, const ObjectRef* in)
            {
                (void)in;
                Token t;
                t.kind = TokenKind::DelegateFunction;
                // The delegate's property: __Name__Delegate
                const std::string prop = "__" + std::string(name) + "__Delegate";
                const auto p = M.member(in ? *in : m_class, prop, [&](ObjectRef r) { return M.is_property(r); });
                if (p) t.object = reference(*p);
                t.name = name_of_text(f.name, e.line);
                t.children = arguments(f, e.args, e.line);
                Value v;
                v.token = add(t);
                v.type = f.ret ? f.ret->type : make(K::Void);
                return v;
            }

            Value primitive_cast(K to, const std::string& sname, const Expr& arg, int line)
            {
                Value v = expression(arg, nullptr);
                if (v.literal)
                {
                    // A literal inside a cast: the compiler casts a byte constant (enum values) to int, an int otherwise
                    const Type b = make(to == K::Int ? K::Byte : K::Int);
                    settle_literal(v, &b);
                }
                K from = v.type.kind;
                if (from == K::Class || from == K::Null) from = K::Object;
                if (from == K::Struct && v.type.ref.valid())
                {
                    const std::string n = lower(M.name_of(v.type.ref));
                    // Vector <-> Rotator, and both to string / bool
                    std::uint8_t code = kNoCast;
                    if (n == "rotator" && sname == "vector") code = 0x39;
                    else if (n == "vector" && sname == "rotator") code = 0x50;
                    else if (n == "vector" && to == K::String) code = 0x58;
                    else if (n == "rotator" && to == K::String) code = 0x59;
                    else if (n == "vector" && to == K::Bool) code = 0x4F;
                    else if (n == "rotator" && to == K::Bool) code = 0x51;
                    if (code != kNoCast) return cast_token(code, v, to == K::Struct ? struct_type(sname == "vector" ? "Vector" : "Rotator") : make(to));
                    fail(line, "no cast from this struct");
                }
                if (to == K::Struct)
                {
                    if (from == K::String) return cast_token(sname == "vector" ? 0x4D : 0x4E, v, struct_type(sname == "vector" ? "Vector" : "Rotator"));
                    fail(line, "no cast to " + sname);
                }
                if (from == to) return v;    // No token: the value already has the type
                const std::uint8_t code = cast_code(from, to);
                if (code == kNoCast) fail(line, "no primitive cast from this type");
                return cast_token(code, v, make(to));
            }
            Value cast_token(std::uint8_t code, const Value& v, Type to)
            {
                Token t;
                t.kind = TokenKind::PrimitiveCast;
                t.byte_value = code;
                t.children = {v.token};
                return {add(t), std::move(to)};
            }
            bool implements(ObjectRef cls, ObjectRef iface)
            {
                std::optional<ObjectRef> c = cls.valid() ? std::optional<ObjectRef>(cls) : std::nullopt;
                for (int guard = 0; c && guard < 64; ++guard)
                {
                    if (const FieldObject* f = M.field(*c); f && f->class_data)
                        for (const auto& [klass, prop] : f->class_data->interfaces)
                            if (const auto d = M.def(*c->package, klass); d && *d == iface) return true;
                    c = M.super_of(*c);
                }
                return false;
            }

            Value dynamic_cast_to(ObjectRef cls, const Expr& arg, int line)
            {
                const Value v = expression(arg, nullptr);
                Token t;
                const bool interface = M.field(cls) && M.field(cls)->class_data && (M.field(cls)->class_data->flags & 0x4000u);
                const bool from_object = v.type.kind == K::Object && (!v.type.ref.valid() || iequal(M.name_of(v.type.ref), "Object"));
                t.kind = interface && !from_object ? TokenKind::InterfaceCast : TokenKind::DynamicCast;
                t.object = reference(cls);
                if (t.object.is_null()) fail(line, "no reference to " + M.name_of(cls) + " in " + m_pkg.name());
                t.children = {v.token};
                return {add(t), make(interface ? K::Interface : K::Object, cls)};
            }

            Value array_method(const Value& array, std::string_view method, const Expr& e, const Type* want)
            {
                (void)want;
                const std::string m = lower(method);
                const Type* inner = array.type.inner.get();
                const Type in = make(K::Int);
                Token t;
                std::vector<std::uint32_t> args;
                const auto arg = [&](std::size_t i, const Type* type) {
                    if (i >= e.args.size()) fail(e.line, "too few arguments for " + std::string(method));
                    return convert(expression(*e.args[i], type), type, e.line).token;
                };
                Type result = make(K::Void);
                if (m == "find")
                {
                    t.kind = e.args.size() == 2 ? TokenKind::DynamicArrayFindStruct : TokenKind::DynamicArrayFind;
                    if (e.args.size() == 2)
                    {
                        const Type n = make(K::Name);
                        args = {arg(0, &n), arg(1, nullptr)};
                    }
                    else
                    {
                        args = {arg(0, inner)};
                    }
                    result = make(K::Int);
                }
                else if (m == "insert" || m == "remove")
                {
                    t.kind = m == "insert" ? TokenKind::DynamicArrayInsert : TokenKind::DynamicArrayRemove;
                    args = {arg(0, &in), arg(1, &in)};
                }
                else if (m == "add")
                {
                    t.kind = TokenKind::DynamicArrayAdd;
                    args = {arg(0, &in)};
                    result = make(K::Int);
                }
                else if (m == "additem")
                {
                    t.kind = TokenKind::DynamicArrayAddItem;
                    args = {arg(0, inner)};
                    result = make(K::Int);
                }
                else if (m == "removeitem")
                {
                    t.kind = TokenKind::DynamicArrayRemoveItem;
                    args = {arg(0, inner)};
                }
                else if (m == "insertitem")
                {
                    t.kind = TokenKind::DynamicArrayInsertItem;
                    args = {arg(0, &in), arg(1, inner)};
                }
                else
                {
                    fail(e.line, "unknown array method " + std::string(method));
                }
                if (m == "additem" || m == "removeitem" || m == "insertitem") modified(array.token);
                t.children.push_back(array.token);
                t.children.insert(t.children.end(), args.begin(), args.end());
                return {add(t), result};
            }

            // ---- Operators ----------------------------------------------------------------------------------------
            // How well an argument of type `have` fits a parameter of type `p`: 0 exact, 1 with a widening the compiler
            // makes silently, -1 not at all
            int fit(const Type& have, const Type& p, bool literal)
            {
                if (have.kind == p.kind)
                {
                    if (have.kind == K::Struct) return have.ref == p.ref || !have.ref.valid() || !p.ref.valid() ? 0 : -1;
                    if (have.kind == K::Byte && p.ref.valid() && have.ref.valid() && !(have.ref == p.ref)) return 1;
                    return 0;
                }
                if (literal && (p.kind == K::Byte || p.kind == K::Float)) return 1;
                if (const int c = auto_cost(have.kind, p.kind); c > 0) return 1 + c;
                if (have.kind == K::Null && (p.is_object_like() || p.kind == K::Delegate)) return 0;
                if (have.is_object_like() && p.is_object_like()) return p.kind == K::Object && have.kind == K::Class ? 1 : (have.kind == p.kind ? 0 : -1);
                return -1;
            }

            Value operator_call(const std::string& text, std::vector<const Expr*> operands, int fixity, int line, const Type* want)
            {
                (void)want;
                // Operands first (literals keep their kind open until the operator is chosen)
                std::vector<Value> values;
                for (const Expr* o : operands) values.push_back(expression(*o, nullptr));
                const auto [lo, hi] = M.operators.equal_range(lower(text));
                Function* best = nullptr;
                int best_score = 1 << 30;
                for (auto it = lo; it != hi; ++it)
                {
                    Function* f = it->second;
                    if (f->params.size() != values.size()) continue;
                    const bool pre = (f->flags & FunctionFlag::PreOperator) != 0;
                    if (values.size() == 1 && ((fixity < 0) != pre)) continue;
                    int score = 0;
                    for (std::size_t i = 0; i < values.size() && score >= 0; ++i)
                    {
                        const int s = fit(values[i].type, f->params[i].type, values[i].literal);
                        score = s < 0 ? -1 : score + s;
                    }
                    if (score >= 0 && score < best_score)
                    {
                        best = f;
                        best_score = score;
                    }
                }
                if (!best && values.size() == 2 && (text == "==" || text == "!="))
                {
                    const bool eq = text == "==";
                    Token t;
                    const Value& a = values[0];
                    if (a.type.kind == K::Struct)
                    {
                        t.kind = eq ? TokenKind::StructCmpEq : TokenKind::StructCmpNe;
                        t.object = reference(a.type.ref);
                    }
                    else if (a.type.kind == K::Delegate || values[1].type.kind == K::Delegate)
                    {
                        t.kind = eq ? TokenKind::DelegateCmpEq : TokenKind::DelegateCmpNe;
                        for (Value& v : values)
                            if (S.tokens[v.token].kind == TokenKind::NoObject) S.tokens[v.token].kind = TokenKind::EmptyDelegate;
                    }
                    else
                    {
                        fail(line, "no operator " + text + " for these operand types");
                    }
                    t.children = {values[0].token, values[1].token};
                    return {add(t), make(K::Bool)};
                }
                if (!best) fail(line, "no operator " + text + " for these operand types");
                Token t;
                if (best->native != 0)
                {
                    t.kind = TokenKind::NativeFunction;
                    t.native_index = best->native;
                }
                else
                {
                    t.kind = TokenKind::FinalFunction;
                    t.object = reference(best->def);
                    if (t.object.is_null()) fail(line, "no reference to operator " + best->name + " in " + m_pkg.name());
                }
                for (std::size_t i = 0; i < values.size(); ++i)
                {
                    Value v = convert(values[i], &best->params[i].type, line);
                    if (best->params[i].out()) modified(v.token);
                    if (best->params[i].skip())
                    {
                        Token s;
                        s.kind = TokenKind::Skip;
                        s.children = {v.token};
                        v.token = add(s);
                    }
                    t.children.push_back(v.token);
                }
                Value v;
                v.token = add(t);
                v.type = best->ret ? best->ret->type : make(K::Void);
                v.type.property = {};
                if (best->ret) v.returns = best->ret->property;
                return v;
            }
        };
    }

    ScriptCompiler::ScriptCompiler(Linker& linker, std::span<const Package* const> scripts)
        : m_impl(std::make_unique<Impl>(linker, scripts))
    {
    }
    ScriptCompiler::~ScriptCompiler() = default;

    CompiledScript ScriptCompiler::compile_body(std::string_view source, ObjectRef owner, std::span<const std::string> default_values,
                                                const PackageEditor* editor)
    {
        CompiledScript out;
        Block body;
        ParseError pe;
        if (!parse_body(source, m_impl->operator_set, body, pe))
        {
            out.errors.push_back(fmt::format("line {}: {}", pe.line, pe.message));
            return out;
        }
        try
        {
            Generator g(*m_impl, owner, out.script, editor);
            g.prologue(default_values);
            g.body(body);
        }
        catch (const Error& e)
        {
            out.errors.push_back(fmt::format("line {}: {}", e.line, e.message));
        }
        catch (const std::exception& e)
        {
            out.errors.push_back(e.what());
        }
        return out;
    }
}
