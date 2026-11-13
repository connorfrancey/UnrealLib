#pragma once
/// @file
/// @brief The compiler's model of the game's classes (internal): what it reads about classes, structs, functions and
///        properties through the Linker, shared by the body compiler and the class package builder.
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <UnrealLib/Compiler/Compiler.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Reflection/Fields.hpp>

#include "Parser.hpp"

namespace UnrealLib::Compiler
{
    namespace detail
    {
        inline std::string lower(std::string_view s)
        {
            std::string out(s);
            for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }
        inline bool iequal(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
            return true;
        }

        struct RefKey
        {
            const Package* package;
            std::int32_t index;
            friend bool operator<(const RefKey& a, const RefKey& b)
            {
                return a.package != b.package ? std::less<const Package*>()(a.package, b.package) : a.index < b.index;
            }
        };
        inline RefKey key(ObjectRef r) { return {r.package, r.index.value}; }

        /// A compile error: the line and what went wrong
        struct Error
        {
            int line;
            std::string message;
        };

        // ---- Types ------------------------------------------------------------------------------------------------
        enum class K : std::uint8_t { Void, Byte, Int, Bool, Float, Name, String, Object, Class, Struct, Array, Delegate,
                                      Interface, Map, Pointer, Null, Unknown };

        struct Type
        {
            K kind = K::Void;
            ObjectRef ref;                       ///< Byte: enum; Object/Interface: class; Class: meta class; Struct: struct
            std::shared_ptr<const Type> inner;   ///< Array
            ObjectRef property;                  ///< The property the value came from (when it did)

            bool is_object_like() const { return kind == K::Object || kind == K::Class || kind == K::Interface || kind == K::Null; }
        };
        inline Type make(K k, ObjectRef ref = {}) { Type t; t.kind = k; t.ref = ref; return t; }

        // ---- Operators and functions --------------------------------------------------------------------------------
        struct Param
        {
            ObjectRef property;
            Type type;
            std::uint64_t flags = 0;
            std::string name;
            bool out() const { return (flags & PropertyFlag::OutParm) != 0; }
            bool optional() const { return (flags & PropertyFlag::OptionalParm) != 0; }
            bool skip() const { return (flags & PropertyFlag::SkipParm) != 0; }
            bool coerce() const { return (flags & PropertyFlag::CoerceParm) != 0; }
        };
        struct Function
        {
            ObjectRef def;
            std::string name;                    ///< Object name ("Add_IntInt")
            std::string text;                    ///< Friendly name for operators ("+"), else the name
            std::uint32_t flags = 0;
            std::uint16_t native = 0;
            std::uint8_t precedence = 0;
            std::vector<Param> params;
            std::optional<Param> ret;
            bool is_final() const { return (flags & FunctionFlag::Final) != 0; }
            bool is_static() const { return (flags & FunctionFlag::Static) != 0; }
            bool is_delegate() const { return (flags & FunctionFlag::Delegate) != 0; }
            bool is_iterator() const { return (flags & FunctionFlag::Iterator) != 0; }
        };

        constexpr std::uint8_t kNoCast = 0xFF;
        // Primitive cast opcodes (UE3 ECastToken): from -> to
        inline std::uint8_t cast_code(K from, K to)
        {
            using P = std::pair<K, K>;
            static const std::map<P, std::uint8_t> codes = {
                {{K::Interface, K::String}, 0x37}, {{K::Interface, K::Bool}, 0x38}, {{K::Struct, K::Struct}, 0x39},
                {{K::Byte, K::Int}, 0x3A}, {{K::Byte, K::Bool}, 0x3B}, {{K::Byte, K::Float}, 0x3C}, {{K::Int, K::Byte}, 0x3D},
                {{K::Int, K::Bool}, 0x3E}, {{K::Int, K::Float}, 0x3F}, {{K::Bool, K::Byte}, 0x40}, {{K::Bool, K::Int}, 0x41},
                {{K::Bool, K::Float}, 0x42}, {{K::Float, K::Byte}, 0x43}, {{K::Float, K::Int}, 0x44}, {{K::Float, K::Bool}, 0x45},
                {{K::Object, K::Bool}, 0x47}, {{K::Name, K::Bool}, 0x48}, {{K::String, K::Byte}, 0x49},
                {{K::String, K::Int}, 0x4A}, {{K::String, K::Bool}, 0x4B}, {{K::String, K::Float}, 0x4C},
                {{K::Byte, K::String}, 0x52}, {{K::Int, K::String}, 0x53}, {{K::Bool, K::String}, 0x54},
                {{K::Float, K::String}, 0x55}, {{K::Object, K::String}, 0x56}, {{K::Name, K::String}, 0x57},
                {{K::Delegate, K::String}, 0x5A}, {{K::String, K::Name}, 0x60},
            };
            const auto it = codes.find({from, to});
            return it == codes.end() ? kNoCast : it->second;
        }
        inline bool primitive_cast_name(std::string_view n, K& to, std::string& struct_name)
        {
            const std::string l = lower(n);
            if (l == "int") to = K::Int;
            else if (l == "float") to = K::Float;
            else if (l == "byte") to = K::Byte;
            else if (l == "bool") to = K::Bool;
            else if (l == "string") to = K::String;
            else if (l == "name") to = K::Name;
            else if (l == "vector" || l == "rotator") { to = K::Struct; struct_name = l; }
            else return false;
            return true;
        }
    }
    using namespace detail;

