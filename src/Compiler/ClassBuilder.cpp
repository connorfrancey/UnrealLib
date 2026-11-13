// The class package builder: a `.uc` class (declaration, variables, functions and events, defaultproperties) to a
// script package laid out like the ones the game loads. The engine then makes the class itself when it loads the
// package: links it, binds its functions, builds its default object.
//
// Two passes: the reflection (class, properties, functions with their parameters and locals, default object) is written
// and loaded into the Linker, so the bodies can be compiled against the class they belong to; the package is then
// written again with the bytecode and the default values. Imports and names are added as the class and its bodies
// need them.
#include <UnrealLib/Compiler/Compiler.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <UnrealLib/Linker/ReflectionSchema.hpp>
#include <UnrealLib/Objects/ObjectHeader.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Writer/PackageWriter.hpp>

#include "Model.hpp"

namespace UnrealLib::Compiler
{
    namespace
    {
        // ---- Declarations ---------------------------------------------------------------------------------------------
        struct TypeDecl
        {
            std::string name;                       ///< "int", "Actor", "Vector", "array", "class"
            std::shared_ptr<TypeDecl> inner;        ///< array<inner>
            std::string meta;                       ///< class<meta>
        };
        struct VarDecl
        {
            TypeDecl type;
            std::string name;
            int dim = 1;
            std::string dim_name;                   ///< Foo[EEnum] / Foo[CONST]: sized by an enum or a const
            std::uint64_t flags = 0;
            bool editable = false;
            std::string category;
            int line = 0;
        };
        struct ParamDecl
        {
            TypeDecl type;
            std::string name;
            bool optional = false, out = false, coerce = false;
            int dim = 1;
            std::string dim_name;
            bool is_const = false;
            std::string default_value;
        };
        struct FuncDecl
        {
            std::string name;
            std::uint32_t flags = 0;
            std::optional<TypeDecl> ret;
            std::vector<ParamDecl> params;
            std::vector<VarDecl> locals;
            std::string body;
            bool has_body = false;
            int native_index = 0;                   ///< native(N)
            int line = 0;
        };
        struct DefaultLine
        {
            enum class Op { Set, Empty, Add, Remove };
            std::string name;
            int index = 0;
            bool indexed = false;                   ///< Name(i)= / Name[i]=
            Op op = Op::Set;
            std::string value;
            int line = 0;
        };
        struct SubobjectDecl
        {
            std::string name, cls;                  ///< Name=, Class= (empty: the inherited template's class)
            std::vector<DefaultLine> lines;
            std::vector<SubobjectDecl> children;    ///< Begin Object blocks inside this one
            int line = 0;
        };
        struct ConstDecl
        {
            std::string name, value;
            int line = 0;
        };
        struct EnumDecl
        {
            std::string name;
            std::vector<std::string> values;      ///< With the compiler's <Prefix>_MAX at the end
            int line = 0;
        };
        struct StructDecl
        {
            std::string name, parent;
            std::uint32_t flags = 0;              ///< StructFlag bits
            std::vector<VarDecl> vars;
            std::vector<DefaultLine> defaults;    ///< structdefaultproperties
            int line = 0;
        };
        struct StateDecl
        {
            std::string name, parent;             ///< `extends` (empty: the parent class's state of that name, if any)
            std::uint32_t flags = 0;              ///< StateFlag bits
            std::vector<std::string> ignores;
            std::vector<FuncDecl> funcs;
            std::string code;                     ///< State code (labels and statements)
            int code_line = 0;
            int line = 0;
        };
        struct ClassDecl
        {
            std::string name, parent;
            bool placeable = false, notplaceable = false, abstract_class = false;
            std::optional<std::string> config;
            std::vector<ConstDecl> consts;
            std::vector<EnumDecl> enums;
            std::vector<StructDecl> structs;
            std::vector<VarDecl> vars;
            std::vector<FuncDecl> funcs;
            std::vector<StateDecl> states;
            std::vector<DefaultLine> defaults;
            std::vector<SubobjectDecl> subobjects;
            std::string within;
            std::uint32_t class_flags = 0;          ///< ClassFlag bits from modifiers
            std::vector<std::string> hide_categories;
            std::vector<std::string> auto_expand_categories;
            std::vector<std::string> interfaces;
        };

        struct DeclError
        {
            int line;
            std::string message;
        };

        class DeclParser
        {
        public:
            explicit DeclParser(std::string_view s) : m_s(s) {}

            ClassDecl parse()
            {
                ClassDecl c;
                skip();
                const bool is_interface = word("interface");
                if (!is_interface && !word("class")) fail("expected 'class' or 'interface'");
                c.name = ident();
                if (is_interface)
                {
                    c.class_flags |= 0x4000u | 0x1u;    // Interface | Abstract
                    c.parent = "Interface";
                    if (word("extends")) c.parent = ident();
                }
                else
                {
                    if (!word("extends")) fail("expected 'extends'");
                    c.parent = ident();
                }
                if (peek() == '.') { ++m_pos; c.parent = ident(); }    // extends Package.Class
                while (skip(), peek() != ';')
                {
                    if (at_end()) fail("expected ';' after the class declaration");
                    const std::string m = lower(ident());
                    if (m == "within")
                    {
                        c.within = ident();
                        continue;
                    }
                    std::string arg;
                    if (skip(), peek() == '(') arg = balanced('(', ')');
                    static const std::map<std::string, std::uint32_t> flags = {
                        {"transient", 0x8u}, {"perobjectconfig", 0x400u}, {"editinlinenew", 0x1000u},
                        {"collapsecategories", 0x2000u}, {"deprecated", 0x2000000u}, {"hidedropdown", 0x4000000u}};
                    // Accepted, nothing to build: C++ only, editor only, or the compiler's ordering
                    static const std::set<std::string> ignored = {
                        "native", "nativereplication", "noexport", "dependson", "showcategories",
                        "autocollapsecategories", "dontsortcategories", "dontcollapsecategories", "classgroup", "inherits",
                        "nontransient", "noteditinlinenew", "dontautocollapsecategories", "forcescriptorder", "safereplace",
                        "hidden"};
                    if (m == "placeable") c.placeable = true;
                    else if (m == "notplaceable") c.notplaceable = true;
                    else if (m == "abstract") c.abstract_class = true;
                    else if (m == "config") c.config = trim(arg);
                    else if (m == "hidecategories")
                    {
                        for (std::string h : split(arg))
                            if (!(h = trim(h)).empty()) c.hide_categories.push_back(h);
                    }
                    else if (m == "autoexpandcategories")
                    {
                        for (std::string h : split(arg))
                            if (!(h = trim(h)).empty()) c.auto_expand_categories.push_back(h);
                    }
                    else if (m == "implements")
                    {
                        for (std::string h : split(arg))
                            if (!(h = trim(h)).empty()) c.interfaces.push_back(h);
                    }
                    else if (const auto f = flags.find(m); f != flags.end()) c.class_flags |= f->second;
                    else if (!ignored.count(m)) fail("unknown class modifier '" + m + "'");
                }
                ++m_pos;
                while (skip(), !at_end())
                {
                    const int line = m_line;
                    const std::size_t save = m_pos;
                    const std::string w = lower(ident());
                    if (w == "var")
                    {
                        const auto vars = var_decl(line);
                        c.vars.insert(c.vars.end(), vars.begin(), vars.end());
                    }
                    else if (w == "defaultproperties") defaults(c.defaults, &c.subobjects);
                    else if (w == "const")
                    {
                        ConstDecl k;
                        k.line = line;
                        k.name = ident();
                        expect('=');
                        k.value = trim(until(';'));
                        c.consts.push_back(std::move(k));
                    }
                    else if (w == "enum") c.enums.push_back(enum_decl(line));
                    else if (w == "struct") c.structs.push_back(struct_decl(line));
                    else if (w == "state") c.states.push_back(state_decl(line, 0));
                    else if (w == "auto" || w == "simulated")
                    {
                        // `auto state`, `simulated state`, `auto simulated state`; else a simulated function
                        std::uint32_t flags = w == "auto" ? StateFlag::Auto : StateFlag::Simulated;
                        if (word("simulated")) flags |= StateFlag::Simulated;
                        else if (word("auto")) flags |= StateFlag::Auto;
                        if (word("state")) c.states.push_back(state_decl(line, flags));
                        else if (w == "auto") fail("expected 'state' after 'auto'");
                        else
                        {
                            m_pos = save;
                            c.funcs.push_back(function(line));
                        }
                    }
                    else if (w == "cpptext" || w == "structcpptext") balanced('{', '}');   // C++ only: nothing to build
                    else if (w == "replication")
                        throw DeclError{line, "replication blocks aren't supported in built classes (single-player game)"};
                    else
                    {
                        m_pos = save;
                        c.funcs.push_back(function(line));
                    }
                }
                return c;
            }

        private:
            std::string_view m_s;
            std::size_t m_pos = 0;
            int m_line = 1;

