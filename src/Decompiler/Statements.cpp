#include <UnrealLib/Decompiler/Statements.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include <fmt/format.h>

#include <UnrealLib/Names/NameTable.hpp>

namespace UnrealLib::Decompiler
{
    std::string jump_label(std::uint32_t offset)
    {
        return fmt::format("J0x{:X}", offset);
    }

    namespace
    {
        constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
        constexpr std::uint32_t no_offset = std::numeric_limits<std::uint32_t>::max();
        constexpr std::uint16_t default_case = 0xFFFF;

        /// A construct that `break` (and, for loops, `continue`) can leave.
        struct Frame
        {
            enum class Kind : std::uint8_t
            {
                Loop,
                ForEach,
                Switch
            };
            Kind kind = Kind::Loop;
            std::uint32_t break_offset = no_offset;
            std::uint32_t alt_break_offset = no_offset; ///< ForEach: past the IteratorPop as well as at it.
            std::uint32_t continue_offset = no_offset;
            std::uint32_t increment_offset = no_offset; ///< Loop: last body statement, a for-loop's increment.
            bool used_increment = false;                 ///< A jump to `increment_offset` was taken as `continue`.
        };

        /// Tokens that produce no source of their own at statement level.
        bool is_silent(TokenKind kind)
        {
            switch (kind)
            {
            case TokenKind::DebugInfo:
            case TokenKind::Nothing:
            case TokenKind::EndOfScript:
            case TokenKind::LabelTable:
            case TokenKind::EatReturnValue:
            case TokenKind::EndFunctionParms:
            case TokenKind::EndParmValue:
            case TokenKind::UndefinedVariable:
                return true;
            default:
                return false;
            }
        }

        class Structurer
        {
        public:
            Structurer(const Script& script, const StructureOptions& options, const std::set<std::uint32_t>& needed)
                : m_script(script), m_options(options), m_needed(needed),
                  m_end(static_cast<std::uint32_t>(script.memory_size))
            {
                m_statements = script.statements;
                for (std::size_t i = 0; i < m_statements.size(); ++i)
                {
                    const std::uint32_t offset = token(i).memory_offset;
                    m_offsets.push_back(offset);
                    m_index.emplace(offset, i);
                }
                for (const LabelEntry& label : options.labels)
                {
                    const std::string name =
                        options.names != nullptr ? options.names->to_string(label.name) : jump_label(label.offset);
                    m_named.emplace(static_cast<std::uint32_t>(label.offset), name);
                }
            }

            StructuredScript run()
            {
                m_out.statements = block(0, m_statements.size(), false);
                if (m_needed.contains(m_end) && !m_named.contains(m_end))
                {
                    Statement label;
                    label.kind = StatementKind::Label;
                    label.offset = m_end;
                    label.text = jump_label(m_end);
                    m_out.statements.push_back(std::move(label));
                    m_emitted.insert(m_end);
                }
                if (m_options.drop_trailing_return && !m_out.statements.empty())
                {
                    const Statement& last = m_out.statements.back();
                    if (last.kind == StatementKind::Return && is_empty_return(last.token))
                    {
                        m_out.statements.pop_back();
                    }
                }
                for (const std::uint32_t target : m_needed)
                {
                    if (!m_emitted.contains(target))
                    {
                        // A goto whose target is a token the structuring consumed (or not a statement start).
                        ++m_out.problems;
                    }
                }
                return std::move(m_out);
            }

            std::set<std::uint32_t> goto_targets;

        private:
            const Token& token(std::size_t statement) const { return m_script.tokens[m_statements[statement]]; }

            std::uint32_t offset_of(std::size_t statement) const
            {
                return statement < m_statements.size() ? m_offsets[statement] : m_end;
            }

            std::size_t index_at(std::uint32_t offset) const
            {
                if (offset == m_end)
                {
                    return m_statements.size();
                }
                const auto it = m_index.find(offset);
                return it == m_index.end() ? npos : it->second;
            }

            /// First operand of @p token, skipping a trailing DebugInfo.
            std::uint32_t operand(const Token& t, std::size_t n = 0) const
            {
                std::size_t seen = 0;
                for (const std::uint32_t child : t.children)
                {
                    if (m_script.tokens[child].kind == TokenKind::DebugInfo)
                    {
                        continue;
                    }
                    if (seen++ == n)
                    {
                        return child;
                    }
                }
                return no_token;
            }

