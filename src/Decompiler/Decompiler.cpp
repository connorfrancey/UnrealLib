#include <UnrealLib/Decompiler/Decompiler.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <utility>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Reflection/Fields.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>

#include "Printer.hpp"

namespace UnrealLib::Decompiler
{
    std::string format_float(float value)
    {
        if (!std::isfinite(value))
        {
            return std::isnan(value) ? "__nan__" : (value > 0 ? "__inf__" : "-__inf__");
        }
        std::array<char, 128> buffer{};
        const auto [end, error] =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed);
        std::string text = error == std::errc{} ? std::string(buffer.data(), end) : fmt::format("{}", value);
        if (text.find('.') == std::string::npos)
        {
            text += ".0";
        }
        return text;
    }

    std::string quote_string(std::string_view text)
    {
        std::string result;
        result.reserve(text.size() + 2);
        result += '"';
        for (const char c : text)
        {
            switch (c)
            {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            default: result += c; break;
            }
        }
        result += '"';
        return result;
    }

    namespace
    {
        constexpr std::size_t max_children = 4096; ///< Bound on a struct's children list (cycles, corrupt data).

        std::pair<const Package*, std::int32_t> key(ObjectRef ref)
        {
            return {ref.package, ref.index.value};
        }

        /// Function modifiers, in upstream UFunction.FormatFlags order.
        std::string function_modifiers(const FunctionData& f)
        {
            std::string out;
            if ((f.flags & FunctionFlag::Private) != 0) out += "private ";
            else if ((f.flags & FunctionFlag::Protected) != 0) out += "protected ";
            if ((f.flags & FunctionFlag::DLLImport) != 0) out += "dllimport ";
            if ((f.flags & FunctionFlag::Net) != 0)
            {
                out += (f.flags & FunctionFlag::NetReliable) != 0 ? "reliable " : "unreliable ";
                if ((f.flags & FunctionFlag::NetClient) != 0) out += "client ";
                if ((f.flags & FunctionFlag::NetServer) != 0) out += "server ";
            }
            if ((f.flags & FunctionFlag::Native) != 0)
            {
                out += f.native_token > 0 ? fmt::format("native({}) ", f.native_token) : std::string("native ");
            }
            if ((f.flags & FunctionFlag::Static) != 0) out += "static ";
            if ((f.flags & FunctionFlag::Final) != 0) out += "final ";
            if ((f.flags & FunctionFlag::Iterator) != 0) out += "iterator ";
            if ((f.flags & FunctionFlag::Latent) != 0) out += "latent ";
            if ((f.flags & FunctionFlag::Singular) != 0) out += "singular ";
            if ((f.flags & FunctionFlag::Simulated) != 0) out += "simulated ";
            if ((f.flags & FunctionFlag::Exec) != 0) out += "exec ";

            bool keyword = false;
            if ((f.flags & FunctionFlag::Event) != 0)
            {
                out += "event ";
                keyword = true;
            }
            if ((f.flags & FunctionFlag::Delegate) != 0)
            {
                out += "delegate ";
                keyword = true;
            }
            switch (call_syntax(f.flags, f.oper_precedence))
            {
            case CallSyntax::PreOperator: out += "preoperator "; keyword = true; break;
            case CallSyntax::PostOperator: out += "postoperator "; keyword = true; break;
            case CallSyntax::Operator: out += fmt::format("operator({}) ", f.oper_precedence); keyword = true; break;
            case CallSyntax::Function: break;
            }
            if (!keyword) out += "function ";
            return out;
        }

        std::string parameter_modifiers(std::uint64_t flags)
        {
            std::string out;
            if ((flags & PropertyFlag::OptionalParm) != 0) out += "optional ";
            if ((flags & PropertyFlag::Const) != 0) out += "const ";
            if ((flags & PropertyFlag::OutParm) != 0) out += "out ";
            if ((flags & PropertyFlag::CoerceParm) != 0) out += "coerce ";
            return out;
        }

        std::string array_suffix(const PropertyData& p)
        {
            return p.dimension() > 1 ? fmt::format("[{}]", p.dimension()) : std::string();
        }

        /// Labels of the LabelTable token(s) of @p script (state code).
        std::vector<LabelEntry> label_table(const Script& script)
        {
            std::vector<LabelEntry> labels;
            for (const Token& t : script.tokens)
            {
                if (t.kind == TokenKind::LabelTable)
                {
                    labels.insert(labels.end(), t.labels.begin(), t.labels.end());
                }
            }
            return labels;
        }
    }

    ScriptDecompiler::ScriptDecompiler(Linker& linker, NativeTable natives, DecompileOptions options)
        : m_linker(linker), m_natives(std::move(natives)), m_options(std::move(options))
    {
    }

    const ScriptDecompiler::FunctionInfo* ScriptDecompiler::function_info(ObjectRef ref)
    {
        const auto k = key(ref);
        if (const auto it = m_functions.find(k); it != m_functions.end())
        {
            return it->second ? &*it->second : nullptr;
        }
        std::optional<FunctionInfo> info;
        try
        {
            ObjectRef target = ref;
            if (!ref.index.is_export())
            {
                const Resolution& resolution = m_linker.resolve(ref);
                target = resolution.status == ResolveStatus::Export ? resolution.target : ObjectRef{};
            }
            if (target.is_export())
            {
                const FieldObject field = m_linker.read_field(target);
                if (field.function)
                {
                    const FunctionData& f = *field.function;
                    FunctionInfo result;
                    result.syntax = call_syntax(f.flags, f.oper_precedence);
                    result.precedence = f.oper_precedence;
                    result.name = result.syntax != CallSyntax::Function && f.friendly_name
                                      ? target.package->names().to_string(*f.friendly_name)
                                      : target.package->object_name(target.index);
                    result.target = target;
                    const ObjectIndex outer = target.package->outer(target.index);
                    result.outer_name = outer.is_null() ? std::string() : target.package->object_name(outer);
                    info = std::move(result);
                }
            }
        }
        catch (const Error&)
        {
            info.reset();
        }
        const auto& stored = m_functions.emplace(k, std::move(info)).first->second;
        return stored ? &*stored : nullptr;
    }

    std::string ScriptDecompiler::super_of_outer(ObjectRef function)
    {
        const ObjectIndex outer = function.package->outer(function.index);
        if (!outer.is_export())
        {
            return {};
        }
        const ObjectRef owner{function.package, outer};
        const auto k = key(owner);
        if (const auto it = m_supers.find(k); it != m_supers.end())
        {
            return it->second;
        }
        std::string name;
        try
        {
            const FieldObject field = m_linker.read_field(owner);
            if (!field.field.super.is_null())
            {
                name = owner.package->object_name(field.field.super);
            }
        }
        catch (const Error&)
        {
        }
        return m_supers.emplace(k, std::move(name)).first->second;
    }

    std::string ScriptDecompiler::type_name(ObjectRef property)
    {
        const auto k = key(property);
        if (const auto it = m_types.find(k); it != m_types.end())
        {
            return it->second;
        }
        std::string result = "__unknown_type__";
        try
        {
            ObjectRef target = property;
            if (!property.index.is_export())
            {
                const Resolution& resolution = m_linker.resolve(property);
                target = resolution.status == ResolveStatus::Export ? resolution.target : ObjectRef{};
            }
            if (target.is_export())
            {
                const FieldObject field = m_linker.read_field(target);
                if (field.property)
                {
                    const PropertyData& p = *field.property;
                    const Package& pkg = *target.package;
                    const auto name_of = [&](ObjectIndex index, std::string_view fallback) {
                        return index.is_null() ? std::string(fallback) : pkg.object_name(index);
                    };
                    const auto inner = [&](ObjectIndex index) {
                        return index.is_null() ? std::string("__unknown_type__") : type_name(ObjectRef{&pkg, index});
                    };
                    switch (p.type)
                    {
                    case PropertyType::Byte: result = name_of(p.reference, "byte"); break;
                    case PropertyType::Int: result = "int"; break;
                    case PropertyType::Bool: result = "bool"; break;
                    case PropertyType::Float: result = "float"; break;
                    case PropertyType::Str: result = "string"; break;
                    case PropertyType::String: result = "string"; break;
                    case PropertyType::Name: result = "name"; break;
                    case PropertyType::Object:
                    case PropertyType::Component:
                    case PropertyType::Interface: result = name_of(p.reference, "Object"); break;
                    case PropertyType::Class:
                        result = p.reference2.is_null() ? std::string("class")
                                                        : "class<" + pkg.object_name(p.reference2) + ">";
                        break;
                    case PropertyType::Struct: result = name_of(p.reference, "__unknown_struct__"); break;
                    case PropertyType::Delegate: result = "delegate<" + name_of(p.reference, "__unknown__") + ">"; break;
                    case PropertyType::Array:
                    {
                        const std::string element = inner(p.reference);
                        // `array<class<X> >`: UnrealScript cannot parse `>>` here.
                        result = "array<" + element + (element.ends_with('>') ? " >" : ">");
                        break;
                    }
                    case PropertyType::Map:
                        result = "map{" + inner(p.reference) + ", " + inner(p.reference2) + "}";
                        break;
                    case PropertyType::FixedArray:
                        result = inner(p.reference) + fmt::format("[{}]", p.fixed_count);
                        break;
                    case PropertyType::Pointer: result = "pointer"; break;
                    case PropertyType::Unknown: break;
                    }
                }
            }
        }
        catch (const Error&)
        {
        }
        return m_types.emplace(k, std::move(result)).first->second;
    }

    DecompiledScript ScriptDecompiler::decompile_body(const Script& script, ObjectRef owner,
                                                      const StructureOptions& structure_options, int depth)
    {
        DecompiledScript out;
        StructureOptions options = structure_options;
        if (options.labels.empty())
        {
            options.labels = label_table(script);
        }
        if (options.names == nullptr)
        {
            options.names = &owner.package->names();
        }
        out.structure = structure(script, options);
        Printer printer(*this, script, owner, out);
        for (const std::uint32_t parameter : out.structure.default_parameters)
        {
            out.default_values.push_back(printer.text(parameter));
        }
        printer.print(out.structure.statements, depth, out.source);
        return out;
    }

    DecompiledScript ScriptDecompiler::decompile(ObjectRef object)
    {
        if (!object.is_export())
        {
            throw Error(ErrorCode::InvalidArgument, "decompile() requires an export reference");
        }
        const Package& package = *object.package;
        const FieldObject field = m_linker.read_field(object);
        if (field.kind != FieldKind::Function && field.kind != FieldKind::State)
        {
            throw Error(ErrorCode::InvalidArgument,
                        fmt::format("'{}' is a {}, not a function or state", m_linker.qualified_path(object),
                                    to_string(field.kind)));
        }

        // Bytecode
        Script script;
        const bool has_script = field.structure && field.structure->script_size > 0;
        if (has_script)
        {
            const TokenMap map = TokenMap::ue3(package.summary().file_version, &package.profile());
            BytecodeOptions options;
            options.token_map = &map;
            options.name_count = static_cast<std::int32_t>(package.names().size());
            options.none_index = package.names().find("None").value_or(-1);
            BinaryReader reader(package.export_data(object.index.export_index()), package.context());
            reader.seek(field.structure->script_offset);
            script = decode_script(reader, static_cast<std::size_t>(field.structure->script_size), options);
        }
        DecompiledScript body = has_script ? decompile_body(script, object, {}, 1) : DecompiledScript{};

        const std::string name = package.object_name(object.index);
        std::string source;
        if (field.kind == FieldKind::State)
        {
            const std::uint32_t flags = field.state ? field.state->flags : 0;
            std::string header;
            if ((flags & StateFlag::Simulated) != 0) header += "simulated ";
            if ((flags & StateFlag::Auto) != 0) header += "auto ";
            header += (flags & StateFlag::Editable) != 0 ? "state() " : "state ";
            header += name;
            if (!field.field.super.is_null())
            {
                header += " extends " + package.object_name(field.field.super);
            }
            // The state's own functions (overrides defined in it), then its label code. The children list holds
            // them in reverse declaration order, as for every struct (upstream UState.FormatFunctions).
            std::string functions;
            std::vector<ObjectRef> members = children(object);
            for (auto it = members.rbegin(); it != members.rend(); ++it)
            {
                if (package.class_name(it->index) != "Function")
                {
                    continue;
                }
                const FieldObject member = m_linker.read_field(*it);
                if (!member.function || (member.function->flags & FunctionFlag::Defined) == 0)
                {
                    continue;
                }
                const DecompiledScript function = decompile(*it);
                accumulate(body, function);
                functions += indent_lines(function.source, m_options.indent) + "\n";
            }
            source = header + "\n{\n" + functions + body.source + "}\n";
        }
        else
        {
            const FunctionData& f = *field.function;
            std::vector<std::string> parameters;
            std::vector<std::string> locals;
            std::string return_type;
            std::size_t optional_index = 0;
            ObjectIndex child = field.structure ? field.structure->children : ObjectIndex{};
            for (std::size_t n = 0; !child.is_null() && n < max_children; ++n)
            {
                if (!child.is_export())
                {
                    break;
                }
                const ObjectRef child_ref{&package, child};
                const FieldObject member = m_linker.read_field(child_ref);
                if (member.property)
                {
                    const PropertyData& p = *member.property;
                    const std::string type = type_name(child_ref);
                    const std::string member_name = package.object_name(child) + array_suffix(p);
                    if ((p.flags & PropertyFlag::ReturnParm) != 0)
                    {
                        return_type = type;
                    }
                    else if ((p.flags & PropertyFlag::Parm) != 0)
                    {
                        std::string parameter = parameter_modifiers(p.flags) + type + " " + member_name;
                        if ((p.flags & PropertyFlag::OptionalParm) != 0 && optional_index < body.default_values.size())
                        {
                            const std::string& value = body.default_values[optional_index++];
                            if (!value.empty())
                            {
                                parameter += " = " + value;
                            }
                        }
                        parameters.push_back(std::move(parameter));
                    }
                    else
                    {
                        locals.push_back("local " + type + " " + member_name + ";");
                    }
                }
                child = member.field.next;
            }

            const std::string function_name = call_syntax(f.flags, f.oper_precedence) != CallSyntax::Function &&
                                                      f.friendly_name
                                                  ? package.names().to_string(*f.friendly_name)
                                                  : name;
            std::string header = function_modifiers(f) + (return_type.empty() ? "" : return_type + " ") +
                                 function_name + "(";
            for (std::size_t i = 0; i < parameters.size(); ++i)
            {
                header += (i == 0 ? "" : ", ") + parameters[i];
            }
            header += ")";
            if ((f.flags & FunctionFlag::Const) != 0)
            {
                header += " const";
            }
            // Native functions have a script listing their parameters, which prints as nothing.
            const bool native = (f.flags & FunctionFlag::Native) != 0;
            if (locals.empty() && (!has_script || (native && body.source.empty())))
            {
                source = header + ";\n";
            }
            else
            {
                source = header + "\n{\n";
                for (const std::string& local : locals)
                {
                    source += m_options.indent + local + "\n";
                }
                if (!locals.empty() && !body.source.empty())
                {
                    source += "\n";
                }
                source += body.source + "}\n";
            }
        }
        body.source = std::move(source);
        return body;
    }
}