            [[noreturn]] void fail(const std::string& m) const { throw DeclError{m_line, m}; }
            bool at_end() const { return m_pos >= m_s.size(); }
            char peek() const { return at_end() ? '\0' : m_s[m_pos]; }
            static std::string trim(std::string t)
            {
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
                return t;
            }
            void skip()
            {
                for (;;)
                {
                    while (!at_end() && std::isspace(static_cast<unsigned char>(m_s[m_pos])))
                    {
                        if (m_s[m_pos] == '\n') ++m_line;
                        ++m_pos;
                    }
                    if (m_s.substr(m_pos, 2) == "//")
                    {
                        while (!at_end() && m_s[m_pos] != '\n') ++m_pos;
                        continue;
                    }
                    if (m_s.substr(m_pos, 2) == "/*")
                    {
                        const auto end = m_s.find("*/", m_pos + 2);
                        for (std::size_t i = m_pos; i < (end == std::string_view::npos ? m_s.size() : end); ++i)
                            if (m_s[i] == '\n') ++m_line;
                        m_pos = end == std::string_view::npos ? m_s.size() : end + 2;
                        continue;
                    }
                    return;
                }
            }
            std::string ident()
            {
                skip();
                const std::size_t start = m_pos;
                while (!at_end() && (std::isalnum(static_cast<unsigned char>(m_s[m_pos])) || m_s[m_pos] == '_')) ++m_pos;
                if (start == m_pos) fail(std::string("expected a name, found '") + peek() + "'");
                return std::string(m_s.substr(start, m_pos - start));
            }
            bool word(std::string_view w)
            {
                skip();
                const std::size_t save = m_pos;
                if (at_end() || !(std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_')) return false;
                if (lower(ident()) == w) return true;
                m_pos = save;
                return false;
            }
            void expect(char c)
            {
                skip();
                if (peek() != c) fail(std::string("expected '") + c + "', found '" + peek() + "'");
                ++m_pos;
            }
            // From an opening `open` (at the cursor) to its matching `close`: the text between them
            std::string balanced(char open, char close)
            {
                expect(open);
                const std::size_t start = m_pos;
                int depth = 1;
                while (!at_end())
                {
                    const char c = m_s[m_pos];
                    if (c == '\n') ++m_line;
                    if (c == '"' || c == '\'')
                    {
                        ++m_pos;
                        while (!at_end() && m_s[m_pos] != c)
                        {
                            if (m_s[m_pos] == '\\') ++m_pos;
                            ++m_pos;
                        }
                    }
                    else if (m_s.substr(m_pos, 2) == "//")
                    {
                        while (!at_end() && m_s[m_pos] != '\n') ++m_pos;
                        continue;
                    }
                    else if (c == open) ++depth;
                    else if (c == close && --depth == 0)
                    {
                        const std::string text(m_s.substr(start, m_pos - start));
                        ++m_pos;
                        return text;
                    }
                    ++m_pos;
                }
                fail(std::string("no matching '") + close + "'");
            }

            // Raw text up to the next top-level `end` (quotes respected); the cursor after it
            std::string until(char end)
            {
                const std::size_t start = m_pos;
                while (!at_end() && m_s[m_pos] != end)
                {
                    const char c = m_s[m_pos];
                    if (c == '\n') ++m_line;
                    if (c == '"' || c == '\'')
                    {
                        ++m_pos;
                        while (!at_end() && m_s[m_pos] != c)
                        {
                            if (m_s[m_pos] == '\\') ++m_pos;
                            ++m_pos;
                        }
                    }
                    ++m_pos;
                }
                if (at_end()) fail(std::string("expected '") + end + "'");
                const std::string text(m_s.substr(start, m_pos - start));
                ++m_pos;
                return text;
            }
            void optional_semicolon()
            {
                if (skip(), peek() == ';') ++m_pos;
            }

            EnumDecl enum_decl(int line)
            {
                EnumDecl e;
                e.line = line;
                e.name = ident();
                const std::string block = balanced('{', '}');
                DeclParser sub(block);
                for (;;)
                {
                    sub.skip();
                    if (sub.at_end()) break;
                    e.values.push_back(sub.ident());
                    sub.skip();
                    if (sub.peek() == ',') ++sub.m_pos;
                    else if (!sub.at_end()) fail("expected ',' between the values of " + e.name);
                }
                if (e.values.empty()) fail("enum " + e.name + " has no values");
                if (e.values.size() > 255) fail("enum " + e.name + " has more than 255 values");
                // The compiler's last value: <Prefix>_MAX (the values' common prefix up to '_', else the enum's name)
                const std::string& last = e.values.back();
                if (!(last.size() > 4 && iequal(std::string_view(last).substr(last.size() - 4), "_MAX")))
                {
                    const auto u = e.values.front().find('_');
                    std::string prefix = u == std::string::npos ? e.name : e.values.front().substr(0, u);
                    for (const std::string& v : e.values)
                        if (v.size() <= prefix.size() + 1 || !iequal(std::string_view(v).substr(0, prefix.size() + 1), prefix + "_"))
                        {
                            prefix = e.name;
                            break;
                        }
                    e.values.push_back(prefix + "_MAX");
                }
                optional_semicolon();
                return e;
            }

            StructDecl struct_decl(int line)
            {
                StructDecl st;
                st.line = line;
                static const std::map<std::string, std::uint32_t> modifiers = {
                    {"native", 0}, {"export", 0}, {"transient", 0}, {"atomic", StructFlag::Atomic},
                    {"immutable", StructFlag::Immutable}, {"immutablewhencooked", StructFlag::ImmutableWhenCooked},
                    {"atomicwhencooked", StructFlag::AtomicWhenCooked}};
                for (;;)
                {
                    const std::string id = ident();
                    skip();
                    const std::size_t save = m_pos;
                    if (peek() == '{' || word("extends"))
                    {
                        m_pos = save;
                        st.name = id;
                        break;
                    }
                    const auto it = modifiers.find(lower(id));
                    if (it == modifiers.end()) fail("unexpected '" + id + "' in a struct declaration");
                    st.flags |= it->second;
                }
                if (word("extends"))
                {
                    st.parent = ident();
                    if (peek() == '.')
                    {
                        ++m_pos;
                        st.parent = ident();
                    }
                }
                skip();
                const int body_line = m_line;
                const std::string block = balanced('{', '}');
                DeclParser sub(block);
                sub.m_line = body_line;
                while (sub.skip(), !sub.at_end())
                {
                    const int l = sub.m_line;
                    const std::string w = lower(sub.ident());
                    if (w == "var")
                    {
                        const auto vars = sub.var_decl(l);
                        st.vars.insert(st.vars.end(), vars.begin(), vars.end());
                    }
                    else if (w == "structdefaultproperties") sub.defaults(st.defaults);
                    else if (w == "structcpptext" || w == "cpptext") sub.balanced('{', '}');
                    else throw DeclError{l, "unexpected '" + w + "' in struct " + st.name + " (structs hold variables)"};
                }
                optional_semicolon();
                return st;
            }

            StateDecl state_decl(int line, std::uint32_t flags)
            {
                StateDecl st;
                st.line = line;
                st.flags = flags;
                if (skip(), peek() == '(')
                {
                    balanced('(', ')');
                    st.flags |= StateFlag::Editable;
                }
                st.name = ident();
                if (word("extends")) st.parent = ident();
                skip();
                const int body_line = m_line;
                const std::string block = balanced('{', '}');
                DeclParser sub(block);
                sub.m_line = body_line;
                static const std::set<std::string> function_words = {
                    "function", "event", "simulated", "final", "static", "exec", "singular", "private", "protected",
                    "public", "reliable", "unreliable", "server", "client", "native", "latent", "iterator"};
                while (sub.skip(), !sub.at_end())
                {
                    const std::size_t save = sub.m_pos;
                    const int l = sub.m_line;
                    if (!(std::isalpha(static_cast<unsigned char>(sub.peek())) || sub.peek() == '_'))
                    {
                        st.code = std::string(sub.m_s.substr(save));
                        st.code_line = l;
                        break;
                    }
                    const std::string w = lower(sub.ident());
                    if (w == "ignores")
                    {
                        for (std::string n : split(sub.until(';')))
                            if (!(n = trim(n)).empty()) st.ignores.push_back(n);
                        continue;
                    }
                    sub.skip();
                    if (function_words.count(w) && sub.peek() != ':')
                    {
                        sub.m_pos = save;
                        st.funcs.push_back(sub.function(l));
                        continue;
                    }
                    // A label or a statement: the state code, to the end of the block
                    st.code = std::string(sub.m_s.substr(save));
                    st.code_line = l;
                    break;
                }
                optional_semicolon();
                return st;
            }
            static std::vector<std::string> split(const std::string& text)
            {
                std::vector<std::string> out;
                std::size_t at = 0;
                for (;;)
                {
                    const auto comma = text.find(',', at);
                    out.push_back(text.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
                    if (comma == std::string::npos) return out;
                    at = comma + 1;
                }
            }

            TypeDecl type()
            {
                TypeDecl t;
                t.name = ident();
                if (skip(), peek() == '<')
                {
                    ++m_pos;
                    if (lower(t.name) == "array") t.inner = std::make_shared<TypeDecl>(type());
                    else t.meta = ident();
                    expect('>');
                }
                return t;
            }

            std::vector<VarDecl> var_decl(int line)
            {
                VarDecl base;
                base.line = line;
                if (skip(), peek() == '(')
                {
                    base.editable = true;
                    base.category = trim(balanced('(', ')'));
                }
                for (;;)    // Modifiers before the type
                {
                    skip();
                    const std::size_t save = m_pos;
                    const std::string m = lower(ident());
                    static const std::map<std::string, std::uint64_t> modifiers = {
                        {"config", PropertyFlag::Config}, {"globalconfig", PropertyFlag::GlobalConfig | PropertyFlag::Config},
                        {"transient", PropertyFlag::Transient}, {"const", PropertyFlag::Const}, {"editconst", PropertyFlag::EditConst},
                        {"input", PropertyFlag::Input}, {"export", PropertyFlag::ExportObject}, {"editinline", PropertyFlag::EditInline},
                        {"duplicatetransient", PropertyFlag::DuplicateTransient}, {"noexport", PropertyFlag::NoExport},
                        {"native", PropertyFlag::Native}, {"private", 0}, {"protected", 0}, {"public", 0}, {"localized", PropertyFlag::Localized},
                        {"init", PropertyFlag::AlwaysInit}, {"noclear", PropertyFlag::NoClear}, {"interp", PropertyFlag::Interp},
                        {"repnotify", PropertyFlag::RepNotify}, {"deprecated", PropertyFlag::Deprecated},
                        {"instanced", PropertyFlag::ExportObject | PropertyFlag::EditInline},
                        {"editinlineuse", PropertyFlag::EditInlineUse}, {"editfixedsize", PropertyFlag::EditFixedSize},
                        {"noimport", PropertyFlag::NoImport}, {"edithide", PropertyFlag::EditHide},
                        {"edittextbox", PropertyFlag::EditTextBox}, {"editoronly", PropertyFlag::EditorOnly},
                        {"notforconsole", 0}, {"archetype", PropertyFlag::ArchetypeProperty},
                        {"crosslevelpassive", PropertyFlag::CrossLevelPassive}, {"crosslevelactive", PropertyFlag::CrossLevelActive},
                        {"repretry", 0}, {"serializetext", PropertyFlag::SerializeText},
                        {"nontransactional", PropertyFlag::NonTransactional}, {"databinding", PropertyFlag::DataBinding},
                        {"privatewrite", PropertyFlag::PrivateWrite}, {"protectedwrite", PropertyFlag::ProtectedWrite}, {"skip", 0}};
                    const auto it = modifiers.find(m);
                    if (it == modifiers.end())
                    {
                        m_pos = save;
                        break;
                    }
                    base.flags |= it->second;
                    if (skip(), peek() == '(') balanced('(', ')');    // config(...) arguments
                }
                base.type = type();
                std::vector<VarDecl> out;
                for (;;)
                {
                    VarDecl v = base;
                    v.name = ident();
                    if (skip(), peek() == '[')
                    {
                        const std::string n = trim(balanced('[', ']'));
                        if (!n.empty() && std::isdigit(static_cast<unsigned char>(n.front()))) v.dim = std::stoi(n);
                        else v.dim_name = n;      // An enum or a const: the builder knows its size
                    }
                    out.push_back(std::move(v));
                    skip();
                    if (peek() == ',') { ++m_pos; continue; }
                    expect(';');
                    return out;
                }
            }

            FuncDecl function(int line)
            {
                FuncDecl f;
                f.line = line;
                for (;;)
                {
                    const std::string m = lower(ident());
                    if (m == "function") break;
                    if (m == "event") { f.flags |= FunctionFlag::Event; break; }
                    if (m == "delegate") { f.flags |= FunctionFlag::Delegate; break; }
                    static const std::map<std::string, std::uint32_t> modifiers = {
                        {"simulated", FunctionFlag::Simulated}, {"final", FunctionFlag::Final}, {"static", FunctionFlag::Static},
                        {"exec", FunctionFlag::Exec}, {"singular", FunctionFlag::Singular}, {"private", FunctionFlag::Private},
                        {"protected", FunctionFlag::Protected}, {"public", FunctionFlag::Public},
                        {"reliable", FunctionFlag::NetReliable | FunctionFlag::Net}, {"unreliable", FunctionFlag::Net},
                        {"server", FunctionFlag::NetServer | FunctionFlag::Net}, {"client", FunctionFlag::NetClient | FunctionFlag::Net},
                        {"native", FunctionFlag::Native}, {"latent", FunctionFlag::Latent}, {"iterator", FunctionFlag::Iterator}};
                    const auto it = modifiers.find(m);
                    if (it == modifiers.end()) fail("unexpected '" + m + "' (expected a declaration)");
                    f.flags |= it->second;
                    if (skip(), peek() == '(')
                    {
                        const std::string arg = trim(balanced('(', ')'));
                        if (m == "native")    // native(N): the engine's native table slot N (a mod native's, step 11)
                        {
                            try { f.native_index = std::stoi(arg); }
                            catch (...) { fail("native(" + arg + "): the index must be a number"); }
                        }
                    }
                }
                TypeDecl first = type();
                if (skip(), peek() == '(') f.name = first.name;
                else
                {
                    f.ret = first;
                    f.name = ident();
                }
                expect('(');
                if (skip(), peek() != ')')
                    for (;;)
                    {
                        ParamDecl p;
                        for (;;)
                        {
                            skip();
                            const std::size_t save = m_pos;
                            const std::string m = lower(ident());
                            if (m == "optional") p.optional = true;
                            else if (m == "out") p.out = true;
                            else if (m == "coerce") p.coerce = true;
                            else if (m == "const") p.is_const = true;
                            else if (m == "init" || m == "skip") {}
                            else { m_pos = save; break; }
                        }
                        p.type = type();
                        p.name = ident();
                        if (skip(), peek() == '[')
                        {
                            const std::string n = trim(balanced('[', ']'));
                            if (!n.empty() && std::isdigit(static_cast<unsigned char>(n.front()))) p.dim = std::stoi(n);
                            else p.dim_name = n;
                        }
                        if (skip(), peek() == '=')    // A default value: up to the next top-level ',' or ')'
                        {
                            ++m_pos;
                            const std::size_t start = m_pos;
                            int depth = 0;
                            while (!at_end() && !(depth == 0 && (peek() == ',' || peek() == ')')))
                            {
                                if (peek() == '(') ++depth;
                                if (peek() == ')') --depth;
                                ++m_pos;
                            }
                            p.default_value = trim(std::string(m_s.substr(start, m_pos - start)));
                        }
                        f.params.push_back(std::move(p));
                        skip();
                        if (peek() == ',') { ++m_pos; continue; }
                        break;
                    }
                expect(')');
                word("const");
                skip();
                if (peek() == ';')
                {
                    ++m_pos;
                    return f;
                }
                const int body_line = m_line;
                std::string body = balanced('{', '}');
                f.has_body = true;
                locals(body, f, body_line);
                return f;
            }

            // The leading `local T a, b;` declarations of a body; the rest is the body's statements
            void locals(const std::string& body, FuncDecl& f, int line)
            {
                DeclParser sub(body);
                sub.m_line = line;
                for (;;)
                {
                    sub.skip();
                    const std::size_t save = sub.m_pos;
                    if (sub.at_end() || !(std::isalpha(static_cast<unsigned char>(sub.peek())))) { sub.m_pos = save; break; }
                    if (lower(sub.ident()) != "local") { sub.m_pos = save; break; }
                    VarDecl base;
                    base.line = sub.m_line;
                    base.type = sub.type();
                    for (;;)
                    {
                        VarDecl v = base;
                        v.name = sub.ident();
                        if (sub.skip(), sub.peek() == '[')
                        {
                            const std::string n = trim(sub.balanced('[', ']'));
                            v.dim = std::stoi(n);
                        }
                        f.locals.push_back(v);
                        sub.skip();
                        if (sub.peek() == ',') { ++sub.m_pos; continue; }
                        sub.expect(';');
                        break;
                    }
                }
                f.body = body.substr(sub.m_pos);
            }

            void defaults(std::vector<DefaultLine>& out, std::vector<SubobjectDecl>* subobjects = nullptr)
            {
                const std::string block = balanced('{', '}');
                int line = m_line;
                // Lines (the block's own count: m_line is past it now)
                for (const char c : block)
                    if (c == '\n') --line;
                std::size_t at = 0;
                std::vector<SubobjectDecl*> open;    // The Begin Object blocks around the current line
                while (at < block.size())
                {
                    std::size_t end = block.find('\n', at);
                    if (end == std::string::npos) end = block.size();
                    std::string l = trim(block.substr(at, end - at));
                    at = end + 1;
                    ++line;
                    if (l.empty() || l.rfind("//", 0) == 0) continue;
                    const std::string ll = lower(l);
                    if (ll.rfind("begin object", 0) == 0)
                    {
                        if (!subobjects) throw DeclError{line, "subobjects (Begin Object) only go in defaultproperties"};
                        SubobjectDecl d;
                        d.line = line;
                        for (const std::string& part : split_spaces(l.substr(12)))
                        {
                            const auto eq = part.find('=');
                            if (eq == std::string::npos) continue;
                            const std::string key = lower(part.substr(0, eq)), value = part.substr(eq + 1);
                            if (key == "class") d.cls = value;
                            else if (key == "name") d.name = value;
                        }
                        if (d.name.empty()) throw DeclError{line, "Begin Object without Name="};
                        auto& list = open.empty() ? *subobjects : open.back()->children;
                        list.push_back(std::move(d));
                        open.push_back(&list.back());
                        continue;
                    }
                    if (ll.rfind("end object", 0) == 0)
                    {
                        if (open.empty()) throw DeclError{line, "End Object without Begin Object"};
                        open.pop_back();
                        continue;
                    }
                    (open.empty() ? out : open.back()->lines).push_back(default_line(l, line));
                }
                if (!open.empty()) throw DeclError{open.back()->line, "Begin Object without End Object"};
            }
            static std::vector<std::string> split_spaces(const std::string& text)
            {
                std::vector<std::string> out;
                std::size_t at = 0;
                while (at < text.size())
                {
                    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
                    const std::size_t start = at;
                    while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at]))) ++at;
                    if (at > start) out.push_back(text.substr(start, at - start));
                }
                return out;
            }
            // `Name=Value`, `Name(i)=Value`, `Name[i]=Value`, `Name.Empty`, `Name.Add(Value)`, `Name.Remove(Value)`
            static DefaultLine default_line(std::string l, int line)
            {
                if (!l.empty() && l.back() == ';') l.pop_back();
                DefaultLine d;
                d.line = line;
                const auto first = l.find_first_of("=([");
                const auto dot = l.find('.');
                if (dot != std::string::npos && (first == std::string::npos || dot < first))
                {
                    d.name = trim(l.substr(0, dot));
                    const std::string rest = trim(l.substr(dot + 1));
                    const std::string op = lower(rest.substr(0, rest.find('(')));
                    const auto open = rest.find('('), close = rest.rfind(')');
                    const std::string arg = open != std::string::npos && close != std::string::npos && close > open
                                                ? trim(rest.substr(open + 1, close - open - 1)) : "";
                    if (op == "empty") d.op = DefaultLine::Op::Empty;
                    else if (op == "add") d.op = DefaultLine::Op::Add;
                    else if (op == "remove") d.op = DefaultLine::Op::Remove;
                    else throw DeclError{line, "unknown array operation in defaultproperties: " + l};
                    d.value = arg;
                    return d;
                }
                const auto eq = l.find('=');
                if (eq == std::string::npos) throw DeclError{line, "expected Name=Value in defaultproperties: " + l};
                std::string lhs = trim(l.substr(0, eq));
                d.value = trim(l.substr(eq + 1));
                const auto open = lhs.find_first_of("([");
                if (open != std::string::npos)
                {
                    try { d.index = std::stoi(lhs.substr(open + 1)); }
                    catch (...) { throw DeclError{line, "array indices in defaultproperties must be numbers: " + lhs}; }
                    d.indexed = true;
                    lhs = trim(lhs.substr(0, open));
                }
                d.name = lhs;
                return d;
            }
        };

