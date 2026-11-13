// The class package builder: a `.uc` class (declaration, variables, functions and events, defaultproperties) to a
// script package laid out like the ones the game loads. The engine then makes the class itself when it loads the
// package: links it, binds its functions, builds its default object.
//
// Two passes: the reflection (class, properties, functions with their parameters and locals, default object) is written
// and loaded into the Linker, so the bodies can be compiled against the class they belong to; the package is then
// written again with the bytecode and the default values. Imports and names are added as the class and its bodies
// need them.
#include <UnrealLib/Compiler/Compiler.hpp>

#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

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
            int line = 0;
        };
        struct DefaultLine
        {
            std::string name;
            int index = 0;
            std::string value;
            int line = 0;
        };
        struct ClassDecl
        {
            std::string name, parent;
            bool placeable = false, notplaceable = false, abstract_class = false;
            std::optional<std::string> config;
            std::vector<VarDecl> vars;
            std::vector<FuncDecl> funcs;
            std::vector<DefaultLine> defaults;
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
                if (!word("class")) fail("expected 'class'");
                c.name = ident();
                if (!word("extends")) fail("expected 'extends'");
                c.parent = ident();
                if (peek() == '.') { ++m_pos; c.parent = ident(); }    // extends Package.Class
                while (skip(), peek() != ';')
                {
                    if (at_end()) fail("expected ';' after the class declaration");
                    const std::string m = lower(ident());
                    std::string arg;
                    if (skip(), peek() == '(') arg = balanced('(', ')');
                    if (m == "placeable") c.placeable = true;
                    else if (m == "notplaceable") c.notplaceable = true;
                    else if (m == "abstract") c.abstract_class = true;
                    else if (m == "config") c.config = trim(arg);
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
                    else if (w == "defaultproperties") defaults(c);
                    else if (w == "struct" || w == "enum" || w == "const" || w == "state" || w == "replication" ||
                             w == "cpptext" || w == "structdefaultproperties")
                        throw DeclError{line, "'" + w + "' isn't supported in built classes yet"};
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
                        {"repnotify", PropertyFlag::RepNotify}, {"deprecated", PropertyFlag::Deprecated}};
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
                        try { v.dim = std::stoi(n); } catch (...) { fail("static array sizes must be numbers here: " + n); }
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
                    static const std::map<std::string, std::uint32_t> modifiers = {
                        {"simulated", FunctionFlag::Simulated}, {"final", FunctionFlag::Final}, {"static", FunctionFlag::Static},
                        {"exec", FunctionFlag::Exec}, {"singular", FunctionFlag::Singular}, {"private", FunctionFlag::Private},
                        {"protected", FunctionFlag::Protected}, {"public", FunctionFlag::Public}, {"reliable", FunctionFlag::NetReliable},
                        {"unreliable", 0}, {"server", FunctionFlag::NetServer}, {"client", FunctionFlag::NetClient},
                        {"native", FunctionFlag::Native}, {"latent", FunctionFlag::Latent}, {"iterator", FunctionFlag::Iterator}};
                    const auto it = modifiers.find(m);
                    if (it == modifiers.end()) fail("unexpected '" + m + "' (expected a declaration)");
                    f.flags |= it->second;
                    if (skip(), peek() == '(') balanced('(', ')');
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
                            else if (m == "const" || m == "init" || m == "skip") {}
                            else { m_pos = save; break; }
                        }
                        p.type = type();
                        p.name = ident();
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

            void defaults(ClassDecl& c)
            {
                const std::string block = balanced('{', '}');
                int line = m_line;
                std::size_t at = 0;
                while (at < block.size())
                {
                    std::size_t end = block.find('\n', at);
                    if (end == std::string::npos) end = block.size();
                    std::string l = trim(block.substr(at, end - at));
                    at = end + 1;
                    ++line;
                    if (l.empty() || l.rfind("//", 0) == 0) continue;
                    if (l.back() == ';') l.pop_back();
                    const auto eq = l.find('=');
                    if (eq == std::string::npos) throw DeclError{line, "expected Name=Value in defaultproperties: " + l};
                    DefaultLine d;
                    d.line = line;
                    std::string lhs = trim(l.substr(0, eq));
                    d.value = trim(l.substr(eq + 1));
                    const auto open = lhs.find_first_of("([");
                    if (open != std::string::npos)
                    {
                        d.index = std::stoi(lhs.substr(open + 1));
                        lhs = trim(lhs.substr(0, open));
                    }
                    if (lower(lhs) == "begin object") throw DeclError{line, "subobjects (Begin Object) aren't supported in built classes yet"};
                    d.name = lhs;
                    c.defaults.push_back(std::move(d));
                }
            }
        };

        // ---- The package ----------------------------------------------------------------------------------------------
        constexpr std::uint64_t kClassFlags = 0x000F000400000000ull;     // As the game's script classes
        constexpr std::uint64_t kFieldFlags = 0x0007000400000000ull;
        constexpr std::uint64_t kDefaultsFlags = 0x0007000400000200ull;  // ... | ClassDefaultObject

        struct Built
        {
            ObjectIndex index;
            FieldObject field;
            std::vector<std::byte> script;
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

                // Exports: the class, its variables, its functions (parameters, return value, locals), its defaults
                m_class = add(kClassFlags, {}, {}, decl.name, import_of(m_parent), {});
                std::vector<ObjectIndex> class_children;
                for (const VarDecl& v : decl.vars) class_children.push_back(property(v, m_class, 0, v.editable ? (v.category.empty() ? decl.name : v.category) : ""));
                for (const FuncDecl& f : decl.funcs) class_children.push_back(function(f));
                m_defaults = add(kDefaultsFlags, m_class, {}, "Default__" + decl.name, {}, parent_defaults(*pf));
                chain(class_children);

                // The class itself
                Built& c = m_built[m_class.value];
                c.field.kind = FieldKind::Class;
                c.field.header.net_index = -1;
                c.field.field.super = import_of(m_parent);
                c.field.structure = StructData{};
                c.field.structure->children = class_children.empty() ? ObjectIndex{} : class_children.front();
                c.field.structure->line = -1;
                c.field.structure->text_pos = -1;
                StateData st = *pf->state;
                st.function_map.clear();
                for (std::size_t i = 0; i < decl.funcs.size(); ++i)
                    st.function_map.emplace_back(m_model.make_name(decl.funcs[i].name), m_functions[i]);
                st.label_table_offset = 0xFFFF;
                c.field.state = st;
                ClassData cd;
                const ClassData& parent = *pf->class_data;
                // Inherited flags (Config, Transient, Localized, SafeReplace, PerObjectConfig, EditInlineNew,
                // CollapseCategories, HasInstancedProps, HasComponents), then Compiled | Parsed
                cd.flags = (parent.flags & 0x00A0346Cu) | 0x2u | 0x10u;
                if (decl.placeable) cd.flags |= 0x200u;
                if (decl.notplaceable) cd.flags &= ~0x200u;
                if (decl.abstract_class) cd.flags |= 0x1u;
                if (!parent.within.is_null())
                    if (const auto w = M.def(pp, parent.within)) cd.within = import_of(*w);
                cd.config_name = m_model.make_name(
                    decl.config ? *decl.config : pp.names().try_to_string(parent.config_name).value_or("None"));
                for (const auto& [klass, prop] : parent.interfaces)
                {
                    const auto k = M.def(pp, klass);
                    const auto p = M.def(pp, prop);
                    if (k && p) cd.interfaces.emplace_back(import_of(*k), import_of(*p));
                }
                // The parent's components (a Trigger's collision cylinder): the class's spawned actors instance them
                // from the parent's templates, as the game's own subclasses do
                for (const auto& [name, templ] : parent.component_defaults)
                    if (const auto t = M.def(pp, templ))
                        cd.component_defaults.emplace_back(m_model.make_name(pp.names().try_to_string(name).value_or("None")), import_of(*t));
                for (const FName& h : parent.hide_categories)
                    cd.hide_categories.push_back(m_model.make_name(pp.names().try_to_string(h).value_or("None")));
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
                for (std::size_t i = 0; i < m_decl->funcs.size(); ++i)
                {
                    const FuncDecl& f = m_decl->funcs[i];
                    if (!f.has_body) continue;
                    std::vector<std::string> defaults;
                    for (const ParamDecl& prm : f.params)
                        if (prm.optional) defaults.push_back(prm.default_value);
                    const CompiledScript r = C.compile_body(f.body, ObjectRef{&p, m_functions[i]}, defaults, &editor);
                    if (!r.ok())
                    {
                        for (const std::string& e : r.errors) m_out.errors.push_back(f.name + ": " + e);
                        continue;
                    }
                    BinaryWriter w(p.context());
                    const std::size_t memory = assemble_script(w, r.script, map);
                    Built& b = m_built[m_functions[i].value];
                    b.script = w.take();
                    b.field.structure->script_size = static_cast<std::int32_t>(memory);
                }
                defaults();
                write_fields();
                m_out.bytes = write();
            }

        private:
            ScriptCompiler& C;
            ScriptCompiler::Impl& M;
            std::string m_package;
            BuiltPackage& m_out;
            PackageModel m_model;
            const ClassDecl* m_decl = nullptr;
            const Package* m_loaded = nullptr;
            ObjectRef m_parent;
            ObjectIndex m_class, m_defaults;
            std::vector<ObjectIndex> m_functions;
            std::map<std::int32_t, Built> m_built;              // By export index value
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
            void chain(const std::vector<ObjectIndex>& fields)
            {
                for (std::size_t i = 0; i < fields.size(); ++i)
                    m_built[fields[i].value].field.field.next = i + 1 < fields.size() ? fields[i + 1] : ObjectIndex{};
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

            struct Resolved
            {
                PropertyType type = PropertyType::Unknown;
                std::string class_name;              // "IntProperty"
                ObjectIndex reference, reference2;
                std::shared_ptr<TypeDecl> inner;
                bool needs_ctor = false;
                TypeDesc desc;
            };
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
                    const ObjectRef meta = find_kind(t.meta.empty() ? "Object" : t.meta, "Class");
                    if (!meta.valid()) throw DeclError{line, "no class " + t.meta};
                    r.reference2 = import_of(meta);
                }
                else if (const ObjectRef c = find_kind(t.name, "Class"); c.valid())
                {
                    simple(PropertyType::Object, "ObjectProperty");
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
                d.flags = v.flags | flags | (v.editable ? PropertyFlag::Edit : 0) | (r.needs_ctor ? PropertyFlag::NeedCtorLink : 0);
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

            ObjectIndex function(const FuncDecl& f)
            {
                // The function it overrides, in the parent classes
                const auto overridden = M.member(m_parent, f.name, [&](ObjectRef r) { return M.is_function(r); });
                const Function* over = overridden ? M.function(*overridden) : nullptr;
                const ObjectIndex super = overridden ? import_of(*overridden) : ObjectIndex{};
                const ObjectIndex i = add(kFieldFlags, core_class("Function"), m_class, f.name, super, {});
                m_functions.push_back(i);
                std::vector<ObjectIndex> children;
                std::uint32_t flags = f.flags | FunctionFlag::Defined;
                if (over)
                    flags |= over->flags & (FunctionFlag::Event | FunctionFlag::Simulated | FunctionFlag::Net |
                                            FunctionFlag::NetReliable | FunctionFlag::Exec | FunctionFlag::Static |
                                            FunctionFlag::Singular | FunctionFlag::NetServer | FunctionFlag::NetClient |
                                            FunctionFlag::Const | FunctionFlag::Public | FunctionFlag::Protected);
                for (const ParamDecl& p : f.params)
                {
                    VarDecl v;
                    v.type = p.type;
                    v.name = p.name;
                    v.line = f.line;
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
                b.field.structure = StructData{};
                b.field.structure->children = children.empty() ? ObjectIndex{} : children.front();
                b.field.structure->line = -1;
                b.field.structure->text_pos = -1;
                FunctionData fd;
                fd.flags = f.has_body ? flags : (flags & ~FunctionFlag::Defined);
                fd.friendly_name = m_model.make_name(f.name);
                b.field.function = fd;
                return i;
            }

            ObjectIndex parent_defaults(const FieldObject& parent)
            {
                if (const auto d = M.def(*m_parent.package, parent.class_data->default_object)) return import_of(*d);
                return {};
            }

            // ---- defaultproperties -----------------------------------------------------------------------------------
            void defaults()
            {
                PropertyParseOptions options;
                options.make_name = [this](std::string_view t) { return m_model.make_name(t); };
                options.resolve_object = [this](std::string_view text) -> std::optional<ObjectIndex> {
                    std::string_view path = text;
                    if (const auto q = text.find('\''); q != std::string_view::npos && text.back() == '\'')
                        path = text.substr(q + 1, text.size() - q - 2);
                    if (const auto d = M.find_path(path)) return import_of(*d);
                    return std::nullopt;
                };
                for (const DefaultLine& d : m_decl->defaults)
                {
                    // Ours or inherited
                    std::optional<TypeDesc> desc;
                    std::string name = d.name;
                    for (const VarDecl& v : m_decl->vars)
                        if (iequal(v.name, d.name))
                        {
                            desc = resolve(v.type, d.line).desc;
                            name = v.name;
                        }
                    if (!desc)
                        if (const auto p = M.member(m_parent, d.name, [&](ObjectRef r) { return M.is_property(r); }))
                        {
                            desc = type_desc(*p);
                            name = M.name_of(*p);
                        }
                    if (!desc)
                    {
                        m_out.errors.push_back(fmt::format("line {}: no property {} for defaultproperties", d.line, d.name));
                        continue;
                    }
                    try
                    {
                        Property prop;
                        prop.name = name;
                        prop.array_index = d.index;
                        prop.value = parse_property_value(d.value, *desc, options);
                        prop.tag = make_property_tag(name, d.index, *desc, options.make_name);
                        m_default_values.push_back(std::move(prop));
                    }
                    catch (const std::exception& e)
                    {
                        m_out.errors.push_back(fmt::format("line {}: {} = {}: {}", d.line, d.name, d.value, e.what()));
                    }
                }
            }
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
        };
    }

    BuiltPackage ScriptCompiler::build_class_package(std::string_view package_name, std::string_view source)
    {
        BuiltPackage out;
        try
        {
            DeclParser parser(source);
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