    // ==== The model: what the compiler reads about the game's classes ====================================================
    struct ScriptCompiler::Impl
    {
        Linker& linker;
        std::vector<const Package*> scripts;
        OperatorSet operator_set;
        std::multimap<std::string, Function*> operators;         // lowered text -> operator functions
        std::map<RefKey, std::unique_ptr<FieldObject>> fields;
        std::map<RefKey, std::unique_ptr<Function>> functions;
        std::map<const Package*, std::map<RefKey, ObjectIndex>> imports;   // per package: definition -> its import
        std::map<RefKey, std::uint32_t> struct_sizes;
        std::map<std::string, ObjectRef> by_path;                 // lowered "Package.Path" -> definition
        std::map<std::string, std::pair<ObjectRef, std::uint8_t>> enumerators;   // lowered "PHYS_Falling" -> enum, value
        std::size_t enumerators_scanned = 0;                      // Script packages scanned so far

        Impl(Linker& l, std::span<const Package* const> s) : linker(l), scripts(s.begin(), s.end()) { load_operators(); }

        // ---- Fields -----------------------------------------------------------------------------------------------
        const FieldObject* field(ObjectRef def)
        {
            if (!def.is_export()) return nullptr;
            const auto k = key(def);
            if (auto it = fields.find(k); it != fields.end()) return it->second.get();
            std::unique_ptr<FieldObject> f;
            try
            {
                f = std::make_unique<FieldObject>(linker.read_field(def));
            }
            catch (const std::exception&)
            {
                f = nullptr;
            }
            return fields.emplace(k, std::move(f)).first->second.get();
        }
        std::optional<ObjectRef> def(const Package& p, ObjectIndex index)
        {
            if (index.is_null()) return std::nullopt;
            if (index.is_export()) return ObjectRef{&p, index};
            const Resolution& r = linker.resolve(p, index);
            if (r.status == ResolveStatus::Export) return r.target;
            return std::nullopt;
        }
        std::string name_of(ObjectRef r) const { return r.package->object_name(r.index); }
        std::string class_of(ObjectRef r) const { return r.package->class_name(r.index); }
        ObjectRef outer_of(ObjectRef r) const
        {
            const ObjectIndex o = r.package->outer(r.index);
            return o.is_null() ? ObjectRef{} : ObjectRef{r.package, o};
        }
        std::optional<ObjectRef> super_of(ObjectRef r)
        {
            const FieldObject* f = field(r);
            if (!f) return std::nullopt;
            return def(*r.package, f->field.super);
        }
        std::vector<ObjectRef> children(ObjectRef r)
        {
            std::vector<ObjectRef> out;
            const FieldObject* f = field(r);
            if (!f || !f->structure) return out;
            ObjectIndex c = f->structure->children;
            for (int guard = 0; !c.is_null() && guard < 100000; ++guard)
            {
                const ObjectRef child{r.package, c};
                out.push_back(child);
                const FieldObject* cf = field(child);
                if (!cf) break;
                c = cf->field.next;
            }
            return out;
        }
        // The index of `d` in package `p`: its export, or the import that resolves to it
        ObjectIndex index_in(const Package& p, ObjectRef d)
        {
            if (d.package == &p) return d.index;
            auto [it, first] = imports.try_emplace(&p);
            if (first)
                for (std::size_t i = 0; i < p.imports().size(); ++i)
                {
                    const ObjectIndex idx = ObjectIndex::from_import(i);
                    const Resolution& r = linker.resolve(p, idx);
                    if (r.status == ResolveStatus::Export) it->second.emplace(key(r.target), idx);
                }
            const auto f = it->second.find(key(d));
            return f == it->second.end() ? ObjectIndex{} : f->second;
        }
        // An object by "Package.Outer.Name" path (object literals)
        // An enumerator by name, among every enum of the script packages (the first one declared wins)
        std::optional<std::pair<ObjectRef, std::uint8_t>> enumerator(std::string_view n)
        {
            for (; enumerators_scanned < scripts.size(); ++enumerators_scanned)
            {
                const Package* p = scripts[enumerators_scanned];
                for (std::size_t i = 0; i < p->exports().size(); ++i)
                {
                    const ObjectRef r{p, ObjectIndex::from_export(i)};
                    if (class_of(r) != "Enum") continue;
                    const FieldObject* f = field(r);
                    if (!f) continue;
                    for (std::size_t v = 0; v < f->enum_names.size() && v < 256; ++v)
                        if (const auto t = p->names().try_to_string(f->enum_names[v]))
                            enumerators.emplace(lower(*t), std::pair{r, static_cast<std::uint8_t>(v)});
                }
            }
            const auto it = enumerators.find(lower(n));
            if (it == enumerators.end()) return std::nullopt;
            return it->second;
        }