        // ---- The package ----------------------------------------------------------------------------------------------
        constexpr std::uint64_t kClassFlags = 0x000F000400000000ull;     // As the game's script classes
        constexpr std::uint64_t kFieldFlags = 0x0007000400000000ull;
        constexpr std::uint64_t kConstFlags = 0x0007000000000000ull;
        constexpr std::uint64_t kDefaultsFlags = 0x0007000400000200ull;  // ... | ClassDefaultObject

        // The engine's probe names: FName indices 300..363 of Mirror's Edge (UE3 build 3716), read from the executable.
        // A state's (or class's) probe mask has bit (index - 300) for each of these events it defines; the engine only
        // calls a probe event (Touch, Tick, BeginState, ...) on an object whose state probes it.
        constexpr std::array<std::string_view, 64> kProbeNames = {
            "", "Destroyed", "GainedChild", "LostChild", "", "", "Trigger", "UnTrigger", "Timer", "HitWall", "Falling",
            "Landed", "PhysicsVolumeChange", "Touch", "UnTouch", "Bump", "BeginState", "EndState", "BaseChange", "Attach",
            "Detach", "", "", "", "", "", "", "EncroachingOn", "EncroachedBy", "PoppedState", "HeadVolumeChange",
            "PostTouch", "PawnEnteredVolume", "MayFall", "PushedState", "PawnLeavingVolume", "Tick", "PlayerTick",
            "ModifyVelocity", "", "SeePlayer", "EnemyNotVisible", "HearNoise", "UpdateEyeHeight", "SeeMonster", "",
            "SpecialHandling", "BotDesireability", "NotifyBump", "NotifyPhysicsVolumeChange", "", "NotifyHeadVolumeChange",
            "NotifyLanded", "NotifyHitWall", "", "PreBeginPlay", "", "PostBeginPlay", "", "PhysicsChangedFor",
            "ActorEnteredVolume", "ActorLeavingVolume", "", ""};
        std::uint64_t probe_bit(std::string_view name)
        {
            for (std::size_t i = 0; i < kProbeNames.size(); ++i)
                if (!kProbeNames[i].empty() && iequal(kProbeNames[i], name)) return 1ull << i;
            return 0;
        }