            bool is_empty_return(std::uint32_t return_token) const
            {
                const std::uint32_t value = operand(m_script.tokens[return_token]);
                // `return;`, or the compiler's implicit `return ReturnValue;` (ReturnNothing) of a function ending.
                return value == no_token || m_script.tokens[value].kind == TokenKind::Nothing ||
                       m_script.tokens[value].kind == TokenKind::ReturnNothing;
            }

            void claim(std::size_t statement) { m_claimed.insert(statement); }
            bool claimed(std::size_t statement) const { return m_claimed.contains(statement); }

            Statement make(StatementKind kind, std::size_t statement) const
            {
                Statement s;
                s.kind = kind;
                s.token = m_statements[statement];
                s.offset = offset_of(statement);
                return s;
            }

            Statement comment(std::string text, std::uint32_t offset)
            {
                Statement s;
                s.kind = StatementKind::Comment;
                s.offset = offset;
                s.text = std::move(text);
                return s;
            }

            void add_goto(std::vector<Statement>& out, std::size_t statement, std::uint32_t target,
                          std::uint32_t condition = no_token)
            {
                Statement s = make(condition == no_token ? StatementKind::Goto : StatementKind::IfNotGoto, statement);
                s.target = target;
                s.condition = condition;
                out.push_back(std::move(s));
                goto_targets.insert(target);
                ++m_out.gotos;
            }

            void emit_labels(std::size_t statement, std::vector<Statement>& out)
            {
                const std::uint32_t offset = offset_of(statement);
                if (const auto it = m_named.find(offset); it != m_named.end())
                {
                    Statement label;
                    label.kind = StatementKind::Label;
                    label.offset = offset;
                    label.text = it->second;
                    out.push_back(std::move(label));
                    m_emitted.insert(offset);
                }
                else if (m_needed.contains(offset) && !m_emitted.contains(offset))
                {
                    Statement label;
                    label.kind = StatementKind::Label;
                    label.offset = offset;
                    label.text = jump_label(offset);
                    out.push_back(std::move(label));
                    m_emitted.insert(offset);
                }
            }

            /// State changed by structuring a range, so that a range can be structured again differently.
            struct Snapshot
            {
                std::size_t gotos, problems, unresolved, defaults;
                std::set<std::uint32_t> targets;
                std::set<std::size_t> claimed;
                std::set<std::uint32_t> emitted;
            };

            Snapshot snapshot() const
            {
                return {m_out.gotos, m_out.problems, m_out.unresolved, m_out.default_parameters.size(),
                        goto_targets, m_claimed, m_emitted};
            }

            void restore(const Snapshot& snap)
            {
                m_out.gotos = snap.gotos;
                m_out.problems = snap.problems;
                m_out.unresolved = snap.unresolved;
                m_out.default_parameters.resize(snap.defaults);
                goto_targets = snap.targets;
                m_claimed = snap.claimed;
                m_emitted = snap.emitted;
            }

            /// The variable a statement assigns or updates (`I = 0`, `I++`, `I += 2`): its token kind and object.
            std::optional<std::pair<TokenKind, std::int32_t>> variable_of(std::uint32_t statement_token) const
            {
                const std::uint32_t first = operand(m_script.tokens[statement_token]);
                if (first == no_token)
                {
                    return std::nullopt;
                }
                const Token& v = m_script.tokens[first];
                switch (v.kind)
                {
                case TokenKind::LocalVariable:
                case TokenKind::InstanceVariable:
                case TokenKind::OutVariable:
                case TokenKind::StateVariable:
                case TokenKind::DefaultVariable:
                    return std::pair{v.kind, v.object.value};
                default:
                    return std::nullopt;
                }
            }

            std::vector<Statement> block(std::size_t begin, std::size_t end, bool skip_first_labels)
            {
                std::vector<Statement> out;
                std::size_t i = begin;
                while (i < end)
                {
                    if (i != begin || !skip_first_labels)
                    {
                        emit_labels(i, out);
                    }
                    if (const std::size_t next = loop_at(i, end, out); next != npos)
                    {
                        i = next;
                        continue;
                    }
                    i = statement(i, end, out);
                }
                return out;
            }