        std::optional<ObjectRef> find_path(std::string_view path)
        {
            if (by_path.empty())
                for (const Package* p : scripts)
                    for (std::size_t i = 0; i < p->exports().size(); ++i)
                    {
                        const ObjectIndex idx = ObjectIndex::from_export(i);
                        by_path.emplace(lower(p->name() + "." + p->object_path(idx)), ObjectRef{p, idx});
                    }
            const auto it = by_path.find(lower(path));
            if (it != by_path.end()) return it->second;
            // Objects of other packages (a map's): load the package and look
            const auto dot = path.find('.');
            if (dot == std::string_view::npos) return std::nullopt;
            const Package* p = linker.find_package(path.substr(0, dot));
            if (!p) return std::nullopt;
            for (std::size_t i = 0; i < p->exports().size(); ++i)
            {
                const ObjectIndex idx = ObjectIndex::from_export(i);
                if (iequal(p->name() + "." + p->object_path(idx), path)) return ObjectRef{p, idx};
            }
            return std::nullopt;
        }

        // ---- Types ------------------------------------------------------------------------------------------------
        Type property_type(ObjectRef prop)
        {
            const FieldObject* f = field(prop);
            Type t;
            t.property = prop;
            if (!f || !f->property) { t.kind = K::Unknown; return t; }
            const PropertyData& d = *f->property;
            const auto ref = [&](ObjectIndex i) { return def(*prop.package, i).value_or(ObjectRef{}); };
            switch (d.type)
            {
            case PropertyType::Byte: t.kind = K::Byte; t.ref = ref(d.reference); break;
            case PropertyType::Int: t.kind = K::Int; break;
            case PropertyType::Bool: t.kind = K::Bool; break;
            case PropertyType::Float: t.kind = K::Float; break;
            case PropertyType::Name: t.kind = K::Name; break;
            case PropertyType::Str: case PropertyType::String: t.kind = K::String; break;
            case PropertyType::Object: case PropertyType::Component: t.kind = K::Object; t.ref = ref(d.reference); break;
            case PropertyType::Class: t.kind = K::Class; t.ref = ref(d.reference2); break;
            case PropertyType::Interface: t.kind = K::Interface; t.ref = ref(d.reference); break;
            case PropertyType::Struct: t.kind = K::Struct; t.ref = ref(d.reference); break;
            case PropertyType::Delegate: t.kind = K::Delegate; t.ref = ref(d.reference); break;
            case PropertyType::Map: t.kind = K::Map; break;
            case PropertyType::Pointer: t.kind = K::Pointer; break;
            case PropertyType::Array:
            {
                t.kind = K::Array;
                if (const auto inner = def(*prop.package, d.reference))
                {
                    Type in = property_type(*inner);
                    in.property = {};
                    t.inner = std::make_shared<Type>(in);
                }
                break;
            }
            default: t.kind = K::Unknown; break;
            }
            return t;
        }
        std::uint64_t property_flags(ObjectRef prop)
        {
            const FieldObject* f = field(prop);
            return f && f->property ? f->property->flags : 0;
        }
        std::int32_t array_dim(ObjectRef prop)
        {
            const FieldObject* f = field(prop);
            return f && f->property ? f->property->dimension() : 1;
        }