        // The two macros everybody writes: `log(...) and `warn(...), as the engine's own (Object.LogInternal, WarnInternal)
        std::string expand_macros(std::string text)
        {
            static const std::pair<std::string_view, std::string_view> macros[] = {{"`log(", "LogInternal("},
                                                                                   {"`warn(", "WarnInternal("}};
            for (const auto& [from, to] : macros)
                for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
                    text.replace(at, from.size(), to);
            return text;
        }

        struct Built
        {
            ObjectIndex index;
            FieldObject field;
            std::vector<std::byte> script;
        };

        // A body to compile once the reflection is loaded: a function's, or a state's code
        struct Unit
        {
            ObjectIndex index;
            const FuncDecl* func = nullptr;
            const StateDecl* state = nullptr;
        };

        class Builder
        {
        public:
            Builder(ScriptCompiler& compiler, ScriptCompiler::Impl& m, std::string package, BuiltPackage& out)
                : C(compiler), M(m), m_package(std::move(package)), m_out(out)
            {
            }

            void build(const ClassDecl& decl)
            {
                m_decl = &decl;
                m_parent = find_kind(decl.parent, "Class");
                if (!m_parent.valid()) throw DeclError{1, "no class " + decl.parent + " to extend"};
                const FieldObject* pf = M.field(m_parent);
                if (!pf || !pf->class_data || !pf->state) throw DeclError{1, decl.parent + "'s class data can't be read"};
                const Package& pp = *m_parent.package;
                for (const Package* p : M.scripts)
                    if (iequal(p->name(), m_package)) throw DeclError{1, "a package named " + m_package + " is already loaded"};

                // The model: the parent's package summary (version, licensee, engine), a script package's flags
                m_model.name = m_package;
                m_model.summary = pp.summary();
                m_model.summary.package_flags = 0x00200001u;        // AllowDownload | ContainsScript
                m_model.summary.name_count = 0;
                m_model.summary.export_count = 0;
                m_model.summary.import_count = 0;
                m_model.summary.generations = {GenerationInfo{}};
                const std::size_t h = std::hash<std::string>{}(m_package);
                m_model.summary.guid = Guid{static_cast<std::uint32_t>(h), static_cast<std::uint32_t>(h >> 16) ^ 0x4D697261u,
                                            static_cast<std::uint32_t>(m_package.size()) * 2654435761u, 0x67654D6Fu};
                (void)m_model.make_name("None");
                (void)m_model.make_name(m_package);
                (void)m_model.make_name("Core");

                m_class = add(kClassFlags, {}, {}, decl.name, import_of(m_parent), {});
                // The class's fields as the game's classes list them: states, functions, consts and structs newest first,
                // then the variables in declaration order, then the enums newest first
                std::vector<ObjectIndex> consts, enums, structs, vars, funcs, states;

                for (const ConstDecl& k : decl.consts)
                {
                    const ObjectIndex i = add(kConstFlags, core_class("Const"), m_class, k.name, {}, {});
                    Built& b = m_built[i.value];
                    b.field.kind = FieldKind::Const;
                    b.field.header.net_index = -1;
                    b.field.const_value = FString(k.value);
                    consts.push_back(i);
                }
                for (const EnumDecl& e : decl.enums)
                {
                    const ObjectIndex i = add(kFieldFlags, core_class("Enum"), m_class, e.name, {}, {});
                    Built& b = m_built[i.value];
                    b.field.kind = FieldKind::Enum;
                    b.field.header.net_index = -1;
                    for (const std::string& v : e.values) b.field.enum_names.push_back(m_model.make_name(v));
                    m_types[lower(e.name)] = {false, i, nullptr};
                    enums.push_back(i);
                }
                // Structs: all names first (members may use structs declared later), then their members
                for (const StructDecl& st : decl.structs)
                {
                    const ObjectIndex i = add(kFieldFlags, core_class("ScriptStruct"), m_class, st.name, {}, {});
                    m_types[lower(st.name)] = {true, i, &st};
                    structs.push_back(i);
                }
                for (const StructDecl& st : decl.structs) build_struct(st);

                // Functions before variables: a `delegate<F>` variable refers to its function
                std::vector<std::pair<FName, ObjectIndex>> class_map;
                std::uint64_t class_probe = pf->state->probe_mask;
                for (const FuncDecl& f : decl.funcs)
                {
                    const ObjectIndex i = function(f, m_class, nullptr);
                    funcs.push_back(i);
                    class_map.emplace_back(m_model.make_name(f.name), i);
                    m_class_funcs[lower(f.name)] = {i, &f};
                    class_probe |= probe_bit(f.name);
                    const bool declared = std::any_of(decl.vars.begin(), decl.vars.end(), [&](const VarDecl& v) {
                        return iequal(v.name, "__" + f.name + "__Delegate");
                    });
                    if ((f.flags & FunctionFlag::Delegate) && !declared)     // Its variable: __Name__Delegate
                    {
                        const ObjectIndex d = add(kFieldFlags, core_class("DelegateProperty"), m_class, "__" + f.name + "__Delegate", {}, {});
                        Built& b = m_built[d.value];
                        b.field.kind = FieldKind::Property;
                        b.field.header.net_index = -1;
                        PropertyData pd;
                        pd.type = PropertyType::Delegate;
                        pd.category = m_model.make_name("None");
                        pd.reference = i;
                        b.field.property = pd;
                        vars.push_back(d);
                        m_delegate_vars[lower("__" + f.name + "__Delegate")] = &f;
                    }
                }
                for (const VarDecl& v : decl.vars)
                    vars.push_back(property(v, m_class, 0, v.editable ? (v.category.empty() ? decl.name : v.category) : ""));
                // States: all names first (`extends` may name a state declared later)
                for (const StateDecl& st : decl.states)
                {
                    const ObjectIndex i = add(kFieldFlags, core_class("State"), m_class, st.name, {}, {});
                    m_states[lower(st.name)] = {i, &st};
                    states.push_back(i);
                }
                for (const StateDecl& st : decl.states) build_state(st);   // Super states first (build_state)

                m_defaults = add(kDefaultsFlags, m_class, {}, "Default__" + decl.name, {}, parent_defaults(*pf));
                for (const SubobjectDecl& sub : decl.subobjects) subobject(sub, m_defaults, {}, "");
                std::vector<ObjectIndex> children;
                for (auto* list : {&states, &funcs, &consts, &structs})
                    children.insert(children.end(), list->rbegin(), list->rend());
                children.insert(children.end(), vars.begin(), vars.end());
                children.insert(children.end(), enums.rbegin(), enums.rend());
                chain(children);

                // The class itself
                Built& c = m_built[m_class.value];
                c.field.kind = FieldKind::Class;
                c.field.header.net_index = -1;
                c.field.field.super = import_of(m_parent);
                c.field.structure = StructData{};
                c.field.structure->children = children.empty() ? ObjectIndex{} : children.front();
                c.field.structure->line = -1;
                c.field.structure->text_pos = -1;
                StateData st = *pf->state;
                st.function_map = class_map;
                st.probe_mask = class_probe;
                st.label_table_offset = 0xFFFF;
                c.field.state = st;
                ClassData cd;
                const ClassData& parent = *pf->class_data;
                // Inherited flags (Config, Transient, Localized, SafeReplace, PerObjectConfig, EditInlineNew,
                // CollapseCategories, HasInstancedProps, HasComponents), then Compiled | Parsed
                // (and Placeable, unless notplaceable)
                cd.flags = (parent.flags & 0x00A0366Cu) | 0x2u | 0x10u | decl.class_flags;
                if (decl.config) cd.flags |= 0x4u;     // Config
                if (decl.placeable) cd.flags |= 0x200u;
                if (decl.notplaceable) cd.flags &= ~0x200u;
                if (decl.abstract_class) cd.flags |= 0x1u;
                if (!decl.within.empty())
                {
                    const ObjectRef w = find_class(decl.within);
                    if (!w.valid()) throw DeclError{1, "no class " + decl.within + " for `within`"};
                    cd.within = import_of(w);
                }
                else if (!parent.within.is_null())
                    if (const auto w = M.def(pp, parent.within)) cd.within = import_of(*w);
                cd.config_name = m_model.make_name(
                    decl.config ? *decl.config : pp.names().try_to_string(parent.config_name).value_or("None"));
                for (const auto& [klass, prop] : parent.interfaces)
                {
                    const auto k = M.def(pp, klass);
                    const auto p = M.def(pp, prop);
                    if (k && p) cd.interfaces.emplace_back(import_of(*k), import_of(*p));
                }
                for (const std::string& name : decl.interfaces)
                {
                    const ObjectRef k = find_class(name);
                    const FieldObject* kf = k.valid() ? M.field(k) : nullptr;
                    if (!kf || !kf->class_data || !(kf->class_data->flags & 0x4000u)) throw DeclError{1, name + " isn't an interface"};
                    if (kf->class_data->flags & 0x80u)
                        throw DeclError{1, "implements(" + name + "): a native interface needs C++ (its vtable); script classes "
                                           "can implement script interfaces"};
                    cd.interfaces.emplace_back(import_of(k), ObjectIndex{});
                }
                // The parent's components (a Trigger's collision cylinder): the class's spawned actors instance them
                // from the parent's templates, as the game's own subclasses do
                // (ours where the class declares its own of the name)
                for (const auto& [name, templ] : parent.component_defaults)
                {
                    const std::string n = pp.names().try_to_string(name).value_or("None");
                    if (m_templates.count(lower(n))) continue;    // Ours replaces it
                    if (const auto t = M.def(pp, templ)) cd.component_defaults.emplace_back(m_model.make_name(n), import_of(*t));
                }
                for (const auto& [n, t] : m_templates)    // The default object's own components (not nested ones)
                    if (t.component && m_model.exports[t.index.export_index()].outer == m_defaults)
                        cd.component_defaults.emplace_back(m_model.make_name(t.name), t.index);
                for (const FName& hc : parent.hide_categories)
                    cd.hide_categories.push_back(m_model.make_name(pp.names().try_to_string(hc).value_or("None")));
                for (const std::string& hc : decl.hide_categories) cd.hide_categories.push_back(m_model.make_name(hc));
                for (const std::string& a : decl.auto_expand_categories) cd.auto_expand_categories.push_back(m_model.make_name(a));
                cd.default_object = m_defaults;
                c.field.class_data = cd;
            }

