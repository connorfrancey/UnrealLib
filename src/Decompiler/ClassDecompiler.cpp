// Class-level output of ScriptDecompiler: class declaration, constants, enums, structs, variables, replication,
// functions, states and default properties.

#include <UnrealLib/Decompiler/Decompiler.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Reflection/Fields.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Utilities/Text.hpp>

#include "Printer.hpp"

namespace UnrealLib::Decompiler
{
    std::string indent_lines(std::string_view text, std::string_view indent)
    {
        std::string out;
        out.reserve(text.size() + text.size() / 8);
        std::size_t start = 0;
        while (start < text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos)
            {
                end = text.size();
            }
            const std::string_view line = text.substr(start, end - start);
            if (!line.empty())
            {
                out += indent;
                out += line;
            }
            out += '\n';
            start = end + 1;
        }
        return out;
    }

    namespace
    {
        constexpr std::size_t max_children = 65536; ///< Bound on a children list (cycles, corrupt data).
        constexpr int max_subobject_nesting = 8;

        /// The class/struct export a (possibly imported) reference points to.
        std::optional<ObjectRef> export_of(Linker& linker, ObjectRef ref)
        {
            if (ref.index.is_null())
            {
                return std::nullopt;
            }
            if (ref.index.is_export())
            {
                return ref;
            }
            const Resolution& resolution = linker.resolve(ref);
            if (resolution.status == ResolveStatus::Export)
            {
                return resolution.target;
            }
            return std::nullopt;
        }

        std::vector<std::string> name_list(const Package& package, const std::vector<FName>& names)
        {
            std::vector<std::string> out;
            out.reserve(names.size());
            for (const FName& n : names) out.push_back(package.names().to_string(n));
            return out;
        }

        bool contains(const std::vector<std::string>& list, std::string_view value)
        {
            return std::any_of(list.begin(), list.end(), [&](const std::string& v) { return Text::iequals(v, value); });
        }

        std::string join(const std::vector<std::string>& items, std::string_view separator)
        {
            std::string out;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                if (i != 0) out += separator;
                out += items[i];
            }
            return out;
        }

        /// Variable modifiers. `Edit` (the `var()` parentheses), `Net` (replication block), `Component`,
        /// `NeedCtorLink` and the parameter flags are implied by the declaration and not repeated.
        std::string variable_modifiers(std::uint64_t f)
        {
            std::string out;
            const auto add = [&](std::uint64_t flag, std::string_view word) {
                if ((f & flag) != 0)
                {
                    out += word;
                    out += ' ';
                }
            };
            add(PropertyFlag::PrivateWrite, "privatewrite");
            add(PropertyFlag::ProtectedWrite, "protectedwrite");
            add(PropertyFlag::RepNotify, "repnotify");
            add(PropertyFlag::NoClear, "noclear");
            add(PropertyFlag::NoImport, "noimport");
            add(PropertyFlag::DataBinding, "databinding");
            add(PropertyFlag::EditHide, "edithide");
            add(PropertyFlag::EditTextBox, "edittextbox");
            add(PropertyFlag::Interp, "interp");
            add(PropertyFlag::NonTransactional, "nontransactional");
            add(PropertyFlag::Const, "const");
            add(PropertyFlag::GlobalConfig, "globalconfig");
            if ((f & PropertyFlag::GlobalConfig) == 0) add(PropertyFlag::Config, "config");
            add(PropertyFlag::Localized, "localized");
            add(PropertyFlag::Transient, "transient");
            add(PropertyFlag::DuplicateTransient, "duplicatetransient");
            add(PropertyFlag::Native, "native");
            add(PropertyFlag::EditConst, "editconst");
            add(PropertyFlag::EditFixedSize, "editfixedsize");
            if ((f & PropertyFlag::ExportObject) != 0 && (f & PropertyFlag::EditInline) != 0)
            {
                out += "instanced ";
            }
            else
            {
                add(PropertyFlag::EditInline, "editinline");
                add(PropertyFlag::ExportObject, "export");
            }
            add(PropertyFlag::EditInlineUse, "editinlineuse");
            add(PropertyFlag::Deprecated, "deprecated");
            add(PropertyFlag::NoExport, "noexport");
            add(PropertyFlag::Input, "input");
            add(PropertyFlag::AlwaysInit, "init");
            add(PropertyFlag::EditorOnly, "editoronly");
            add(PropertyFlag::ArchetypeProperty, "archetype");
            add(PropertyFlag::CrossLevelPassive, "crosslevelpassive");
            add(PropertyFlag::CrossLevelActive, "crosslevelactive");
            add(PropertyFlag::SerializeText, "serializetext");
            return out;
        }

        std::string struct_modifiers(std::uint32_t f)
        {
            std::string out;
            if ((f & StructFlag::Native) != 0) out += "native ";
            if ((f & StructFlag::Atomic) != 0) out += "atomic ";
            if ((f & StructFlag::AtomicWhenCooked) != 0) out += "atomicwhencooked ";
            if ((f & StructFlag::Immutable) != 0) out += "immutable ";
            if ((f & StructFlag::ImmutableWhenCooked) != 0) out += "immutablewhencooked ";
            return out;
        }

        /// Whether a property value (or anything nested in it) is kept as raw bytes.
        bool has_raw(const PropertyValue& value)
        {
            if (std::holds_alternative<RawValue>(value.value))
            {
                return true;
            }
            if (const auto* array = std::get_if<ArrayValue>(&value.value))
            {
                return !array->decoded() ||
                       std::any_of(array->elements.begin(), array->elements.end(), [](const PropertyValue& e) {
                           return has_raw(e);
                       });
            }
            if (const auto* s = std::get_if<StructValue>(&value.value))
            {
                return std::any_of(s->fields.begin(), s->fields.end(),
                                   [](const Property& field) { return has_raw(field.value); });
            }
            return false;
        }
    }

    std::string ScriptDecompiler::indentation(int depth) const
    {
        std::string out;
        for (int i = 0; i < depth; ++i) out += m_options.indent;
        return out;
    }

    void ScriptDecompiler::accumulate(DecompiledScript& totals, const DecompiledScript& part)
    {
        totals.unknown_natives += part.unknown_natives;
        totals.placeholders += part.placeholders;
        totals.structure.gotos += part.structure.gotos;
        totals.structure.problems += part.structure.problems;
        totals.structure.unresolved += part.structure.unresolved;
        totals.diagnostics.append(part.diagnostics);
    }

    std::vector<ObjectRef> ScriptDecompiler::children(ObjectRef owner)
    {
        std::vector<ObjectRef> result;
        const FieldObject field = m_linker.read_field(owner);
        ObjectIndex child = field.structure ? field.structure->children : ObjectIndex{};
        for (std::size_t n = 0; child.is_export() && n < max_children; ++n)
        {
            const ObjectRef ref{owner.package, child};
            result.push_back(ref);
            child = m_linker.read_field(ref).field.next;
        }
        return result;
    }

    std::string ScriptDecompiler::variable_declaration(ObjectRef property, std::string_view owner_name)
    {
        const Package& package = *property.package;
        const FieldObject field = m_linker.read_field(property);
        if (!field.property)
        {
            return {};
        }
        const PropertyData& p = *field.property;
        const std::string name = package.object_name(property.index);
        std::string out = "var";
        if ((p.flags & PropertyFlag::Edit) != 0)
        {
            const std::string category = p.category ? package.names().to_string(*p.category) : std::string();
            out += category.empty() || Text::iequals(category, "None") || Text::iequals(category, owner_name)
                       ? std::string("()")
                       : "(" + category + ")";
        }
        out += " " + variable_modifiers(p.flags) + type_name(property) + " " + name;
        if (p.dimension() > 1)
        {
            out += "[" + (p.array_enum.is_null() ? std::to_string(p.dimension()) : package.object_name(p.array_enum)) +
                   "]";
        }
        out += ";";
        // `delegate<X> __X__Delegate`: generated by the compiler for every delegate declaration.
        if (name.starts_with("__") && name.ends_with("__Delegate"))
        {
            out = "// " + out;
        }
        return out;
    }

    std::string ScriptDecompiler::const_source(ObjectRef constant, int depth)
    {
        const FieldObject field = m_linker.read_field(constant);
        std::string value = field.const_value ? field.const_value->text : std::string();
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                                  value.back() == '\n'))
        {
            value.pop_back();
        }
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        {
            value.erase(value.begin());
        }
        return indentation(depth) + "const " + constant.package->object_name(constant.index) + " = " + value + ";\n";
    }

    std::string ScriptDecompiler::enum_source(ObjectRef enumeration, int depth)
    {
        const Package& package = *enumeration.package;
        const FieldObject field = m_linker.read_field(enumeration);
        const std::string name = package.object_name(enumeration.index);
        std::vector<std::string> values = name_list(package, field.enum_names);
        // The compiler appends `<Enum>_MAX`; it is not part of the declaration.
        if (!values.empty() && Text::iequals(values.back(), name + "_MAX"))
        {
            values.pop_back();
        }
        std::string out = indentation(depth) + "enum " + name + "\n" + indentation(depth) + "{\n";
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            out += indentation(depth + 1) + values[i] + (i + 1 < values.size() ? ",\n" : "\n");
        }
        return out + indentation(depth) + "};\n";
    }

    std::string ScriptDecompiler::struct_source(ObjectRef structure, int depth, DecompiledScript& totals)
    {
        const Package& package = *structure.package;
        const FieldObject field = m_linker.read_field(structure, m_schema);
        const std::string name = package.object_name(structure.index);
        std::string out = indentation(depth) + "struct " +
                          struct_modifiers(field.script_struct ? field.script_struct->flags : 0) + name;
        if (!field.field.super.is_null())
        {
            out += " extends " + package.object_name(field.field.super);
        }
        out += "\n" + indentation(depth) + "{\n";

        const std::vector<ObjectRef> members = children(structure);
        std::string nested;
        std::string variables;
        for (auto it = members.rbegin(); it != members.rend(); ++it)
        {
            const std::string kind = package.class_name(it->index);
            if (kind == "Const") nested += const_source(*it, depth + 1);
            else if (kind == "Enum") nested += enum_source(*it, depth + 1) + "\n";
            else if (kind == "ScriptStruct") nested += struct_source(*it, depth + 1, totals) + "\n";
        }
        for (const ObjectRef& member : members)
        {
            if (field_kind_of(package.class_name(member.index)) == FieldKind::Property)
            {
                variables += indentation(depth + 1) + variable_declaration(member, name) + "\n";
            }
        }
        out += nested + variables;

        if (field.script_struct && !field.script_struct->defaults.empty())
        {
            std::string defaults;
            format_properties(package, field.script_struct->defaults, {}, depth + 2, defaults, totals);
            if (!defaults.empty())
            {
                out += (variables.empty() && nested.empty() ? "" : "\n") + indentation(depth + 1) +
                       "structdefaultproperties\n" + indentation(depth + 1) + "{\n" + defaults +
                       indentation(depth + 1) + "}\n";
            }
        }
        return out + indentation(depth) + "};\n";
    }

    std::string ScriptDecompiler::class_header(ObjectRef object, const FieldObject& field)
    {
        const Package& package = *object.package;
        const ClassData& c = *field.class_data;
        const bool is_interface = (c.flags & ClassFlag::Interface) != 0;
        std::string out = (is_interface ? "interface " : "class ") + package.object_name(object.index);

        // The parent class, for the flags and lists a class inherits (only differences are declared).
        std::optional<FieldObject> parent;
        const Package* parent_package = nullptr;
        std::string super_name;
        if (!field.field.super.is_null())
        {
            super_name = package.object_name(field.field.super);
            if (const auto ref = export_of(m_linker, ObjectRef{&package, field.field.super}))
            {
                try
                {
                    FieldObject f = m_linker.read_field(*ref);
                    if (f.class_data)
                    {
                        parent = std::move(f);
                        parent_package = ref->package;
                    }
                }
                catch (const Error&)
                {
                }
            }
            if (!(is_interface && Text::iequals(super_name, "Object")))
            {
                out += " extends " + super_name;
            }
        }
        const std::uint32_t parent_flags = parent ? parent->class_data->flags : 0;
        const auto inherited = [&](std::uint32_t flag) { return (parent_flags & flag) != 0; };

        if (!c.within.is_null())
        {
            const std::string within = package.object_name(c.within);
            const std::string parent_within =
                parent && !parent->class_data->within.is_null() ? parent_package->object_name(parent->class_data->within)
                                                                : std::string("Object");
            if (!Text::iequals(within, parent_within))
            {
                out += " within " + within;
            }
        }

        std::vector<std::string> modifiers;
        const auto flag = [&](std::uint32_t f, std::string_view word, bool inheritable) {
            if ((c.flags & f) != 0 && !(inheritable && inherited(f)))
            {
                modifiers.emplace_back(word);
            }
        };
        flag(ClassFlag::Abstract, "abstract", false);
        flag(ClassFlag::Native, "native", false);
        flag(ClassFlag::NativeReplication, "nativereplication", false);
        flag(ClassFlag::Transient, "transient", true);
        if ((c.flags & ClassFlag::Config) != 0)
        {
            const std::string config = package.names().to_string(c.config_name);
            const std::string parent_config =
                parent && inherited(ClassFlag::Config) ? parent_package->names().to_string(parent->class_data->config_name)
                                                       : std::string();
            if (!inherited(ClassFlag::Config) || !Text::iequals(config, parent_config))
            {
                modifiers.push_back(Text::iequals(config, "None") || Text::iequals(config, "System")
                                        ? std::string("config")
                                        : "config(" + config + ")");
            }
        }
        flag(ClassFlag::PerObjectConfig, "perobjectconfig", true);
        flag(ClassFlag::EditInlineNew, "editinlinenew", true);
        flag(ClassFlag::CollapseCategories, "collapsecategories", true);
        flag(ClassFlag::NoExport, "noexport", false);
        flag(ClassFlag::SafeReplace, "safereplace", true);
        flag(ClassFlag::HideDropDown, "hidedropdown", false);
        flag(ClassFlag::Deprecated, "deprecated", false);
        if ((c.flags & ClassFlag::Placeable) != 0)
        {
            flag(ClassFlag::Placeable, "placeable", true);
        }
        else if (inherited(ClassFlag::Placeable))
        {
            modifiers.emplace_back("notplaceable");
        }

        const auto group = [&](std::string_view word, const std::vector<std::string>& own,
                               const std::vector<std::string>& parents) {
            std::vector<std::string> added;
            for (const std::string& item : own)
            {
                if (!contains(parents, item)) added.push_back(item);
            }
            if (!added.empty())
            {
                modifiers.push_back(std::string(word) + "(" + join(added, ", ") + ")");
            }
        };
        group("hidecategories", name_list(package, c.hide_categories),
              parent ? name_list(*parent_package, parent->class_data->hide_categories) : std::vector<std::string>{});
        group("autoexpandcategories", name_list(package, c.auto_expand_categories),
              parent ? name_list(*parent_package, parent->class_data->auto_expand_categories)
                     : std::vector<std::string>{});
        std::vector<std::string> interfaces;
        for (const auto& [interface, property] : c.interfaces)
        {
            interfaces.push_back(package.object_name(interface));
        }
        std::vector<std::string> parent_interfaces;
        if (parent)
        {
            for (const auto& [interface, property] : parent->class_data->interfaces)
            {
                parent_interfaces.push_back(parent_package->object_name(interface));
            }
        }
        group("implements", interfaces, parent_interfaces);

        if (modifiers.empty())
        {
            return out + ";\n";
        }
        for (const std::string& m : modifiers)
        {
            out += "\n" + m_options.indent + m;
        }
        return out + ";\n";
    }

    std::string ScriptDecompiler::replication(ObjectRef object, const FieldObject& field,
                                              const std::vector<ObjectRef>& properties, DecompiledScript& totals)
    {
        // Replicated variables point (RepOffset) at their condition in the class script.
        std::map<std::uint16_t, std::vector<std::string>> groups;
        for (const ObjectRef& property : properties)
        {
            const FieldObject p = m_linker.read_field(property);
            if (p.property && (p.property->flags & PropertyFlag::Net) != 0 && p.property->rep_offset &&
                *p.property->rep_offset != 0xFFFF)
            {
                groups[*p.property->rep_offset].push_back(object.package->object_name(property.index));
            }
        }
        if (groups.empty() || !field.structure || field.structure->script_size <= 0)
        {
            return {};
        }
        const Package& package = *object.package;
        const TokenMap map = TokenMap::ue3(package.summary().file_version, &package.profile());
        BytecodeOptions options;
        options.token_map = &map;
        options.name_count = static_cast<std::int32_t>(package.names().size());
        options.none_index = package.names().find("None").value_or(-1);
        BinaryReader reader(package.export_data(object.index.export_index()), package.context());
        reader.seek(field.structure->script_offset);
        const Script script = decode_script(reader, static_cast<std::size_t>(field.structure->script_size), options);

        DecompiledScript sink;
        Printer printer(*this, script, object, sink);
        std::string out = "replication\n{\n";
        bool first = true;
        for (auto& [offset, names] : groups)
        {
            std::uint32_t condition = no_token;
            for (std::uint32_t t = 0; t < script.tokens.size(); ++t)
            {
                if (script.tokens[t].memory_offset == offset)
                {
                    condition = t; // the first token at an offset is the outermost one
                    break;
                }
            }
            std::sort(names.begin(), names.end());
            out += first ? "" : "\n";
            first = false;
            if (condition == no_token)
            {
                ++sink.placeholders;
                sink.diagnostics.warning("decompiler.placeholder",
                                         fmt::format("replication condition at 0x{:X} not found", offset));
                out += m_options.indent + fmt::format("if (__condition_0x{:X}__)\n", offset);
            }
            else
            {
                out += m_options.indent + "if (" + printer.text(condition) + ")\n";
            }
            out += m_options.indent + m_options.indent + join(names, ", ") + ";\n";
        }
        accumulate(totals, sink);
        return out + "}\n";
    }

    void ScriptDecompiler::format_properties(const Package& package, const std::vector<Property>& properties,
                                             const std::set<std::int32_t>& subobjects, int depth, std::string& out,
                                             DecompiledScript& totals)
    {
        PropertyFormatOptions format;
        format.names = &package.names();
        format.object_name = [&](ObjectIndex index) -> std::string {
            if (index.is_null() || !package.is_valid(index))
            {
                return "None";
            }
            if (subobjects.contains(index.value))
            {
                return package.object_name(index); // defined by a Begin Object block above
            }
            const std::string path = index.is_export() ? package.name() + "." + package.object_path(index)
                                                       : package.object_path(index);
            return package.class_name(index) + "'" + path + "'";
        };

        const std::string pad = indentation(depth);
        for (const Property& p : properties)
        {
            const std::string name = p.array_index == 0 ? p.name : fmt::format("{}[{}]", p.name, p.array_index);
            if (has_raw(p.value))
            {
                ++totals.placeholders;
                totals.diagnostics.warning("decompiler.raw-default",
                                           fmt::format("{}: value of '{}' kept as raw bytes", package.name(), name));
                out += pad + "// " + name + "=" + format_property_value(p.value, format) + "\n";
                continue;
            }
            if (const auto* array = std::get_if<ArrayValue>(&p.value.value))
            {
                if (array->elements.empty())
                {
                    out += pad + name + ".Empty\n";
                }
                for (std::size_t i = 0; i < array->elements.size(); ++i)
                {
                    out += pad + fmt::format("{}({})=", name, i) + format_property_value(array->elements[i], format) +
                           "\n";
                }
                continue;
            }
            out += pad + name + "=" + format_property_value(p.value, format) + "\n";
        }
    }

    void ScriptDecompiler::property_lines(ObjectRef object, int depth, std::string& out, DecompiledScript& totals,
                                          int nesting)
    {
        const Package& package = *object.package;
        // Subobjects (components and other instanced objects) are the exports whose outer is this object.
        std::vector<ObjectIndex> subobjects;
        std::set<std::int32_t> subobject_set;
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            if (package.exports()[i].outer == object.index)
            {
                subobjects.push_back(ObjectIndex::from_export(i));
                subobject_set.insert(ObjectIndex::from_export(i).value);
            }
        }
        const std::string pad = indentation(depth);
        if (nesting < max_subobject_nesting)
        {
            for (const ObjectIndex sub : subobjects)
            {
                out += pad + "Begin Object Class=" + package.class_name(sub) + " Name=" + package.object_name(sub) + "\n";
                property_lines(ObjectRef{&package, sub}, depth + 1, out, totals, nesting + 1);
                out += pad + "End Object\n";
            }
        }
        try
        {
            const ObjectProperties props = m_linker.read_properties(object, m_schema);
            format_properties(package, props.properties, subobject_set, depth, out, totals);
        }
        catch (const Error& error)
        {
            ++totals.placeholders;
            totals.diagnostics.warning("decompiler.default-properties",
                                       m_linker.qualified_path(object) + ": " + error.what());
            out += pad + "// properties could not be read: " + error.what() + "\n";
        }
    }

    std::string ScriptDecompiler::properties_block(ObjectRef object, std::string_view keyword, int depth,
                                                   DecompiledScript& totals)
    {
        std::string inner;
        property_lines(object, depth + 1, inner, totals);
        if (inner.empty())
        {
            return {};
        }
        return indentation(depth) + std::string(keyword) + "\n" + indentation(depth) + "{\n" + inner +
               indentation(depth) + "}\n";
    }

    DecompiledScript ScriptDecompiler::decompile_class(ObjectRef object)
    {
        if (!object.is_export())
        {
            throw Error(ErrorCode::InvalidArgument, "decompile_class() requires an export reference");
        }
        const Package& package = *object.package;
        const FieldObject field = m_linker.read_field(object);
        if (field.kind != FieldKind::Class || !field.class_data)
        {
            throw Error(ErrorCode::InvalidArgument,
                        fmt::format("'{}' is a {}, not a class", m_linker.qualified_path(object), to_string(field.kind)));
        }
        const std::string name = package.object_name(object.index);

        DecompiledScript result;
        std::string out = class_header(object, field);

        // Members by kind. The children list is in reverse declaration order except for variables (upstream
        // UClass/UStruct decompilers enumerate them the same way).
        const std::vector<ObjectRef> members = children(object);
        std::vector<ObjectRef> constants, enums, structs, variables, functions, states;
        for (const ObjectRef& member : members)
        {
            const std::string kind = package.class_name(member.index);
            if (kind == "Const") constants.push_back(member);
            else if (kind == "Enum") enums.push_back(member);
            else if (kind == "ScriptStruct") structs.push_back(member);
            else if (kind == "Function") functions.push_back(member);
            else if (kind == "State") states.push_back(member);
            else if (field_kind_of(kind) == FieldKind::Property) variables.push_back(member);
        }
        for (auto* list : {&constants, &enums, &structs, &functions, &states})
        {
            std::reverse(list->begin(), list->end());
        }

        if (!constants.empty())
        {
            out += "\n";
            for (const ObjectRef& c : constants) out += const_source(c, 0);
        }
        for (const ObjectRef& e : enums) out += "\n" + enum_source(e, 0);
        for (const ObjectRef& s : structs) out += "\n" + struct_source(s, 0, result);
        if (!variables.empty())
        {
            out += "\n";
            for (const ObjectRef& v : variables) out += variable_declaration(v, name) + "\n";
        }
        if (const std::string block = replication(object, field, variables, result); !block.empty())
        {
            out += "\n" + block;
        }
        for (const ObjectRef& f : functions)
        {
            const DecompiledScript function = decompile(f);
            accumulate(result, function);
            out += "\n" + function.source;
        }
        for (const ObjectRef& s : states)
        {
            const DecompiledScript state = decompile(s);
            accumulate(result, state);
            out += "\n" + state.source;
        }
        if (!field.class_data->default_object.is_null() && field.class_data->default_object.is_export())
        {
            const std::string defaults =
                properties_block(ObjectRef{&package, field.class_data->default_object}, "defaultproperties", 0, result);
            if (!defaults.empty())
            {
                out += "\n" + defaults;
            }
        }
        result.source = std::move(out);
        return result;
    }
}
