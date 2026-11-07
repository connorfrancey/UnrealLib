#include <UnrealLib/T3D/T3D.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <set>
#include <unordered_set>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib::T3D
{
    std::optional<std::string> T3DBlock::attribute(std::string_view key) const
    {
        for (const auto& [k, v] : attributes)
        {
            if (Text::iequals(k, key)) return v;
        }
        return std::nullopt;
    }

    const T3DProperty* T3DBlock::property(std::string_view name, std::optional<std::int32_t> index) const
    {
        for (const T3DProperty& p : properties)
        {
            if (Text::iequals(p.name, name) && p.index == index) return &p;
        }
        return nullptr;
    }

    namespace
    {
        [[noreturn]] void fail(std::size_t line, const std::string& message)
        {
            throw Error(ErrorCode::InvalidData, fmt::format("T3D line {}: {}", line, message));
        }

        std::string_view trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
            return s;
        }

        bool starts_with_word(std::string_view s, std::string_view word)
        {
            return s.size() >= word.size() && Text::iequals(s.substr(0, word.size()), word) &&
                   (s.size() == word.size() || s[word.size()] == ' ' || s[word.size()] == '\t');
        }

        /// Position of the first `=` at nesting level 0 (outside parentheses, double quotes and Class'...' names),
        /// or npos. Throws for an unterminated double-quoted string anywhere on the line.
        std::size_t top_level_equals(std::string_view s, std::size_t line)
        {
            std::size_t found = std::string_view::npos;
            int depth = 0;
            bool in_string = false, in_name = false;
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                const char c = s[i];
                if (in_string)
                {
                    if (c == '\\') ++i;
                    else if (c == '"') in_string = false;
                    continue;
                }
                if (in_name)
                {
                    if (c == '\'') in_name = false;
                    continue;
                }
                if (c == '"') in_string = true;
                else if (c == '\'') in_name = true;
                else if (c == '(' || c == '[') ++depth;
                else if ((c == ')' || c == ']') && depth > 0) --depth;
                else if (c == '=' && depth == 0 && found == std::string_view::npos) found = i;
            }
            if (in_string) fail(line, "unterminated string");
            return found;
        }

        /// Splits a Begin line's attribute list on whitespace outside quotes, parentheses and names.
        std::vector<std::pair<std::string, std::string>> parse_attributes(std::string_view s, std::size_t line)
        {
            std::vector<std::pair<std::string, std::string>> out;
            while (!(s = trim(s)).empty())
            {
                int depth = 0;
                bool in_string = false, in_name = false;
                std::size_t end = 0;
                for (; end < s.size(); ++end)
                {
                    const char c = s[end];
                    if (in_string) { if (c == '\\') ++end; else if (c == '"') in_string = false; continue; }
                    if (in_name) { if (c == '\'') in_name = false; continue; }
                    if (c == '"') in_string = true;
                    else if (c == '\'') in_name = true;
                    else if (c == '(') ++depth;
                    else if (c == ')' && depth > 0) --depth;
                    else if ((c == ' ' || c == '\t') && depth == 0) break;
                }
                if (in_string || in_name) fail(line, "unterminated quote in Begin line");
                const std::string_view token = s.substr(0, end);
                const std::size_t eq = token.find('=');
                if (eq == std::string_view::npos || eq == 0)
                {
                    fail(line, fmt::format("expected Key=Value in Begin line, found '{}'", token));
                }
                out.emplace_back(std::string(token.substr(0, eq)), std::string(token.substr(eq + 1)));
                s.remove_prefix(end);
            }
            return out;
        }

        T3DProperty parse_property(std::string_view s, std::size_t eq, std::size_t line)
        {
            T3DProperty p;
            p.line = line;
            std::string_view lhs = trim(s.substr(0, eq));
            p.value = std::string(trim(s.substr(eq + 1)));
            if (lhs.empty()) fail(line, "property without a name");
            const char close = lhs.back();
            if (close == ')' || close == ']')
            {
                const char open = close == ')' ? '(' : '[';
                const std::size_t start = lhs.find(open);
                if (start == std::string_view::npos || start == 0) fail(line, "malformed property index");
                const std::string_view digits = lhs.substr(start + 1, lhs.size() - start - 2);
                std::int32_t index = 0;
                const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
                if (ec != std::errc{} || ptr != digits.data() + digits.size() || index < 0)
                {
                    fail(line, fmt::format("invalid property index '{}'", digits));
                }
                p.index = index;
                p.bracket_index = close == ']';
                lhs = lhs.substr(0, start);
            }
            for (const char c : lhs)
            {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                {
                    fail(line, fmt::format("invalid property name '{}'", lhs));
                }
            }
            p.name = std::string(lhs);
            return p;
        }
    }

    T3DDocument parse(std::string_view text)
    {
        T3DDocument doc;
        std::vector<T3DBlock> stack;
        std::size_t line_number = 0;
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            ++line_number;
            const std::string_view line = trim(text.substr(start, end - start));
            start = end + 1;
            if (line.empty())
            {
                if (end == text.size()) break;
                continue;
            }
            if (starts_with_word(line, "Begin"))
            {
                std::string_view rest = trim(line.substr(5));
                std::size_t type_end = 0;
                while (type_end < rest.size() && rest[type_end] != ' ' && rest[type_end] != '\t') ++type_end;
                if (type_end == 0 || rest.substr(0, type_end).find('=') != std::string_view::npos)
                {
                    fail(line_number, "Begin without a block type");
                }
                T3DBlock block;
                block.type = std::string(rest.substr(0, type_end));
                block.attributes = parse_attributes(rest.substr(type_end), line_number);
                block.line = line_number;
                stack.push_back(std::move(block));
            }
            else if (starts_with_word(line, "End"))
            {
                const std::string_view type = trim(line.substr(3));
                if (stack.empty()) fail(line_number, fmt::format("'End {}' without a matching Begin", type));
                if (!Text::iequals(type, stack.back().type))
                {
                    fail(line_number, fmt::format("'End {}' closes 'Begin {}' (line {})", type, stack.back().type,
                                                  stack.back().line));
                }
                T3DBlock done = std::move(stack.back());
                stack.pop_back();
                if (stack.empty())
                {
                    doc.blocks.push_back(std::move(done));
                }
                else
                {
                    stack.back().children.push_back(std::move(done));
                    stack.back().layout += 'c';
                }
            }
            else
            {
                if (stack.empty()) fail(line_number, "content outside a Begin/End block");
                const std::size_t eq = top_level_equals(line, line_number);
                if (eq == std::string_view::npos)
                {
                    stack.back().lines.emplace_back(line);
                    stack.back().layout += 'l';
                }
                else
                {
                    stack.back().properties.push_back(parse_property(line, eq, line_number));
                    stack.back().layout += 'p';
                }
            }
            if (end == text.size()) break;
        }
        if (!stack.empty())
        {
            fail(stack.back().line, fmt::format("'Begin {}' is never closed", stack.back().type));
        }
        return doc;
    }

    namespace
    {
        void serialize_block(const T3DBlock& b, std::string_view indent, int depth, std::string& out)
        {
            std::string pad;
            for (int i = 0; i < depth; ++i) pad += indent;
            out += pad + "Begin " + b.type;
            for (const auto& [k, v] : b.attributes) out += " " + k + "=" + v;
            out += "\n";
            const std::string inner = pad + std::string(indent);
            const auto property = [&](const T3DProperty& p) {
                out += inner + p.name;
                if (p.index) out += p.bracket_index ? fmt::format("[{}]", *p.index) : fmt::format("({})", *p.index);
                out += "=" + p.value + "\n";
            };
            std::size_t pc = 0, lc = 0, cc = 0;
            for (const char k : b.layout)
            {
                pc += k == 'p';
                lc += k == 'l';
                cc += k == 'c';
            }
            if (pc == b.properties.size() && lc == b.lines.size() && cc == b.children.size() &&
                b.layout.size() == pc + lc + cc)
            {
                std::size_t p = 0, l = 0, c = 0;
                for (const char k : b.layout)
                {
                    if (k == 'p') property(b.properties[p++]);
                    else if (k == 'l') out += inner + b.lines[l++] + "\n";
                    else serialize_block(b.children[c++], indent, depth + 1, out);
                }
            }
            else
            {
                for (const T3DProperty& p : b.properties) property(p);
                for (const std::string& l : b.lines) out += inner + l + "\n";
                for (const T3DBlock& c : b.children) serialize_block(c, indent, depth + 1, out);
            }
            out += pad + "End " + b.type + "\n";
        }

        void validate_block(const T3DBlock& b, const T3DBlock* actor, Diagnostics& d)
        {
            const auto where = [&](const T3DBlock& x) { return x.line != 0 ? fmt::format("line {}: ", x.line) : ""; };
            if (Text::iequals(b.type, "Actor"))
            {
                if (!b.attribute("Class")) d.warning("t3d.actor-without-class", where(b) + "Actor without Class=");
                if (!b.attribute("Name")) d.warning("t3d.actor-without-name", where(b) + "Actor without Name=");
                actor = &b;
            }
            if (Text::iequals(b.type, "Map") &&
                std::none_of(b.children.begin(), b.children.end(),
                             [](const T3DBlock& c) { return Text::iequals(c.type, "Level"); }))
            {
                d.warning("t3d.map-without-level", where(b) + "Map without a Level block");
            }
            std::unordered_set<std::string, Text::CaseInsensitiveHash, Text::CaseInsensitiveEqual> names;
            for (const T3DBlock& c : b.children)
            {
                if (const auto name = c.attribute("Name"))
                {
                    if (!names.insert(*name).second)
                    {
                        d.warning("t3d.duplicate-name",
                                  where(c) + fmt::format("'{}' is defined twice in the same {}", *name, b.type));
                    }
                }
            }
            if (actor == &b)
            {
                // A bare identifier value that names no subobject of the actor and is not a keyword is suspicious
                // only when the property is a known component slot; values of other types cannot be checked here.
                std::unordered_set<std::string, Text::CaseInsensitiveHash, Text::CaseInsensitiveEqual> subobjects;
                for (const T3DBlock& c : b.children)
                {
                    if (const auto name = c.attribute("Name")) subobjects.insert(*name);
                }
                for (const T3DProperty& p : b.properties)
                {
                    if (Text::iequals(p.name, "Components") && p.value != "None" &&
                        p.value.find('\'') == std::string::npos && !subobjects.contains(p.value))
                    {
                        d.warning("t3d.unresolved-subobject",
                                  fmt::format("line {}: {} refers to '{}', which is not defined in the actor", p.line,
                                              p.name, p.value));
                    }
                }
            }
            for (const T3DBlock& c : b.children) validate_block(c, actor, d);
        }
    }

    std::string serialize(const T3DDocument& document, std::string_view indent)
    {
        std::string out;
        for (const T3DBlock& b : document.blocks) serialize_block(b, indent, 0, out);
        return out;
    }

    Diagnostics validate(const T3DDocument& document)
    {
        Diagnostics d;
        for (const T3DBlock& b : document.blocks) validate_block(b, nullptr, d);
        return d;
    }

    namespace
    {
        std::string reference(const Package& package, ObjectIndex index, const std::set<std::int32_t>& local)
        {
            if (index.is_null() || !package.is_valid(index)) return "None";
            if (local.contains(index.value)) return package.object_name(index);
            const std::string path =
                index.is_export() ? package.name() + "." + package.object_path(index) : package.object_path(index);
            return package.class_name(index) + "'" + path + "'";
        }

        bool has_raw(const PropertyValue& v)
        {
            if (std::holds_alternative<RawValue>(v.value)) return true;
            if (const auto* a = std::get_if<ArrayValue>(&v.value))
            {
                return !a->decoded() || std::any_of(a->elements.begin(), a->elements.end(), has_raw);
            }
            if (const auto* s = std::get_if<StructValue>(&v.value))
            {
                return std::any_of(s->fields.begin(), s->fields.end(),
                                   [](const Property& f) { return has_raw(f.value); });
            }
            return false;
        }

        /// Exports of a package grouped by their outer export (index = outer's export index).
        std::vector<std::vector<ObjectIndex>> children_by_outer(const Package& package)
        {
            std::vector<std::vector<ObjectIndex>> out(package.exports().size());
            for (std::size_t i = 0; i < package.exports().size(); ++i)
            {
                const ObjectIndex outer = package.exports()[i].outer;
                if (outer.is_export() && outer.export_index() < out.size())
                {
                    out[outer.export_index()].push_back(ObjectIndex::from_export(i));
                }
            }
            return out;
        }

        void export_into(Linker& linker, ObjectRef object, T3DBlock& block, const ExportOptions& options,
                         const std::vector<std::vector<ObjectIndex>>& children, Diagnostics* diagnostics, int depth)
        {
            const Package& package = *object.package;
            const std::vector<ObjectIndex>& subobjects = children[object.index.export_index()];
            std::set<std::int32_t> local;
            for (const ObjectIndex sub : subobjects) local.insert(sub.value);
            if (depth < options.max_subobject_depth)
            {
                for (const ObjectIndex sub : subobjects)
                {
                    T3DBlock child;
                    child.type = "Object";
                    child.attributes.emplace_back("Class", package.class_name(sub));
                    child.attributes.emplace_back("Name", package.object_name(sub));
                    const ObjectIndex archetype = package.exports()[sub.export_index()].archetype;
                    if (!archetype.is_null()) child.attributes.emplace_back("Archetype", reference(package, archetype, {}));
                    export_into(linker, ObjectRef{&package, sub}, child, options, children, diagnostics, depth + 1);
                    block.children.push_back(std::move(child));
                    block.layout += 'c';
                }
            }
            PropertyFormatOptions format;
            format.names = &package.names();
            format.object_name = [&](ObjectIndex i) { return reference(package, i, local); };
            const ObjectProperties props = linker.read_properties(object, options.schema);
            for (const Property& p : props.properties)
            {
                if (has_raw(p.value) && diagnostics != nullptr)
                {
                    diagnostics->warning("t3d.raw-value", fmt::format("{}: {} kept as raw bytes",
                                                                      linker.qualified_path(object), p.name));
                }
                const auto add = [&](std::optional<std::int32_t> index, const PropertyValue& value) {
                    T3DProperty out;
                    out.name = p.name;
                    out.index = index;
                    out.value = format_property_value(value, format);
                    block.properties.push_back(std::move(out));
                    block.layout += 'p';
                };
                const auto* array = std::get_if<ArrayValue>(&p.value.value);
                if (array != nullptr && array->decoded())
                {
                    for (std::size_t i = 0; i < array->elements.size(); ++i)
                    {
                        add(static_cast<std::int32_t>(i), array->elements[i]);
                    }
                }
                else
                {
                    add(p.array_index != 0 ? std::optional<std::int32_t>(p.array_index) : std::nullopt, p.value);
                }
            }
        }

        T3DBlock export_block(Linker& linker, ObjectRef object, std::string_view block_type,
                              const ExportOptions& options, const std::vector<std::vector<ObjectIndex>>& children,
                              Diagnostics* diagnostics)
        {
            const Package& package = *object.package;
            T3DBlock block;
            block.type = std::string(block_type);
            block.attributes.emplace_back("Class", package.class_name(object.index));
            block.attributes.emplace_back("Name", package.object_name(object.index));
            const ObjectIndex archetype = object.export_entry().archetype;
            if (!archetype.is_null()) block.attributes.emplace_back("Archetype", reference(package, archetype, {}));
            export_into(linker, object, block, options, children, diagnostics, 0);
            return block;
        }
    }

    T3DBlock export_object(Linker& linker, ObjectRef object, std::string_view block_type, const ExportOptions& options,
                           Diagnostics* diagnostics)
    {
        if (!object.is_export())
        {
            throw Error(ErrorCode::InvalidArgument, "export_object() requires an export");
        }
        return export_block(linker, object, block_type, options, children_by_outer(*object.package), diagnostics);
    }

    T3DDocument export_level(Linker& linker, const Package& package, const ExportOptions& options,
                             Diagnostics* diagnostics)
    {
        const auto children = children_by_outer(package);
        T3DBlock map;
        map.type = "Map";
        map.attributes.emplace_back("Name", package.name());
        for (std::size_t i = 0; i < package.exports().size(); ++i)
        {
            const ObjectIndex level_index = ObjectIndex::from_export(i);
            if (package.class_name(level_index) != "Level" ||
                (package.exports()[i].object_flags & ObjectFlag::ClassDefaultObject) != 0)
            {
                continue;
            }
            T3DBlock level;
            level.type = "Level";
            level.attributes.emplace_back("NAME", package.object_name(level_index));
            for (const ObjectIndex a : children[i])
            {
                const ObjectRef actor{&package, a};
                if (!linker.is_a(actor, "Engine.Actor").value_or(false)) continue;
                level.children.push_back(export_block(linker, actor, "Actor", options, children, diagnostics));
                level.layout += 'c';
            }
            map.children.push_back(std::move(level));
            map.layout += 'c';
        }
        if (map.children.empty())
        {
            throw Error(ErrorCode::NotFound, fmt::format("package '{}' has no Level", package.name()));
        }
        T3DDocument doc;
        doc.blocks.push_back(std::move(map));
        return doc;
    }
}