            // Pass 1 written and loaded: the bodies compiled against it, then everything written again
            void finish(Linker& linker)
            {
                write_fields();
                std::vector<std::byte> first = write();
                Package loaded;
                loaded.load_from_memory(std::move(first), m_package);
                const Package& p = linker.add(std::move(loaded));
                m_loaded = &p;
                M.scripts.push_back(&p);
                struct Pop
                {
                    std::vector<const Package*>& v;
                    ~Pop() { v.pop_back(); }
                } pop{M.scripts};

                const TokenMap map = TokenMap::ue3(p.summary().file_version, &p.profile());
                PackageEditor editor;
                editor.reference = [this](ObjectRef d) { return import_of(d); };
                editor.name = [this](std::string_view t) { return m_model.make_name(t); };
                for (const Unit& u : m_units)
                {
                    std::string source, what;
                    std::vector<std::string> defaults;
                    if (u.func)
                    {
                        if (!u.func->has_body && !(u.func->flags & FunctionFlag::Native)) continue;   // A native's script: its parameters
                        source = expand_macros(u.func->body);
                        what = u.func->name;
                        for (const ParamDecl& prm : u.func->params)
                            if (prm.optional) defaults.push_back(prm.default_value);
                    }
                    else
                    {
                        // State code ends with the compiler's `stop` (a state without code is `stop` alone)
                        source = expand_macros(u.state->code);
                        std::string tail = source;
                        while (!tail.empty() && std::isspace(static_cast<unsigned char>(tail.back()))) tail.pop_back();
                        if (!(tail.size() >= 5 && iequal(std::string_view(tail).substr(tail.size() - 5), "stop;"))) source += "\nstop;\n";
                        what = "state " + u.state->name;
                    }
                    const CompiledScript r = C.compile_body(source, ObjectRef{&p, u.index}, defaults, &editor);
                    if (!r.ok())
                    {
                        const int base = u.func ? u.func->line : u.state->code_line;
                        for (const std::string& e : r.errors) m_out.errors.push_back(what + ": " + e + fmt::format(" (from line {})", base));
                        continue;
                    }
                    BinaryWriter w(p.context());
                    const std::size_t memory = assemble_script(w, r.script, map);
                    Built& b = m_built[u.index.value];
                    b.script = w.take();
                    b.field.structure->script_size = static_cast<std::int32_t>(memory);
                    if (u.state) b.field.state->label_table_offset = label_table_offset(r.script, map, p);
                }
                ReflectionSchema schema(linker);
                for (const Package* s : M.scripts) schema.add_package(*s);
                defaults(schema);
                write_fields();
                m_out.bytes = write();
            }

        private:
            struct OwnType
            {
                bool is_struct = false;
                ObjectIndex index;
                const StructDecl* decl = nullptr;
            };
            struct OwnFunction
            {
                ObjectIndex index;
                const FuncDecl* decl = nullptr;
            };
            struct OwnState
            {
                ObjectIndex index;
                const StateDecl* decl = nullptr;
            };

            ScriptCompiler& C;
            ScriptCompiler::Impl& M;
            std::string m_package;
            BuiltPackage& m_out;
            PackageModel m_model;
            const ClassDecl* m_decl = nullptr;
            const Package* m_loaded = nullptr;
            ObjectRef m_parent;
            ObjectIndex m_class, m_defaults;
            std::vector<Unit> m_units;
            std::map<std::int32_t, Built> m_built;              // By export index value
            std::map<std::string, OwnType> m_types;             // Our structs and enums, by lowered name
            std::map<std::string, OwnFunction> m_class_funcs;   // Our class-level functions
            std::map<std::string, OwnState> m_states;           // Our states
            std::vector<Property> m_default_values;

            // ---- Imports ---------------------------------------------------------------------------------------------
            ObjectIndex package_import(std::string_view name)
            {
                if (const auto i = m_model.find_import("Package", ObjectIndex{}, name)) return *i;
                return m_model.add_import("Core", "Package", ObjectIndex{}, name);
            }
            // An object of another package: its import, with its outers' (added when missing)
            ObjectIndex import_of(ObjectRef d)
            {
                if (!d.valid()) return {};
                if (m_loaded && d.package == m_loaded) return d.index;   // Ours: indices as in the model
                const Package& p = *d.package;
                if (d.index.is_import())    // An object nobody exports (intrinsic): the import's own path
                {
                    const ImportEntry& e = p.imports()[d.index.import_index()];
                    const ObjectIndex outer = e.outer.is_null() ? ObjectIndex{} : import_of(ObjectRef{&p, e.outer});
                    const std::string cls = p.names().try_to_string(e.class_name).value_or("Class");
                    const std::string name = p.object_name(d.index);
                    if (const auto i = m_model.find_import(cls, outer, name)) return *i;
                    return m_model.add_import(p.names().try_to_string(e.class_package).value_or("Core"), cls, outer, name);
                }
                const ObjectIndex outer_index = p.outer(d.index);
                const ObjectIndex outer = outer_index.is_null() ? package_import(p.name()) : import_of(ObjectRef{&p, outer_index});
                const std::string cls = p.class_name(d.index);
                const std::string name = p.object_name(d.index);
                if (const auto i = m_model.find_import(cls, outer, name)) return *i;
                return m_model.add_import(class_package(d), cls, outer, name);
            }
            // The package the class of an export lives in ("Core" for Class, Function, IntProperty, ...)
            std::string class_package(ObjectRef d)
            {
                const ObjectIndex ci = d.package->exports()[d.index.export_index()].class_index;
                if (ci.is_null()) return "Core";
                if (ci.is_export()) return d.package->name();
                ObjectIndex top = ci;
                for (int guard = 0; guard < 16 && !d.package->outer(top).is_null(); ++guard) top = d.package->outer(top);
                return d.package->object_name(top);
            }
            ObjectIndex core_class(std::string_view name)
            {
                const ObjectIndex core = package_import("Core");
                if (const auto i = m_model.find_import("Class", core, name)) return *i;
                return m_model.add_import("Core", "Class", core, name);
            }

            ObjectIndex add(std::uint64_t flags, ObjectIndex cls, ObjectIndex outer, const std::string& name, ObjectIndex super,
                            ObjectIndex archetype)
            {
                ExportEntry e;
                e.class_index = cls;
                e.super_index = super;
                e.outer = outer;
                e.object_name = m_model.make_name(name);
                e.archetype = archetype;
                e.object_flags = flags;
                const ObjectIndex i = m_model.add_export(std::move(e), {});
                m_built[i.value].index = i;
                return i;
            }
            void set_super(ObjectIndex i, ObjectIndex super)
            {
                m_model.exports[i.export_index()].super_index = super;
                m_built[i.value].field.field.super = super;
            }
            void chain(const std::vector<ObjectIndex>& fields)
            {
                for (std::size_t i = 0; i < fields.size(); ++i)
                    m_built[fields[i].value].field.field.next = i + 1 < fields.size() ? fields[i + 1] : ObjectIndex{};
            }
            StructData new_structure(const std::vector<ObjectIndex>& children)
            {
                StructData s;
                s.children = children.empty() ? ObjectIndex{} : children.front();
                s.line = -1;
                s.text_pos = -1;
                return s;
            }

            // ---- Types -----------------------------------------------------------------------------------------------
            // A class, struct or enum by name among the script packages ("Vector" is Core.Object.Vector)
            ObjectRef find_kind(std::string_view name, std::string_view kind)
            {
                for (const Package* p : M.scripts)
                    for (std::size_t i = 0; i < p->exports().size(); ++i)
                    {
                        const ObjectIndex idx = ObjectIndex::from_export(i);
                        if (iequal(p->object_name(idx), name) && p->class_name(idx) == kind)
                            if (kind != "Class" || p->outer(idx).is_null()) return ObjectRef{p, idx};
                    }
                return {};
            }

            // A class by name: exported by a script package, or an intrinsic one they only import (StaticMesh, Texture2D)
            ObjectRef find_class(std::string_view name)
            {
                if (const ObjectRef c = find_kind(name, "Class"); c.valid()) return c;
                for (const Package* p : M.scripts)
                    for (std::size_t i = 0; i < p->imports().size(); ++i)
                    {
                        const ObjectIndex idx = ObjectIndex::from_import(i);
                        const ImportEntry& e = p->imports()[i];
                        if (iequal(p->object_name(idx), name) && iequal(p->names().try_to_string(e.class_name).value_or(""), "Class"))
                            return ObjectRef{p, idx};
                    }
                return {};
            }
            // A delegate function by name, in any class of the script packages
            ObjectRef find_delegate(std::string_view name)
            {
                for (const Package* p : M.scripts)
                    for (std::size_t i = 0; i < p->exports().size(); ++i)
                    {
                        const ObjectIndex idx = ObjectIndex::from_export(i);
                        if (!iequal(p->object_name(idx), name) || p->class_name(idx) != "Function") continue;
                        const ObjectRef r{p, idx};
                        if (const Function* f = M.function(r); f && f->is_delegate()) return r;
                    }
                return {};
            }
            // Whether a class derives from Core.Component
            bool is_component(ObjectRef cls)
            {
                std::optional<ObjectRef> c = cls;
                for (int guard = 0; c && c->valid() && guard < 64; ++guard)
                {
                    if (iequal(M.name_of(*c), "Component")) return true;
                    if (c->index.is_import()) return false;
                    c = M.super_of(*c);
                }
                return false;
            }