        // Value sizes (Context and Switch store the size of the value): the engine's layout rules
        std::uint32_t size_of(const Type& t)
        {
            switch (t.kind)
            {
            case K::Byte: return 1;
            case K::Int: case K::Bool: case K::Float: case K::Object: case K::Class: case K::Pointer: case K::Null: return 4;
            case K::Name: case K::Interface: return 8;
            case K::String: case K::Array: case K::Delegate: return 12;
            case K::Map: return 60;
            case K::Struct: return t.ref.valid() ? struct_size(t.ref) : 0;
            default: return 0;
            }
        }
        std::uint32_t align_of(const Type& t)
        {
            if (t.kind == K::Byte) return 1;
            if (t.kind == K::Struct && t.ref.valid() && iequal(name_of(t.ref), "Matrix")) return 16;
            return 4;
        }
        std::uint32_t struct_size(ObjectRef s)
        {
            const auto k = key(s);
            if (auto it = struct_sizes.find(k); it != struct_sizes.end()) return it->second;
            struct_sizes[k] = 0;    // Guard
            std::uint32_t offset = 0;
            if (const auto super = super_of(s)) offset = struct_size(*super);
            bool bools = false;
            for (const ObjectRef c : children(s))
            {
                const FieldObject* f = field(c);
                if (!f || !f->property) continue;
                const Type t = property_type(c);
                if (t.kind == K::Bool)
                {
                    if (bools) continue;    // Packed into the previous one's dword
                    bools = true;
                }
                else
                {
                    bools = false;
                }
                const std::uint32_t a = align_of(t);
                offset = (offset + a - 1) / a * a + size_of(t) * static_cast<std::uint32_t>(std::max(1, array_dim(c)));
            }
            struct_sizes[k] = offset;
            return offset;
        }

        // ---- Functions --------------------------------------------------------------------------------------------
        Function* function(ObjectRef d)
        {
            const auto k = key(d);
            if (auto it = functions.find(k); it != functions.end()) return it->second.get();
            auto fn = std::make_unique<Function>();
            fn->def = d;
            fn->name = name_of(d);
            fn->text = fn->name;
            if (const FieldObject* f = field(d); f && f->function)
            {
                fn->flags = f->function->flags;
                fn->native = f->function->native_token;
                fn->precedence = f->function->oper_precedence;
                if (f->function->friendly_name)
                    fn->text = d.package->names().try_to_string(*f->function->friendly_name).value_or(fn->name);
            }
            for (const ObjectRef c : children(d))
            {
                const FieldObject* cf = field(c);
                if (!cf || !cf->property) continue;
                const std::uint64_t flags = cf->property->flags;
                Param p{c, property_type(c), flags, name_of(c)};
                if (flags & PropertyFlag::ReturnParm) fn->ret = p;
                else if (flags & PropertyFlag::Parm) fn->params.push_back(p);
            }
            return functions.emplace(k, std::move(fn)).first->second.get();
        }

        void load_operators()
        {
            std::map<std::string, std::map<std::uint8_t, int>> precedences;
            for (const Package* p : scripts)
                for (std::size_t i = 0; i < p->exports().size(); ++i)
                {
                    const ObjectRef r{p, ObjectIndex::from_export(i)};
                    if (p->class_name(r.index) != "Function") continue;
                    Function* fn = function(r);
                    if (!(fn->flags & FunctionFlag::Operator)) continue;
                    operators.emplace(lower(fn->text), fn);
                    if (fn->flags & FunctionFlag::PreOperator) operator_set.prefix.insert(fn->text);
                    else if (fn->precedence == 0) operator_set.postfix.insert(fn->text);
                    else ++precedences[fn->text][fn->precedence];
                }
            // An operator's precedence for parsing: the one most of its overloads declare
            for (const auto& [text, counts] : precedences)
            {
                const auto best = std::max_element(counts.begin(), counts.end(),
                                                   [](const auto& a, const auto& b) { return a.second < b.second; });
                operator_set.binary[text] = best->first;
            }
        }

        // ---- Lookup -----------------------------------------------------------------------------------------------
        // The first child named `n` of struct `s` or its supers (properties, functions, states, consts)
        std::optional<ObjectRef> member(ObjectRef s, std::string_view n, const std::function<bool(ObjectRef)>& accept)
        {
            std::optional<ObjectRef> cur = s;
            for (int guard = 0; cur && guard < 64; ++guard)
            {
                for (const ObjectRef c : children(*cur))
                    if (iequal(name_of(c), n) && accept(c)) return c;
                cur = super_of(*cur);
            }
            return std::nullopt;
        }
        bool is_property(ObjectRef r)
        {
            const FieldObject* f = field(r);
            return f && f->kind == FieldKind::Property;
        }
        bool is_function(ObjectRef r) { return class_of(r) == "Function"; }
    };
}