            /// do-until and while(true) loops headed by statement @p i. Returns the next statement, or npos.
            std::size_t loop_at(std::size_t i, std::size_t end, std::vector<Statement>& out)
            {
                const std::uint32_t top = offset_of(i);
                // The outermost (last) backward jump to this statement closes the loop.
                std::size_t last = npos;
                for (std::size_t j = end; j-- > i + 1;)
                {
                    const Token& t = token(j);
                    if ((t.kind == TokenKind::JumpIfNot || t.kind == TokenKind::Jump) && t.word == top && !claimed(j))
                    {
                        last = j;
                        break;
                    }
                }
                if (last == npos)
                {
                    return npos;
                }
                const Token& closing = token(last);
                if (closing.kind == TokenKind::Jump)
                {
                    // `T: JumpIfNot(c) -> E; ...; Jump -> T; E:` is a while loop, handled by statement().
                    const Token& head = token(i);
                    if (head.kind == TokenKind::JumpIfNot && head.word > top && index_at(head.word) == last + 1)
                    {
                        return npos;
                    }
                }

                claim(last);
                Frame frame;
                frame.kind = Frame::Kind::Loop;
                frame.break_offset = offset_of(last + 1);
                frame.continue_offset = closing.kind == TokenKind::JumpIfNot ? offset_of(last) : top;
                m_frames.push_back(frame);
                Statement loop = make(closing.kind == TokenKind::JumpIfNot ? StatementKind::DoUntil : StatementKind::While,
                                      i);
                if (closing.kind == TokenKind::JumpIfNot)
                {
                    loop.condition = operand(closing);
                }
                loop.body = block(i, last, true);
                m_frames.pop_back();
                out.push_back(std::move(loop));
                return last + 1;
            }

            std::size_t statement(std::size_t i, std::size_t end, std::vector<Statement>& out)
            {
                const Token& t = token(i);
                if (t.kind == TokenKind::DefaultParameter)
                {
                    m_out.default_parameters.push_back(m_statements[i]);
                    return i + 1;
                }
                if (is_silent(t.kind))
                {
                    return i + 1;
                }
                switch (t.kind)
                {
                case TokenKind::JumpIfNot:
                    return if_statement(i, end, out);
                case TokenKind::Jump:
                    jump(i, out);
                    return i + 1;
                case TokenKind::Switch:
                    return switch_statement(i, end, out);
                case TokenKind::Iterator:
                case TokenKind::DynamicArrayIterator:
                    return foreach_statement(i, end, out);
                case TokenKind::FilterEditorOnly:
                {
                    const std::size_t j = index_at(t.word);
                    if (j == npos || j <= i || j > end)
                    {
                        out.push_back(comment("filtereditoronly block could not be structured", offset_of(i)));
                        ++m_out.problems;
                        return i + 1;
                    }
                    Statement s = make(StatementKind::FilterEditorOnly, i);
                    s.body = block(i + 1, j, false);
                    out.push_back(std::move(s));
                    return j;
                }
                case TokenKind::Return:
                case TokenKind::ReturnNothing:
                    out.push_back(make(StatementKind::Return, i));
                    return i + 1;
                case TokenKind::Stop:
                    out.push_back(make(StatementKind::Stop, i));
                    return i + 1;
                case TokenKind::IteratorPop:
                    return iterator_pop(i, end, out);
                case TokenKind::IteratorNext:
                    if (innermost_loop() != nullptr && innermost_loop()->kind == Frame::Kind::ForEach)
                    {
                        out.push_back(make(StatementKind::Continue, i));
                    }
                    else
                    {
                        out.push_back(comment("IteratorNext outside of a foreach", offset_of(i)));
                        ++m_out.problems;
                    }
                    return i + 1;
                case TokenKind::Case:
                    out.push_back(comment("case outside of a switch", offset_of(i)));
                    ++m_out.problems;
                    return i + 1;
                case TokenKind::Unresolved:
                    out.push_back(comment(fmt::format("unresolved token 0x{:02X}", t.opcode), offset_of(i)));
                    ++m_out.unresolved;
                    return i + 1;
                default:
                    out.push_back(make(StatementKind::Expression, i));
                    return i + 1;
                }
            }