            struct Resolved
            {
                PropertyType type = PropertyType::Unknown;
                std::string class_name;              // "IntProperty"
                ObjectIndex reference, reference2;
                std::shared_ptr<TypeDecl> inner;
                bool needs_ctor = false;
                bool component = false;
                TypeDesc desc;
            };
            bool struct_needs_ctor(const StructDecl& st, int depth = 0)
            {
                if (depth > 16) return false;
                for (const VarDecl& v : st.vars)
                {
                    const std::string n = lower(v.type.name);
                    if (n == "string" || n == "array") return true;
                    if (const auto it = m_types.find(n); it != m_types.end() && it->second.is_struct && struct_needs_ctor(*it->second.decl, depth + 1))
                        return true;
                }
                return false;
            }
            Resolved resolve(const TypeDecl& t, int line)
            {
                Resolved r;
                const std::string n = lower(t.name);
                const auto simple = [&](PropertyType pt, const char* cls) {
                    r.type = pt;
                    r.class_name = cls;
                };
                if (n == "int") simple(PropertyType::Int, "IntProperty");
                else if (n == "float") simple(PropertyType::Float, "FloatProperty");
                else if (n == "bool") simple(PropertyType::Bool, "BoolProperty");
                else if (n == "byte") simple(PropertyType::Byte, "ByteProperty");
                else if (n == "name") simple(PropertyType::Name, "NameProperty");
                else if (n == "string")
                {
                    simple(PropertyType::Str, "StrProperty");
                    r.needs_ctor = true;
                }
                else if (n == "array")
                {
                    simple(PropertyType::Array, "ArrayProperty");
                    if (!t.inner) throw DeclError{line, "array without an element type"};
                    r.inner = t.inner;
                    r.needs_ctor = true;
                    r.desc.inner = std::make_shared<TypeDesc>(resolve(*t.inner, line).desc);
                }
                else if (n == "class")
                {
                    simple(PropertyType::Class, "ClassProperty");
                    r.reference = core_class("Class");
                    if (!t.meta.empty() && iequal(t.meta, m_decl->name)) r.reference2 = m_class;
                    else
                    {
                        const ObjectRef meta = find_class(t.meta.empty() ? "Object" : t.meta);
                        if (!meta.valid()) throw DeclError{line, "no class " + t.meta};
                        r.reference2 = import_of(meta);
                    }
                }
                else if (const auto own = m_types.find(n); own != m_types.end())
                {
                    if (own->second.is_struct)
                    {
                        simple(PropertyType::Struct, "StructProperty");
                        r.desc.struct_name = t.name;
                        r.needs_ctor = struct_needs_ctor(*own->second.decl);
                    }
                    else
                    {
                        simple(PropertyType::Byte, "ByteProperty");
                        r.desc.is_enum = true;
                    }
                    r.reference = own->second.index;
                }
                else if (iequal(t.name, m_decl->name))     // The class itself
                {
                    simple(PropertyType::Object, "ObjectProperty");
                    r.reference = m_class;
                }
                else if (n == "delegate")
                {
                    simple(PropertyType::Delegate, "DelegateProperty");
                    if (const auto own = m_class_funcs.find(lower(t.meta)); own != m_class_funcs.end()) r.reference = own->second.index;
                    else if (const auto f = M.member(m_parent, t.meta, [&](ObjectRef x) { return M.is_function(x); }))
                        r.reference = import_of(*f);
                    else if (const ObjectRef d = find_delegate(t.meta); d.valid())     // Another class's (an interface's)
                        r.reference = import_of(d);
                    else
                        throw DeclError{line, "no delegate " + t.meta};
                }
                else if (const ObjectRef c = find_class(t.name); c.valid())
                {
                    r.component = is_component(c);
                    const FieldObject* cf = c.index.is_export() ? M.field(c) : nullptr;
                    if (cf && cf->class_data && (cf->class_data->flags & 0x4000u)) simple(PropertyType::Interface, "InterfaceProperty");
                    else if (r.component) simple(PropertyType::Component, "ComponentProperty");
                    else simple(PropertyType::Object, "ObjectProperty");
                    r.reference = import_of(c);
                }
                else if (const ObjectRef s = find_kind(t.name, "ScriptStruct"); s.valid())
                {
                    simple(PropertyType::Struct, "StructProperty");
                    r.reference = import_of(s);
                    r.desc.struct_name = M.name_of(s);
                    for (const ObjectRef ch : M.children(s))
                        if (M.is_property(ch) && (M.property_flags(ch) & PropertyFlag::NeedCtorLink)) r.needs_ctor = true;
                }
                else if (const ObjectRef e = find_kind(t.name, "Enum"); e.valid())
                {
                    simple(PropertyType::Byte, "ByteProperty");
                    r.reference = import_of(e);
                    r.desc.is_enum = true;
                }
                else
                {
                    throw DeclError{line, "unknown type '" + t.name + "'"};
                }
                r.desc.type = r.type;
                return r;
            }

            // A property export under `owner`, and an array's inner property under it
            ObjectIndex property(const VarDecl& v, ObjectIndex owner, std::uint64_t flags, const std::string& category)
            {
                const Resolved r = resolve(v.type, v.line);
                const ObjectIndex i = add(kFieldFlags, core_class(r.class_name), owner, v.name, {}, {});
                PropertyData d;
                d.type = r.type;
                d.array_dim = v.dim;
                if (!v.dim_name.empty()) d.array_dim = static_array_size(v, d.array_enum);
                d.flags = v.flags | flags | (v.editable ? PropertyFlag::Edit : 0) | (r.needs_ctor ? PropertyFlag::NeedCtorLink : 0) |
                          (r.component ? PropertyFlag::Component : 0);
                d.category = m_model.make_name(category.empty() ? "None" : category);
                d.reference = r.reference;
                d.reference2 = r.reference2;
                if (r.inner)
                {
                    VarDecl inner;
                    inner.type = *r.inner;
                    inner.name = v.name;
                    inner.line = v.line;
                    d.reference = property(inner, i, 0, "");
                }
                Built& b = m_built[i.value];      // After the inner property: m_built may have grown
                b.field.kind = FieldKind::Property;
                b.field.header.net_index = -1;
                b.field.property = d;
                return i;
            }

            // Foo[EEnum] (the enum's values without _MAX) or Foo[CONST]
            int static_array_size(const VarDecl& v, ObjectIndex& array_enum)
            {
                const std::string n = lower(v.dim_name);
                if (const auto own = m_types.find(n); own != m_types.end() && !own->second.is_struct)
                {
                    array_enum = own->second.index;
                    for (const EnumDecl& e : m_decl->enums)
                        if (iequal(e.name, v.dim_name)) return static_cast<int>(e.values.size()) - 1;
                }
                for (const ConstDecl& k : m_decl->consts)
                    if (iequal(k.name, v.dim_name))
                    {
                        try { return std::stoi(k.value); }
                        catch (...) { throw DeclError{v.line, "const " + k.name + " isn't a number"}; }
                    }
                if (const ObjectRef e = find_kind(v.dim_name, "Enum"); e.valid())
                    if (const FieldObject* f = M.field(e))
                    {
                        array_enum = import_of(e);
                        return static_cast<int>(f->enum_names.size()) - 1;
                    }
                if (const auto k = M.member(m_parent, v.dim_name, [&](ObjectRef x) { return M.class_of(x) == "Const"; }))
                    if (const FieldObject* f = M.field(*k); f && f->const_value)
                    {
                        try { return std::stoi(f->const_value->text); }
                        catch (...) {}
                    }
                throw DeclError{v.line, "the size of " + v.name + "[" + v.dim_name + "] isn't an enum or a numeric const"};
            }

            void build_struct(const StructDecl& st)
            {
                const ObjectIndex i = m_types[lower(st.name)].index;
                ObjectIndex super;
                if (!st.parent.empty())
                {
                    if (const auto own = m_types.find(lower(st.parent)); own != m_types.end() && own->second.is_struct)
                        super = own->second.index;
                    else if (const ObjectRef s = find_kind(st.parent, "ScriptStruct"); s.valid())
                        super = import_of(s);
                    else
                        throw DeclError{st.line, "no struct " + st.parent + " for " + st.name + " to extend"};
                }
                std::vector<ObjectIndex> members;
                for (const VarDecl& v : st.vars) members.push_back(property(v, i, 0, v.editable ? st.name : ""));
                chain(members);
                set_super(i, super);
                Built& b = m_built[i.value];
                b.field.kind = FieldKind::ScriptStruct;
                b.field.header.net_index = -1;
                b.field.structure = new_structure(members);
                b.field.script_struct = ScriptStructData{};
                b.field.script_struct->flags = st.flags;
            }

            // The function `name` overrides: in the super state (when in a state), else the class's own, else the parent
            // class's. Its index and the flags a function inherits from it.
            std::pair<ObjectIndex, std::uint32_t> overridden(std::string_view name, const StateDecl* state)
            {
                // What an override takes from the function it overrides: the replication flags and exec
                const std::uint32_t inherit = FunctionFlag::Net | FunctionFlag::NetReliable | FunctionFlag::NetServer |
                                              FunctionFlag::NetClient | FunctionFlag::Exec;
                const auto parents = [&](ObjectRef scope) -> std::pair<ObjectIndex, std::uint32_t> {
                    const auto o = M.member(scope, name, [&](ObjectRef r) { return M.is_function(r); });
                    if (!o) return {};
                    const Function* f = M.function(*o);
                    return {import_of(*o), f ? f->flags & inherit : 0u};
                };
                if (state)
                {
                    // The super state chain: our states (`extends`), then the parent class's state of the name
                    std::set<std::string> seen;
                    for (const StateDecl* s = state; s && seen.insert(lower(s->name)).second;)
                    {
                        if (s != state)
                            for (const FuncDecl& f : s->funcs)
                                if (iequal(f.name, name))
                                {
                                    const ObjectIndex fi = m_state_funcs.at(&f);
                                    return {fi, m_own_flags[fi.value] & inherit};
                                }
                        if (const ObjectRef ps = parent_state(*s); ps.valid())
                            if (auto r = parents(ps); !r.first.is_null()) return r;
                        const auto next = s->parent.empty() || iequal(s->parent, s->name) ? m_states.end()
                                                                                          : m_states.find(lower(s->parent));
                        s = next == m_states.end() ? nullptr : next->second.decl;
                    }
                    if (const auto own = m_class_funcs.find(lower(name)); own != m_class_funcs.end())
                        return {own->second.index, m_own_flags[own->second.index.value] & inherit};
                }
                return parents(m_parent);
            }

            ObjectIndex function(const FuncDecl& f, ObjectIndex outer, const StateDecl* state)
            {
                const auto [super, inherited] = overridden(f.name, state);
                const ObjectIndex i = add(kFieldFlags, core_class("Function"), outer, f.name, super, {});
                if (state) m_state_funcs[&f] = i;
                m_units.push_back(Unit{i, &f, nullptr});
                std::vector<ObjectIndex> children;
                std::uint32_t flags = f.flags | FunctionFlag::Defined | inherited;
                if (!(flags & (FunctionFlag::Private | FunctionFlag::Protected))) flags |= FunctionFlag::Public;
                for (const ParamDecl& p : f.params)
                {
                    VarDecl v;
                    v.type = p.type;
                    v.name = p.name;
                    v.line = f.line;
                    v.dim = p.dim;
                    v.dim_name = p.dim_name;
                    std::uint64_t pf = PropertyFlag::Parm;
                    if (p.optional)
                    {
                        pf |= PropertyFlag::OptionalParm;
                        flags |= FunctionFlag::HasOptionalParms;
                    }
                    if (p.out)
                    {
                        pf |= PropertyFlag::OutParm;
                        flags |= FunctionFlag::HasOutParms;
                    }
                    if (p.coerce) pf |= PropertyFlag::CoerceParm;
                    if (p.is_const) pf |= PropertyFlag::Const;
                    if (!p.default_value.empty()) flags |= FunctionFlag::HasDefaults;
                    children.push_back(property(v, i, pf, ""));
                }
                if (f.ret)
                {
                    VarDecl v;
                    v.type = *f.ret;
                    v.name = "ReturnValue";
                    v.line = f.line;
                    children.push_back(property(v, i, PropertyFlag::Parm | PropertyFlag::OutParm | PropertyFlag::ReturnParm, ""));
                }
                for (const VarDecl& l : f.locals) children.push_back(property(l, i, 0, ""));
                chain(children);
                Built& b = m_built[i.value];
                b.field.kind = FieldKind::Function;
                b.field.header.net_index = -1;
                b.field.field.super = super;
                b.field.structure = new_structure(children);
                FunctionData fd;
                fd.flags = f.has_body ? flags : (flags & ~FunctionFlag::Defined);
                fd.native_token = static_cast<std::uint16_t>(f.native_index);
                m_own_flags[i.value] = fd.flags;
                fd.friendly_name = m_model.make_name(f.name);
                b.field.function = fd;
                return i;
            }

