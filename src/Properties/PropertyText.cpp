// Parsing of property values from format_property_value() text (the inverse of the formatter).

#include <UnrealLib/Properties/PropertyValue.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <charconv>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>

namespace UnrealLib
{
    namespace
    {
        [[noreturn]] void bad(std::string_view text, std::string_view what)
        {
            throw Error(ErrorCode::InvalidData, fmt::format("'{}': {}", text, what));
        }

        std::string_view trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
            return s;
        }

        /// Splits the inside of a parenthesised list on top-level commas (outside nested parentheses, double-quoted
        /// strings with backslash escapes and single-quoted reference paths). "()" yields no items.
        std::vector<std::string_view> split_list(std::string_view text)
        {
            const std::string_view t = trim(text);
            if (t.size() < 2 || t.front() != '(' || t.back() != ')') bad(text, "expected a parenthesised list");
            const std::string_view inner = t.substr(1, t.size() - 2);
            std::vector<std::string_view> items;
            if (trim(inner).empty()) return items;
            int depth = 0;
            bool in_string = false;
            bool in_path = false;
            std::size_t start = 0;
            for (std::size_t i = 0; i < inner.size(); ++i)
            {
                const char c = inner[i];
                if (in_string)
                {
                    if (c == '\\') ++i;
                    else if (c == '"') in_string = false;
                    continue;
                }
                if (in_path)
                {
                    if (c == '\'') in_path = false;
                    continue;
                }
                if (c == '"') in_string = true;
                else if (c == '\'') in_path = true;
                else if (c == '(') ++depth;
                else if (c == ')')
                {
                    if (--depth < 0) bad(text, "unbalanced parentheses");
                }
                else if (c == ',' && depth == 0)
                {
                    items.push_back(trim(inner.substr(start, i - start)));
                    start = i + 1;
                }
            }
            if (in_string || in_path || depth != 0) bad(text, "unterminated string, path or parentheses");
            items.push_back(trim(inner.substr(start)));
            return items;
        }

        /// Splits `Name=value` or `Name[i]=value` at the first top-level '='.
        struct FieldText
        {
            std::string_view name;
            std::int32_t index = 0;
            bool indexed = false;
            std::string_view value;
        };