            Frame* innermost_loop()
            {
                for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it)
                {
                    if (it->kind != Frame::Kind::Switch)
                    {
                        return &*it;
                    }
                }
                return nullptr;
            }

            const Frame* innermost_foreach() const
            {
                for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it)
                {
                    if (it->kind == Frame::Kind::ForEach)
                    {
                        return &*it;
                    }
                }
                return nullptr;
            }

            /// Whether a jump to @p target would be a `break` or `continue` of the enclosing constructs.
            bool is_loop_exit(std::uint32_t target)
            {
                if (!m_frames.empty() &&
                    (target == m_frames.back().break_offset || target == m_frames.back().alt_break_offset))
                {
                    return true;
                }
                const Frame* loop = innermost_loop();
                return loop != nullptr && (target == loop->continue_offset || target == loop->increment_offset);
            }

            void jump(std::size_t i, std::vector<Statement>& out)
            {
                const std::uint32_t target = token(i).word;
                if (!m_frames.empty() &&
                    (target == m_frames.back().break_offset || target == m_frames.back().alt_break_offset))
                {
                    out.push_back(make(StatementKind::Break, i));
                    return;
                }
                if (Frame* loop = innermost_loop(); loop != nullptr)
                {
                    if (target == loop->continue_offset)
                    {
                        out.push_back(make(StatementKind::Continue, i));
                        return;
                    }
                    if (target == loop->increment_offset)
                    {
                        loop->used_increment = true; // `continue` of a for loop: it jumps to the increment
                        out.push_back(make(StatementKind::Continue, i));
                        return;
                    }
                }
                if (target == offset_of(i + 1))
                {
                    return; // Jump to the next statement: no source.
                }
                add_goto(out, i, target);
            }

            std::size_t if_statement(std::size_t i, std::size_t end, std::vector<Statement>& out)
            {
                const Token& t = token(i);
                const std::uint32_t target = t.word;
                const std::uint32_t condition = operand(t);
                const std::size_t j = target > offset_of(i) ? index_at(target) : npos;
                if (j == npos || j > end)
                {
                    add_goto(out, i, target, condition);
                    return i + 1;
                }

                if (j > i + 1 && token(j - 1).kind == TokenKind::Jump && !claimed(j - 1))
                {
                    const std::uint32_t exit = token(j - 1).word;
                    if (exit == offset_of(i))
                    {
                        claim(j - 1);
                        loop_statement(i, j, out);
                        return j;
                    }
                    if (exit > target && exit <= offset_of(end))
                    {
                        const std::size_t k = index_at(exit);
                        // A jump to where the enclosing loop continues (or ends) may be an else-exit or a
                        // continue/break: both mean the same. `if (c) {} else B` is `if (c) continue;`, and an else
                        // reading that leaves gotos inside is dropped for the continue/break one.
                        const bool loop_exit = is_loop_exit(exit);
                        bool empty_then = true;
                        for (std::size_t m = i + 1; m + 1 < j; ++m)
                        {
                            empty_then = empty_then && is_silent(token(m).kind);
                        }
                        if (k != npos && k <= end && !(loop_exit && empty_then))
                        {
                            // if (c) A else B
                            const Snapshot before = snapshot();
                            Statement s = make(StatementKind::If, i);
                            s.condition = condition;
                            s.body = block(i + 1, j - 1, false);
                            s.else_body = block(j, k, false);
                            s.has_else = true;
                            if (!loop_exit || m_out.gotos == before.gotos)
                            {
                                out.push_back(std::move(s));
                                return k;
                            }
                            restore(before);
                        }
                    }
                }

                Statement s = make(StatementKind::If, i);
                s.condition = condition;
                s.body = block(i + 1, j, false);
                out.push_back(std::move(s));
                return j;
            }