            // The parent class's state named like ours (what a state of the same name overrides)
            ObjectRef parent_state(const StateDecl& st)
            {
                const auto o = M.member(m_parent, st.name, [&](ObjectRef r) { return M.class_of(r) == "State"; });
                return o ? *o : ObjectRef{};
            }

            void build_state(const StateDecl& st)
            {
                if (!m_states_built.insert(lower(st.name)).second) return;
                // Our super state first: its functions are what this one's override
                if (!st.parent.empty())
                    if (const auto it = m_states.find(lower(st.parent)); it != m_states.end() && !iequal(st.parent, st.name))
                        build_state(*it->second.decl);
                const ObjectIndex i = m_states[lower(st.name)].index;
                // Its super state: `extends` (ours or the parent class's), else the parent class's state of its name
                ObjectIndex super;
                std::optional<StateData> base;    // The super state's data, for its probe and ignore masks
                const OwnState* own_super = nullptr;
                if (!st.parent.empty())
                {
                    if (const auto it = m_states.find(lower(st.parent)); it != m_states.end() && !iequal(st.parent, st.name))
                        own_super = &it->second;
                    else if (const auto o = M.member(m_parent, st.parent, [&](ObjectRef r) { return M.class_of(r) == "State"; }))
                    {
                        super = import_of(*o);
                        if (const FieldObject* f = M.field(*o); f && f->state) base = f->state;
                    }
                    else
                        throw DeclError{st.line, "no state " + st.parent + " for state " + st.name + " to extend"};
                }
                else if (const ObjectRef ps = parent_state(st); ps.valid())
                {
                    super = import_of(ps);
                    if (const FieldObject* f = M.field(ps); f && f->state) base = f->state;
                }
                if (own_super) super = own_super->index;

                std::vector<ObjectIndex> funcs;
                std::vector<std::pair<FName, ObjectIndex>> map;
                std::uint64_t probe = base ? base->probe_mask : 0;
                std::uint64_t ignore = base ? base->ignore_mask.value_or(~0ull) : ~0ull;
                for (const FuncDecl& f : st.funcs)
                {
                    const ObjectIndex fi = function(f, i, &st);
                    funcs.push_back(fi);
                    map.emplace_back(m_model.make_name(f.name), fi);
                    probe |= probe_bit(f.name);
                }
                for (const std::string& n : st.ignores) ignore &= ~probe_bit(n);
                std::reverse(funcs.begin(), funcs.end());    // Newest first, as the game's states list them
                chain(funcs);
                set_super(i, super);
                Built& b = m_built[i.value];
                b.field.kind = FieldKind::State;
                b.field.header.net_index = -1;
                b.field.structure = new_structure(funcs);
                StateData sd;
                sd.probe_mask = probe;
                sd.ignore_mask = ignore;
                sd.label_table_offset = 0xFFFF;
                sd.flags = st.flags;
                sd.function_map = map;
                b.field.state = sd;
                m_units.push_back(Unit{i, nullptr, &st});
            }

            // The memory offset of a state's LabelTable (0xFFFF: no labels): the size of what comes before it
            static std::uint16_t label_table_offset(const Script& script, const TokenMap& map, const Package& p)
            {
                for (std::size_t s = 0; s < script.statements.size(); ++s)
                    if (script.tokens[script.statements[s]].kind == TokenKind::LabelTable)
                    {
                        Script head = script;
                        head.statements.resize(s);
                        BinaryWriter w(p.context());
                        return static_cast<std::uint16_t>(assemble_script(w, head, map));
                    }
                return 0xFFFF;
            }

            // The parent classes' template of a name: an export under one of their default objects (nearest first)
            ObjectRef inherited_template(std::string_view name)
            {
                std::optional<ObjectRef> c = m_parent;
                for (int guard = 0; c && c->valid() && guard < 64; ++guard)
                {
                    const FieldObject* f = M.field(*c);
                    if (!f || !f->class_data) break;
                    if (const auto cdo = M.def(*c->package, f->class_data->default_object); cdo && cdo->index.is_export())
                    {
                        const Package& p = *cdo->package;
                        for (std::size_t i = 0; i < p.exports().size(); ++i)
                            if (p.exports()[i].outer == cdo->index && iequal(p.object_name(ObjectIndex::from_export(i)), name))
                                return ObjectRef{&p, ObjectIndex::from_export(i)};
                    }
                    c = M.super_of(*c);
                }
                return {};
            }
            ObjectRef class_of_export(ObjectRef e)
            {
                const ObjectIndex ci = e.package->exports()[e.index.export_index()].class_index;
                return M.def(*e.package, ci).value_or(ObjectRef{});
            }

            struct Template
            {
                std::string name;
                ObjectIndex index;
                ObjectRef cls;
                bool component = false;
                const SubobjectDecl* decl = nullptr;
            };
            std::map<std::string, Template> m_templates;   // Our subobjects, by lowered path ("Outer.Name" when nested)
            const Template* template_at(ObjectIndex i) const
            {
                for (const auto& [k, x] : m_templates)
                    if (x.index == i) return &x;
                return nullptr;
            }

            // An export named `name` directly under `outer` (another package's), if any
            static ObjectRef child_named(ObjectRef outer, std::string_view name)
            {
                if (!outer.valid() || !outer.index.is_export()) return {};
                const Package& p = *outer.package;
                for (std::size_t i = 0; i < p.exports().size(); ++i)
                    if (p.exports()[i].outer == outer.index && iequal(p.object_name(ObjectIndex::from_export(i)), name))
                        return ObjectRef{&p, ObjectIndex::from_export(i)};
                return {};
            }

            // `Begin Object Class=X Name=N`: a template under the default object (or under the template around it); it
            // overrides the parent's of the name
            void subobject(const SubobjectDecl& sub, ObjectIndex outer, ObjectRef outer_base, const std::string& prefix)
            {
                const ObjectRef base = outer == m_defaults ? inherited_template(sub.name) : child_named(outer_base, sub.name);
                ObjectRef cls = sub.cls.empty() ? ObjectRef{} : find_class(sub.cls);
                if (!sub.cls.empty() && !cls.valid()) throw DeclError{sub.line, "no class " + sub.cls + " for subobject " + sub.name};
                if (!cls.valid() && base.valid()) cls = class_of_export(base);
                if (!cls.valid()) throw DeclError{sub.line, "subobject " + sub.name + " needs Class= (no inherited one of the name)"};
                const std::string key = lower(prefix + sub.name);
                if (m_templates.count(key)) throw DeclError{sub.line, "two subobjects named " + sub.name};
                const ObjectIndex i = add(kFieldFlags, import_of(cls), outer, sub.name, {}, base.valid() ? import_of(base) : ObjectIndex{});
                m_templates[key] = Template{sub.name, i, cls, is_component(cls), &sub};
                for (const SubobjectDecl& child : sub.children) subobject(child, i, base, key + ".");
            }

            ObjectIndex parent_defaults(const FieldObject& parent)
            {
                if (const auto d = M.def(*m_parent.package, parent.class_data->default_object)) return import_of(*d);
                return {};
            }

            // ---- defaultproperties and structdefaultproperties ------------------------------------------------------
            PropertyParseOptions parse_options(const PropertySchema& schema, const std::string& scope = "")
            {
                PropertyParseOptions options;
                options.make_name = [this](std::string_view t) { return m_model.make_name(t); };
                options.resolve_object = [this, scope](std::string_view text) -> std::optional<ObjectIndex> {
                    std::string_view path = text;
                    if (const auto q = text.find('\''); q != std::string_view::npos && text.back() == '\'')
                        path = text.substr(q + 1, text.size() - q - 2);
                    // A subobject by its name: ours, else the parent classes'
                    if (path.find('.') == std::string_view::npos)
                    {
                        // Inside a subobject: its own subobjects first
                        if (!scope.empty())
                            if (const auto t = m_templates.find(lower(scope + std::string(path))); t != m_templates.end())
                                return t->second.index;
                        if (const auto t = m_templates.find(lower(path)); t != m_templates.end()) return t->second.index;
                        for (const auto& [k, t] : m_templates)
                            if (iequal(t.name, path)) return t.index;
                        if (const ObjectRef t = inherited_template(path); t.valid()) return import_of(t);
                    }
                    if (const auto d = M.find_path(path)) return import_of(*d);
                    return std::nullopt;
                };
                options.schema = &schema;
                return options;
            }

            // ---- Values of other packages, in ours: names, objects (a parent's template we override: ours) --------------
            ObjectIndex remap(const Package& src, ObjectIndex i)
            {
                if (i.is_null()) return {};
                const auto d = M.def(src, i);
                if (!d) return {};
                if (d->index.is_export())
                {
                    const ObjectIndex outer = d->package->outer(d->index);
                    if (!outer.is_null() && d->package->object_name(outer).rfind("Default__", 0) == 0)
                        if (const auto t = m_templates.find(lower(d->package->object_name(d->index))); t != m_templates.end())
                            return t->second.index;
                }
                return import_of(*d);
            }
            FName remap(const Package& src, const FName& n) { return m_model.make_name(src.names().try_to_string(n).value_or("None")); }
            void remap(const Package& src, PropertyValue& v)
            {
                std::visit(
                    [&](auto& x) {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, EnumValue> || std::is_same_v<T, NameValue>) x.value = remap(src, x.value);
                        else if constexpr (std::is_same_v<T, ObjectValue>) x.value = remap(src, x.value);
                        else if constexpr (std::is_same_v<T, DelegateValue>)
                        {
                            x.object = remap(src, x.object);
                            x.function = remap(src, x.function);
                        }
                        else if constexpr (std::is_same_v<T, StructValue>)
                        {
                            for (Property& f : x.fields) remap(src, f);
                        }
                        else if constexpr (std::is_same_v<T, ArrayValue>)
                        {
                            for (PropertyValue& e : x.elements) remap(src, e);
                        }
                    },
                    v.value);
            }
            void remap(const Package& src, Property& p)
            {
                remap(src, p.value);
                if (p.tag)
                {
                    p.tag->name = remap(src, p.tag->name);
                    p.tag->type_name = remap(src, p.tag->type_name);
                    if (p.tag->struct_name) p.tag->struct_name = remap(src, *p.tag->struct_name);
                    if (p.tag->enum_name) p.tag->enum_name = remap(src, *p.tag->enum_name);
                }
            }
            // The parent classes' default values (nearest first wins), in our package
            const std::vector<Property>& inherited(const PropertySchema& schema)
            {
                if (m_inherited_read) return m_inherited;
                m_inherited_read = true;
                std::set<std::string> seen;
                std::optional<ObjectRef> c = m_parent;
                for (int guard = 0; c && c->valid() && guard < 64; ++guard)
                {
                    const FieldObject* f = M.field(*c);
                    if (!f || !f->class_data) break;
                    if (const auto cdo = M.def(*c->package, f->class_data->default_object); cdo && cdo->index.is_export())
                    {
                        try
                        {
                            ObjectProperties props = M.linker.read_properties(*cdo, &schema);
                            for (Property& p : props.properties)
                            {
                                const std::string key = lower(p.name) + "#" + std::to_string(p.array_index);
                                if (!seen.insert(key).second) continue;
                                remap(*cdo->package, p);
                                m_inherited.push_back(std::move(p));
                            }
                        }
                        catch (const std::exception&)
                        {
                        }
                    }
                    c = M.super_of(*c);
                }
                return m_inherited;
            }
            bool m_inherited_read = false;
            std::vector<Property> m_inherited;