        FieldText split_field(std::string_view item)
        {
            const std::size_t eq = item.find('=');
            if (eq == std::string_view::npos) bad(item, "expected Field=Value");
            FieldText f;
            std::string_view key = trim(item.substr(0, eq));
            f.value = trim(item.substr(eq + 1));
            if (!key.empty() && (key.back() == ']' || key.back() == ')'))
            {
                const std::size_t open = key.find_first_of("[(");
                if (open == std::string_view::npos) bad(item, "malformed field index");
                const std::string_view digits = key.substr(open + 1, key.size() - open - 2);
                const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), f.index);
                if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size() || f.index < 0)
                {
                    bad(item, "malformed field index");
                }
                f.indexed = true;
                key = trim(key.substr(0, open));
            }
            if (key.empty()) bad(item, "empty field name");
            f.name = key;
            return f;
        }

        template <class T>
        T parse_integer(std::string_view text, std::string_view what)
        {
            std::string_view t = trim(text);
            if (!t.empty() && t.front() == '+') t.remove_prefix(1);
            T value{};
            const auto r = std::from_chars(t.data(), t.data() + t.size(), value);
            if (t.empty() || r.ec != std::errc{} || r.ptr != t.data() + t.size()) bad(text, what);
            return value;
        }

        float parse_float(std::string_view text)
        {
            std::string_view t = trim(text);
            if (!t.empty() && t.front() == '+') t.remove_prefix(1);
            // format_float() writes NaNs other than the default quiet ones as nan(0xBITS).
            if (t.size() == 15 && Text::iequals(t.substr(0, 6), "nan(0x") && t.back() == ')')
            {
                std::uint32_t bits = 0;
                const std::string_view hex = t.substr(6, 8);
                const auto r = std::from_chars(hex.data(), hex.data() + hex.size(), bits, 16);
                const float value = std::bit_cast<float>(bits);
                if (r.ec != std::errc{} || r.ptr != hex.data() + hex.size() || !std::isnan(value))
                {
                    bad(text, "malformed NaN bits");
                }
                return value;
            }
            if (Text::iequals(t, "nan")) return std::bit_cast<float>(0x7FC00000u);
            if (Text::iequals(t, "-nan")) return std::bit_cast<float>(0xFFC00000u);
            float value = 0.0f;
            const auto r = std::from_chars(t.data(), t.data() + t.size(), value);
            if (t.empty() || r.ec != std::errc{} || r.ptr != t.data() + t.size()) bad(text, "expected a float");
            return value;
        }

        std::string unquote(std::string_view text)
        {
            const std::string_view t = trim(text);
            if (t.size() < 2 || t.front() != '"' || t.back() != '"') bad(text, "expected a quoted string");
            std::string out;
            out.reserve(t.size() - 2);
            for (std::size_t i = 1; i + 1 < t.size(); ++i)
            {
                char c = t[i];
                if (c == '\\')
                {
                    if (i + 2 >= t.size()) bad(text, "dangling escape");
                    c = t[++i];
                }
                else if (c == '"')
                {
                    bad(text, "unescaped quote inside a string");
                }
                out += c;
            }
            return out;
        }

        class Parser
        {
        public:
            explicit Parser(const PropertyParseOptions& options) : m_options(options)
            {
                if (!m_options.make_name) throw Error(ErrorCode::InvalidArgument, "PropertyParseOptions::make_name is required");
            }

            const PropertySchema& schema() const
            {
                return m_options.schema != nullptr ? *m_options.schema : BuiltinPropertySchema::instance();
            }

            FName name(std::string_view text) const { return m_options.make_name(text); }

            ObjectIndex object(std::string_view text) const
            {
                const std::string_view t = trim(text);
                if (t.empty()) bad(text, "expected an object reference");
                if (Text::iequals(t, "None")) return ObjectIndex{};
                if (!m_options.resolve_object) bad(text, "object references cannot be resolved (no resolver)");
                const std::optional<ObjectIndex> index = m_options.resolve_object(t);
                if (!index) bad(text, "unresolved object reference");
                return *index;
            }

            PropertyValue default_value(const TypeDesc& type) const
            {
                PropertyValue v;
                switch (type.type)
                {
                case PropertyType::Byte:
                    if (type.is_enum) v.value = EnumValue{name("None")};
                    else v.value = std::uint8_t{0};
                    break;
                case PropertyType::Int: v.value = std::int32_t{0}; break;
                case PropertyType::Bool: v.value = false; break;
                case PropertyType::Float: v.value = 0.0f; break;
                case PropertyType::Name: v.value = NameValue{name("None")}; break;
                case PropertyType::Str:
                case PropertyType::String: v.value = FString{}; break;
                case PropertyType::Object:
                case PropertyType::Class:
                case PropertyType::Component:
                case PropertyType::Interface: v.value = ObjectValue{}; break;
                case PropertyType::Delegate: v.value = DelegateValue{ObjectIndex{}, name("None")}; break;
                case PropertyType::Array: v.value = ArrayValue{}; break;
                case PropertyType::Struct:
                {
                    StructValue s;
                    s.struct_name = type.struct_name;
                    if (const StructLayout* layout = schema().find_struct(type.struct_name); layout && layout->binary)
                    {
                        s.encoding = StructEncoding::Binary;
                        for (const StructField& field : layout->fields)
                        {
                            for (std::int32_t i = 0; i < std::max(field.array_dim, 1); ++i)
                            {
                                Property p;
                                p.name = field.name;
                                p.array_index = i;
                                p.value = default_value(field.type);
                                s.fields.push_back(std::move(p));
                            }
                        }
                    }
                    v.value = std::move(s);
                    break;
                }
                default:
                    throw UnsupportedError(
                        fmt::format("no default value for {} values", to_string(type.type)));
                }
                return v;
            }

            PropertyValue parse(std::string_view text, const PropertyValue& like, const TypeDesc* type) const
            {
                PropertyValue out;
                std::visit([&](const auto& t) { out.value = parse_as(text, t, type); }, like.value);
                return out;
            }

        private:
            PropertyValue::Variant parse_as(std::string_view text, std::monostate, const TypeDesc*) const
            {
                if (!trim(text).empty()) bad(text, "expected no value");
                return std::monostate{};
            }

            PropertyValue::Variant parse_as(std::string_view text, bool, const TypeDesc*) const
            {
                const std::string_view t = trim(text);
                if (Text::iequals(t, "True") || t == "1") return true;
                if (Text::iequals(t, "False") || t == "0") return false;
                bad(text, "expected True or False");
            }

            PropertyValue::Variant parse_as(std::string_view text, std::uint8_t, const TypeDesc*) const
            {
                return parse_integer<std::uint8_t>(text, "expected a byte (0-255)");
            }

            PropertyValue::Variant parse_as(std::string_view text, const EnumValue&, const TypeDesc*) const
            {
                const std::string_view t = trim(text);
                if (t.empty()) bad(text, "expected an enumerator");
                return EnumValue{name(t)};
            }

            PropertyValue::Variant parse_as(std::string_view text, std::int32_t, const TypeDesc*) const
            {
                return parse_integer<std::int32_t>(text, "expected an integer");
            }

            PropertyValue::Variant parse_as(std::string_view text, float, const TypeDesc*) const
            {
                return parse_float(text);
            }

            PropertyValue::Variant parse_as(std::string_view text, const NameValue&, const TypeDesc*) const
            {
                return NameValue{name(unquote(text))};
            }

            PropertyValue::Variant parse_as(std::string_view text, const FString& like, const TypeDesc*) const
            {
                return FString(unquote(text), like.encoding, like.null_terminated);
            }

            PropertyValue::Variant parse_as(std::string_view text, const ObjectValue&, const TypeDesc*) const
            {
                return ObjectValue{object(text)};
            }

            PropertyValue::Variant parse_as(std::string_view text, const DelegateValue&, const TypeDesc*) const
            {
                // `Function` or `<object>.Function`; the object part may itself contain dots (`Class'P.O'`).
                const std::string_view t = trim(text);
                const std::size_t quote = t.rfind('\'');
                const std::size_t dot = t.rfind('.');
                if (dot == std::string_view::npos || (quote != std::string_view::npos && dot < quote))
                {
                    if (t.empty()) bad(text, "expected a delegate");
                    return DelegateValue{ObjectIndex{}, name(t)};
                }
                return DelegateValue{object(t.substr(0, dot)), name(t.substr(dot + 1))};
            }

            PropertyValue::Variant parse_as(std::string_view text, const RawValue& like, const TypeDesc*) const
            {
                if (trim(text) != fmt::format("<{} raw bytes>", like.bytes.size()))
                {
                    bad(text, "a value kept as raw bytes cannot be changed as text");
                }
                return like;
            }

            PropertyValue::Variant parse_as(std::string_view text, const ArrayValue& like, const TypeDesc* type) const
            {
                if (like.raw)
                {
                    if (trim(text) != fmt::format("<{} elements, {} raw bytes>", like.count, like.raw->size()))
                    {
                        bad(text, "an array kept as raw bytes cannot be changed as text");
                    }
                    return like;
                }
                const TypeDesc* inner = type != nullptr && type->type == PropertyType::Array ? type->inner.get() : nullptr;
                const std::vector<std::string_view> items = split_list(text);
                ArrayValue out;
                out.elements.reserve(items.size());
                std::optional<PropertyValue> fallback;
                for (std::size_t i = 0; i < items.size(); ++i)
                {
                    const PropertyValue* element = nullptr;
                    if (!like.elements.empty()) element = &like.elements[std::min(i, like.elements.size() - 1)];
                    else if (inner != nullptr)
                    {
                        if (!fallback) fallback = default_value(*inner);
                        element = &*fallback;
                    }
                    else bad(text, "element type of an empty array is unknown");
                    out.elements.push_back(parse(items[i], *element, inner));
                }
                out.count = static_cast<std::int32_t>(out.elements.size());
                return out;
            }

            PropertyValue::Variant parse_as(std::string_view text, const StructValue& like, const TypeDesc* type) const
            {
                std::string struct_name = like.struct_name;
                if (struct_name.empty() && type != nullptr) struct_name = type->struct_name;
                const StructLayout* layout = struct_name.empty() ? nullptr : schema().find_struct(struct_name);
                const auto layout_field = [&](std::string_view field) -> const StructField* {
                    if (layout == nullptr) return nullptr;
                    for (const StructField& f : layout->fields)
                    {
                        if (Text::iequals(f.name, field)) return &f;
                    }
                    return nullptr;
                };
                const auto template_field = [&](auto& fields, const FieldText& f) {
                    return std::find_if(fields.begin(), fields.end(), [&](const Property& p) {
                        return p.array_index == f.index && Text::iequals(p.name, f.name);
                    });
                };

                StructValue out;
                out.struct_name = like.struct_name;
                out.encoding = like.encoding;
                if (like.encoding == StructEncoding::Binary)
                {
                    out.fields = like.fields;
                    for (const std::string_view item : split_list(text))
                    {
                        const FieldText f = split_field(item);
                        const auto it = template_field(out.fields, f);
                        if (it == out.fields.end()) bad(item, fmt::format("struct {} has no field {}", struct_name, f.name));
                        const StructField* lf = layout_field(f.name);
                        it->value = parse(f.value, it->value, lf != nullptr ? &lf->type : nullptr);
                    }
                    return out;
                }
                for (const std::string_view item : split_list(text))
                {
                    const FieldText f = split_field(item);
                    const StructField* lf = layout_field(f.name);
                    const auto it = template_field(like.fields, f);
                    Property p;
                    if (it != like.fields.end())
                    {
                        p = *it;
                        p.value = parse(f.value, it->value, lf != nullptr ? &lf->type : nullptr);
                    }
                    else if (lf != nullptr)
                    {
                        p.name = lf->name;
                        p.array_index = f.index;
                        p.value = parse(f.value, default_value(lf->type), &lf->type);
                        p.tag = make_property_tag(lf->name, f.index, lf->type, m_options.make_name);
                    }
                    else
                    {
                        bad(item, fmt::format("unknown field {} of struct {}", f.name,
                                              struct_name.empty() ? "<unnamed>" : struct_name));
                    }
                    out.fields.push_back(std::move(p));
                }
                return out;
            }

            const PropertyParseOptions& m_options;
        };
    }

    PropertyValue parse_property_value(std::string_view text, const PropertyValue& like,
                                       const PropertyParseOptions& options, const TypeDesc* type)
    {
        return Parser(options).parse(text, like, type);
    }

    PropertyValue parse_property_value(std::string_view text, const TypeDesc& type, const PropertyParseOptions& options)
    {
        const Parser parser(options);
        return parser.parse(text, parser.default_value(type), &type);
    }

    PropertyValue default_property_value(const TypeDesc& type, const PropertyParseOptions& options)
    {
        return Parser(options).default_value(type);
    }

    PropertyTag make_property_tag(std::string_view name, std::int32_t array_index, const TypeDesc& type,
                                  const std::function<FName(std::string_view)>& make_name)
    {
        if (type.type == PropertyType::Unknown) throw UnsupportedError(fmt::format("property {} has an unknown type", name));
        PropertyTag tag;
        tag.name = make_name(name);
        tag.type = type.type;
        tag.type_name = make_name(to_string(type.type));
        tag.array_index = array_index;
        if (type.type == PropertyType::Struct) tag.struct_name = make_name(type.struct_name);
        if (type.type == PropertyType::Bool) tag.bool_value = false;
        return tag;
    }
}