            /// `while (c) A` or `for (init; c; increment) A`: statement @p i is the condition, @p j - 1 the jump back.
            void loop_statement(std::size_t i, std::size_t j, std::vector<Statement>& out)
            {
                const Token& head = token(i);
                const std::uint32_t condition = operand(head);
                // The last body statement is a for-loop increment if some `continue` jumps to it.
                std::size_t last = j - 1;
                while (last > i + 1 && is_silent(token(last - 1).kind))
                {
                    --last;
                }
                const std::size_t increment = last > i + 1 ? last - 1 : npos;

                const Snapshot before = snapshot();
                for (const bool allow_for : {true, false})
                {
                    Frame frame;
                    frame.kind = Frame::Kind::Loop;
                    frame.break_offset = head.word;
                    frame.continue_offset = offset_of(i);
                    if (allow_for && increment != npos)
                    {
                        frame.increment_offset = offset_of(increment);
                    }
                    m_frames.push_back(frame);
                    std::vector<Statement> body = block(i + 1, j - 1, false);
                    const bool used = m_frames.back().used_increment;
                    m_frames.pop_back();

                    if (used)
                    {
                        if (body.empty() || body.back().kind != StatementKind::Expression ||
                            body.back().offset != offset_of(increment))
                        {
                            restore(before); // The increment cannot be lifted: structure it as a while loop.
                            continue;
                        }
                        Statement loop = make(StatementKind::For, i);
                        loop.condition = condition;
                        loop.increment = body.back().token;
                        body.pop_back();
                        loop.body = std::move(body);
                        // Lift the initialization when it sets the variable the increment updates.
                        if (!out.empty() && out.back().kind == StatementKind::Expression)
                        {
                            const TokenKind kind = m_script.tokens[out.back().token].kind;
                            const auto initialized = variable_of(out.back().token);
                            if ((kind == TokenKind::Let || kind == TokenKind::LetBool) && initialized &&
                                initialized == variable_of(loop.increment))
                            {
                                loop.init = out.back().token;
                                out.pop_back();
                            }
                        }
                        out.push_back(std::move(loop));
                        return;
                    }
                    Statement loop = make(StatementKind::While, i);
                    loop.condition = condition;
                    loop.body = std::move(body);
                    out.push_back(std::move(loop));
                    return;
                }
            }

            std::size_t switch_statement(std::size_t i, std::size_t end, std::vector<Statement>& out)
            {
                std::size_t first = i + 1;
                while (first < end && token(first).kind == TokenKind::DebugInfo)
                {
                    ++first;
                }
                std::vector<std::size_t> cases;
                std::size_t switch_end = npos;
                bool has_default = false;
                for (std::size_t c = first; c < end && token(c).kind == TokenKind::Case;)
                {
                    cases.push_back(c);
                    const std::uint16_t next = token(c).word;
                    if (next == default_case)
                    {
                        has_default = true;
                        break;
                    }
                    const std::size_t n = index_at(next);
                    if (n == npos || n <= c || n > end)
                    {
                        cases.clear();
                        break;
                    }
                    if (n == end || token(n).kind != TokenKind::Case)
                    {
                        switch_end = n; // The last case's "next" is the end of a switch without default.
                        break;
                    }
                    c = n;
                }
                if (cases.empty())
                {
                    out.push_back(comment("switch could not be structured", offset_of(i)));
                    ++m_out.problems;
                    out.push_back(make(StatementKind::Expression, i));
                    return i + 1;
                }
                if (has_default)
                {
                    // The end of the switch is where the `break`s ending the case bodies jump: the jump right before
                    // the next case token. Without any, the default case runs to the end of the enclosing block.
                    const std::uint32_t last_case = offset_of(cases.back());
                    std::uint32_t furthest = 0;
                    for (std::size_t n = 0; n + 1 < cases.size(); ++n)
                    {
                        std::size_t k = cases[n + 1] - 1;
                        while (k > cases[n] && token(k).kind == TokenKind::DebugInfo)
                        {
                            --k;
                        }
                        const Token& t = token(k);
                        if (k > cases[n] && t.kind == TokenKind::Jump && t.word > last_case && t.word <= offset_of(end))
                        {
                            furthest = std::max<std::uint32_t>(furthest, t.word);
                        }
                    }
                    switch_end = furthest != 0 ? index_at(furthest) : end;
                    if (switch_end == npos || switch_end > end)
                    {
                        switch_end = end;
                    }
                }
                if (switch_end == npos)
                {
                    switch_end = end;
                }

                Frame frame;
                frame.kind = Frame::Kind::Switch;
                frame.break_offset = offset_of(switch_end);
                m_frames.push_back(frame);
                Statement s = make(StatementKind::Switch, i);
                for (std::size_t n = 0; n < cases.size(); ++n)
                {
                    const std::size_t body_end = n + 1 < cases.size() ? cases[n + 1] : switch_end;
                    Statement c = make(StatementKind::Case, cases[n]);
                    c.body = block(cases[n] + 1, body_end, false);
                    s.body.push_back(std::move(c));
                }
                m_frames.pop_back();
                // The compiler ends every switch with a default case; an empty one adds nothing to the source.
                if (has_default && s.body.size() > 1 && s.body.back().body.empty())
                {
                    s.body.pop_back();
                }
                out.push_back(std::move(s));
                return switch_end;
            }