            // A property's type: ours (a variable of the class or struct) or inherited (`inherited_from`'s)
            std::optional<std::pair<std::string, TypeDesc>> property_desc(const std::string& name, const std::vector<VarDecl>& own,
                                                                          ObjectRef inherited_from, int line)
            {
                for (const VarDecl& v : own)
                    if (iequal(v.name, name)) return std::pair{v.name, resolve(v.type, line).desc};
                if (&own == &m_decl->vars)
                    if (const auto d = m_delegate_vars.find(lower(name)); d != m_delegate_vars.end())
                    {
                        TypeDesc t;
                        t.type = PropertyType::Delegate;
                        return std::pair{"__" + d->second->name + "__Delegate", t};
                    }
                if (inherited_from.valid())
                    if (const auto p = M.member(inherited_from, name, [&](ObjectRef r) { return M.is_property(r); }))
                        return std::pair{M.name_of(*p), type_desc(*p)};
                return std::nullopt;
            }

            // defaultproperties lines to tagged properties. Dynamic arrays are whole values: `Arr(i)=`, `.Empty`, `.Add()`
            // and `.Remove()` work on the inherited array (`inherited`, may be null) and give the result.
            std::vector<Property> values(const std::vector<DefaultLine>& lines, const std::vector<VarDecl>& own, ObjectRef inherited_from,
                                         const PropertyParseOptions& options, const std::vector<Property>* inherited_values)
            {
                std::vector<Property> out;
                std::map<std::string, std::size_t> arrays;    // Lowered name -> its Property in `out`
                for (const DefaultLine& d : lines)
                {
                    const auto pd = property_desc(d.name, own, inherited_from, d.line);
                    if (!pd)
                    {
                        m_out.errors.push_back(fmt::format("line {}: no property {} to give a default value", d.line, d.name));
                        continue;
                    }
                    const auto& [name, desc] = *pd;
                    try
                    {
                        if (desc.type == PropertyType::Array)
                        {
                            const TypeDesc inner = desc.inner ? *desc.inner : TypeDesc{};
                            auto it = arrays.find(lower(name));
                            if (it == arrays.end())
                            {
                                Property prop;
                                prop.name = name;
                                prop.value.value = ArrayValue{};
                                if (inherited_values)
                                    for (const Property& p : *inherited_values)
                                        if (iequal(p.name, name) && std::holds_alternative<ArrayValue>(p.value.value)) prop.value = p.value;
                                prop.tag = make_property_tag(name, 0, desc, options.make_name);
                                out.push_back(std::move(prop));
                                it = arrays.emplace(lower(name), out.size() - 1).first;
                            }
                            auto& elements = std::get<ArrayValue>(out[it->second].value.value).elements;
                            switch (d.op)
                            {
                            case DefaultLine::Op::Empty: elements.clear(); break;
                            case DefaultLine::Op::Add: elements.push_back(parse_property_value(d.value, inner, options)); break;
                            case DefaultLine::Op::Remove:
                            {
                                const PropertyValue gone = parse_property_value(d.value, inner, options);
                                elements.erase(std::remove(elements.begin(), elements.end(), gone), elements.end());
                                break;
                            }
                            case DefaultLine::Op::Set:
                                if (!d.indexed)
                                {
                                    out[it->second].value = parse_property_value(d.value, desc, options);
                                    break;
                                }
                                while (elements.size() <= static_cast<std::size_t>(d.index))
                                    elements.push_back(default_property_value(inner, options));
                                elements[static_cast<std::size_t>(d.index)] = parse_property_value(d.value, inner, options);
                                break;
                            }
                            continue;
                        }
                        if (d.op != DefaultLine::Op::Set) throw std::runtime_error(name + " isn't a dynamic array");
                        Property prop;
                        prop.name = name;
                        prop.array_index = d.index;
                        prop.value = parse_property_value(d.value, desc, options);
                        prop.tag = make_property_tag(name, d.index, desc, options.make_name);
                        std::erase_if(out, [&](const Property& p) { return iequal(p.name, name) && p.array_index == d.index; });
                        out.push_back(std::move(prop));
                    }
                    catch (const std::exception& e)
                    {
                        m_out.errors.push_back(fmt::format("line {}: {} = {}: {}", d.line, d.name, d.value, e.what()));
                    }
                }
                // The count written is the array's own field: what the edits left
                for (const auto& [n, at] : arrays)
                    if (auto* a = std::get_if<ArrayValue>(&out[at].value.value); a && !a->raw)
                        a->count = static_cast<std::int32_t>(a->elements.size());
                return out;
            }   // By template, in m_templates order
            void defaults(const PropertySchema& schema)
            {
                const PropertyParseOptions options = parse_options(schema);
                const std::vector<Property>& base = inherited(schema);
                m_default_values = values(m_decl->defaults, m_decl->vars, m_parent, options, &base);
                // Inherited values that point at a parent's template we override (Components, CollisionComponent):
                // given again, pointing at ours, unless the class sets them itself
                if (!m_templates.empty())
                {
                    for (const Property& p : base)
                    {
                        if (std::any_of(m_default_values.begin(), m_default_values.end(),
                                        [&](const Property& q) { return iequal(q.name, p.name) && q.array_index == p.array_index; }))
                            continue;
                        if (references_template(p.value)) m_default_values.push_back(p);
                    }
                }
                for (const StructDecl& st : m_decl->structs)
                {
                    ObjectRef parent;
                    if (!st.parent.empty() && !m_types.count(lower(st.parent))) parent = find_kind(st.parent, "ScriptStruct");
                    m_built[m_types[lower(st.name)].index.value].field.script_struct->defaults =
                        values(st.defaults, st.vars, parent, options, nullptr);
                }
                for (auto& [key, t] : m_templates)
                    m_template_props[t.index.value] = values(t.decl->lines, {}, t.cls, parse_options(schema, key + "."), nullptr);
            }
            bool references_template(const PropertyValue& v)
            {
                bool found = false;
                std::visit(
                    [&](const auto& x) {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, ObjectValue>)
                        {
                            for (const auto& [k, t] : m_templates)
                                if (x.value == t.index) found = true;
                        }
                        else if constexpr (std::is_same_v<T, StructValue>)
                        {
                            for (const Property& f : x.fields) found = found || references_template(f.value);
                        }
                        else if constexpr (std::is_same_v<T, ArrayValue>)
                        {
                            for (const PropertyValue& e : x.elements) found = found || references_template(e);
                        }
                    },
                    v.value);
                return found;
            }
            std::map<std::int32_t, std::vector<Property>> m_template_props;   // Template export -> its properties
            TypeDesc type_desc(ObjectRef prop)
            {
                TypeDesc t;
                const FieldObject* f = M.field(prop);
                if (!f || !f->property) return t;
                t.type = f->property->type;
                if (t.type == PropertyType::Struct)
                    if (const auto s = M.def(*prop.package, f->property->reference)) t.struct_name = M.name_of(*s);
                if (t.type == PropertyType::Byte) t.is_enum = !f->property->reference.is_null();
                if (t.type == PropertyType::Array)
                    if (const auto in = M.def(*prop.package, f->property->reference)) t.inner = std::make_shared<TypeDesc>(type_desc(*in));
                return t;
            }

            // ---- Writing ---------------------------------------------------------------------------------------------
            void write_fields()
            {
                const SerializationContext ctx = m_model.summary.context();
                const FName none = m_model.make_name("None");
                FieldWriteOptions options;
                options.none = none;
                for (auto& [value, b] : m_built)
                {
                    BinaryWriter w(ctx);
                    const ExportEntry& e = m_model.exports[b.index.export_index()];
                    ObjectHeaderInput input;
                    input.object_flags = e.object_flags;
                    input.is_class = e.class_index.is_null();
                    if (b.index.value == m_defaults.value)
                    {
                        input.is_class_default_template = true;
                        ObjectHeader header;
                        header.net_index = -1;
                        write_object_header(w, header, input);
                        write_properties(w, m_default_values, none);
                    }
                    else if (const Template* t = template_at(b.index))    // A subobject template
                    {
                        input.is_class_default_template = true;
                        input.is_component = t && t->component;
                        ObjectHeader header;
                        header.net_index = -1;
                        if (input.is_component) header.component = ComponentTemplate{ObjectIndex{}, m_model.make_name(t->name)};
                        write_object_header(w, header, input);
                        const auto props = m_template_props.find(b.index.value);
                        write_properties(w, props == m_template_props.end() ? std::vector<Property>{} : props->second, none);
                    }
                    else
                    {
                        write_field(w, b.field, b.script, input, options);
                    }
                    m_model.export_data[b.index.export_index()] = w.take();
                }
            }
            std::vector<std::byte> write()
            {
                m_model.summary.generations.back().net_object_count = static_cast<std::int32_t>(m_model.exports.size());
                return write_package(m_model);
            }

            std::map<const FuncDecl*, ObjectIndex> m_state_funcs;   // A state's functions, by declaration
            std::map<std::int32_t, std::uint32_t> m_own_flags;      // Our functions' flags, by export index value
            std::map<std::string, const FuncDecl*> m_delegate_vars; // __Name__Delegate -> its delegate
            std::set<std::string> m_states_built;
        };
    }

    BuiltPackage ScriptCompiler::build_class_package(std::string_view package_name, std::string_view source)
    {
        BuiltPackage out;
        try
        {
            // A delegate's hidden variable as the decompiler prints it (`// var delegate<F> __F__Delegate;`) marks where it
            // goes among the variables: declared there
            std::string text(source);
            for (std::size_t at = text.find("// var delegate<"); at != std::string::npos; at = text.find("// var delegate<", at))
            {
                const std::size_t end = text.find('\n', at);
                const std::string line = text.substr(at + 3, (end == std::string::npos ? text.size() : end) - at - 3);
                if (line.find("> __") != std::string::npos && line.find("__Delegate;") != std::string::npos) text.replace(at, 3, "");
                else at += 3;
            }
            DeclParser parser(text);
            const ClassDecl decl = parser.parse();
            Builder builder(*this, *m_impl, std::string(package_name), out);
            builder.build(decl);
            builder.finish(m_impl->linker);
        }
        catch (const DeclError& e)
        {
            out.errors.push_back(fmt::format("line {}: {}", e.line, e.message));
        }
        catch (const std::exception& e)
        {
            out.errors.push_back(e.what());
        }
        if (!out.errors.empty()) out.bytes.clear();
        return out;
    }
}