            std::size_t foreach_statement(std::size_t i, std::size_t end, std::vector<Statement>& out)
            {
                const std::uint32_t target = token(i).word;
                const std::size_t j = index_at(target);
                if (j == npos || j <= i || j > end)
                {
                    out.push_back(comment("foreach could not be structured", offset_of(i)));
                    ++m_out.problems;
                    return i + 1;
                }
                std::size_t body_end = j;
                bool popped = false;
                if (body_end > i + 1 && token(body_end - 1).kind == TokenKind::IteratorPop)
                {
                    --body_end;
                    popped = true;
                }
                std::uint32_t continue_offset = offset_of(body_end);
                if (body_end > i + 1 && token(body_end - 1).kind == TokenKind::IteratorNext)
                {
                    --body_end;
                    continue_offset = offset_of(body_end);
                }
                std::size_t next = j;
                if (!popped && j < end && token(j).kind == TokenKind::IteratorPop)
                {
                    next = j + 1;
                }

                Frame frame;
                frame.kind = Frame::Kind::ForEach;
                frame.break_offset = target;
                frame.alt_break_offset = offset_of(next);
                frame.continue_offset = continue_offset;
                m_frames.push_back(frame);
                Statement s = make(StatementKind::ForEach, i);
                s.body = block(i + 1, body_end, false);
                m_frames.pop_back();
                out.push_back(std::move(s));
                return next;
            }

            std::size_t iterator_pop(std::size_t i, std::size_t end, std::vector<Statement>& out)
            {
                const Frame* loop = innermost_foreach();
                if (loop == nullptr)
                {
                    out.push_back(comment("IteratorPop outside of a foreach", offset_of(i)));
                    ++m_out.problems;
                    return i + 1;
                }
                if (i + 1 < end && token(i + 1).kind == TokenKind::Return)
                {
                    return i + 1; // Leaving the loop(s) by returning: the pop has no source.
                }
                if (innermost_loop() != loop)
                {
                    out.push_back(comment("IteratorPop inside a nested loop", offset_of(i)));
                    ++m_out.problems;
                    return i + 1;
                }
                out.push_back(make(StatementKind::Break, i));
                if (i + 1 < end && token(i + 1).kind == TokenKind::Jump &&
                    (token(i + 1).word == loop->break_offset || token(i + 1).word == loop->alt_break_offset))
                {
                    return i + 2;
                }
                return i + 1;
            }

            const Script& m_script;
            const StructureOptions& m_options;
            const std::set<std::uint32_t>& m_needed;
            std::uint32_t m_end;
            std::vector<std::uint32_t> m_statements;
            std::vector<std::uint32_t> m_offsets;
            std::map<std::uint32_t, std::size_t> m_index;
            std::map<std::uint32_t, std::string> m_named;
            std::set<std::size_t> m_claimed;
            std::set<std::uint32_t> m_emitted;
            std::vector<Frame> m_frames;
            StructuredScript m_out;
        };
    }

    StructuredScript structure(const Script& script, const StructureOptions& options)
    {
        // Pass 1 finds the goto targets; pass 2 emits labels at them (the structure itself does not depend on labels).
        const std::set<std::uint32_t> none;
        Structurer first(script, options, none);
        (void)first.run();
        const std::set<std::uint32_t> targets = first.goto_targets;
        Structurer second(script, options, targets);
        return second.run();
    }
}
