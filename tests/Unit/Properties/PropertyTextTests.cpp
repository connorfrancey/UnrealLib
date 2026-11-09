// parse_property_value(): the inverse of format_property_value().

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <string>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

using namespace UnrealLib;

namespace
{
    TypeDesc type_of(PropertyType type, std::string struct_name = {})
    {
        TypeDesc desc;
        desc.type = type;
        desc.struct_name = std::move(struct_name);
        return desc;
    }

    /// Names and objects of a pretend package: objects are "#<index>" or the names in `objects`.
    struct Env
    {
        NameTable names;
        std::map<std::string, ObjectIndex> objects{{"Sub", ObjectIndex{3}},
                                                   {"Texture2D'Pkg.Group.Tex'", ObjectIndex{-2}}};
        const PropertySchema* schema = nullptr;

        FName name(std::string_view text) { return FName{names.add(text), 0}; }

        PropertyParseOptions parse_options()
        {
            PropertyParseOptions o;
            o.make_name = [this](std::string_view t) { return name(t); };
            o.resolve_object = [this](std::string_view t) -> std::optional<ObjectIndex> {
                const auto it = objects.find(std::string(t));
                if (it != objects.end()) return it->second;
                if (t.size() > 1 && t.front() == '#') return ObjectIndex{std::stoi(std::string(t.substr(1)))};
                return std::nullopt;
            };
            o.schema = schema;
            return o;
        }

        std::string format(const PropertyValue& v)
        {
            PropertyFormatOptions o;
            o.names = &names;
            o.object_name = [this](ObjectIndex i) {
                for (const auto& [text, index] : objects)
                {
                    if (index == i) return text;
                }
                return "#" + std::to_string(i.value);
            };
            return format_property_value(v, o);
        }

        PropertyValue round_trip(const PropertyValue& v)
        {
            return parse_property_value(format(v), v, parse_options());
        }
    };

    PropertyValue value(PropertyValue::Variant v) { return PropertyValue{std::move(v)}; }

    Property field(std::string name, PropertyValue::Variant v, std::int32_t index = 0)
    {
        Property p;
        p.name = std::move(name);
        p.array_index = index;
        p.value.value = std::move(v);
        return p;
    }

    /// Schema with one tagged struct "Pair" (A: int, B: Vector, C: array of float).
    class TestSchema final : public PropertySchema
    {
    public:
        TestSchema()
        {
            m_pair.name = "Pair";
            m_pair.fields.push_back({"A", type_of(PropertyType::Int), 1});
            m_pair.fields.push_back({"B", type_of(PropertyType::Struct, "Vector"), 1});
            TypeDesc floats = type_of(PropertyType::Array);
            floats.inner = std::make_shared<TypeDesc>(type_of(PropertyType::Float));
            m_pair.fields.push_back({"C", floats, 1});
        }
        const StructLayout* find_struct(std::string_view name) const override
        {
            if (Text::iequals(name, "Pair")) return &m_pair;
            return BuiltinPropertySchema::instance().find_struct(name);
        }
        std::optional<TypeDesc> find_property(std::string_view, std::string_view) const override { return {}; }

    private:
        StructLayout m_pair;
    };
}

TEST(PropertyTextTests, ScalarsRoundTrip)
{
    Env env;
    const std::vector<PropertyValue> values = {
        value(true), value(false), value(std::uint8_t{200}), value(std::int32_t{-123456}), value(0.1f),
        value(-0.0f), value(3.4028235e38f), value(1e-45f), value(std::numeric_limits<float>::infinity()),
        value(NameValue{env.name("Some_Name")}), value(EnumValue{env.name("ROLE_Authority")}),
        value(FString("quote \" and \\ backslash, (paren) = 'tick'")),
        value(FString("wide", StringEncoding::Utf16, true)), value(ObjectValue{}), value(ObjectValue{ObjectIndex{3}}),
        value(ObjectValue{ObjectIndex{-2}}), value(DelegateValue{ObjectIndex{}, env.name("OnFoo")}),
        value(DelegateValue{ObjectIndex{-2}, env.name("OnBar")}), value(std::monostate{})};
    for (const PropertyValue& v : values)
    {
        EXPECT_EQ(env.round_trip(v), v) << env.format(v);
    }
}

TEST(PropertyTextTests, ParsesEditedText)
{
    Env env;
    const PropertyParseOptions o = env.parse_options();
    EXPECT_EQ(parse_property_value(" +42 ", value(std::int32_t{0}), o), value(std::int32_t{42}));
    EXPECT_EQ(parse_property_value("TRUE", value(false), o), value(true));
    EXPECT_EQ(parse_property_value("1.5e2", value(0.0f), o), value(150.0f));
    EXPECT_EQ(parse_property_value("None", value(ObjectValue{ObjectIndex{3}}), o), value(ObjectValue{}));
    // Strings keep the template's on-disk encoding.
    const PropertyValue wide = parse_property_value("\"new\"", value(FString("old", StringEncoding::Utf16, true)), o);
    EXPECT_EQ(std::get<FString>(wide.value).encoding, StringEncoding::Utf16);
}

TEST(PropertyTextTests, BinaryStructsKeepOmittedFields)
{
    Env env;
    StructValue vector{"Vector", StructEncoding::Binary,
                       {field("X", 1.0f), field("Y", 2.0f), field("Z", 3.0f)}};
    EXPECT_EQ(env.round_trip(value(vector)), value(vector));
    const PropertyValue edited = parse_property_value("(Z=-5.25, X = 7)", value(vector), env.parse_options());
    const auto& fields = std::get<StructValue>(edited.value).fields;
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields[0].value, value(7.0f));
    EXPECT_EQ(fields[1].value, value(2.0f));
    EXPECT_EQ(fields[2].value, value(-5.25f));
    EXPECT_THROW((void)parse_property_value("(W=1.0)", value(vector), env.parse_options()), Error);
}

TEST(PropertyTextTests, TaggedStructsFollowTheText)
{
    TestSchema schema;
    Env env;
    env.schema = &schema;
    Property a = field("A", std::int32_t{1});
    a.tag = make_property_tag("A", 0, type_of(PropertyType::Int), [&](std::string_view t) { return env.name(t); });
    StructValue pair{"Pair", StructEncoding::Tagged, {a}};
    EXPECT_EQ(env.round_trip(value(pair)), value(pair));

    // B and C are new: their tags and values come from the schema layout; A is dropped.
    const PropertyValue edited =
        parse_property_value("(B=(X=1.0,Y=2.0,Z=3.0),C=(0.5,1.5))", value(pair), env.parse_options());
    const auto& fields = std::get<StructValue>(edited.value).fields;
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields[0].name, "B");
    ASSERT_TRUE(fields[0].tag.has_value());
    EXPECT_EQ(fields[0].tag->type, PropertyType::Struct);
    EXPECT_EQ(env.names.to_string(*fields[0].tag->struct_name), "Vector");
    EXPECT_EQ(std::get<StructValue>(fields[0].value.value).encoding, StructEncoding::Binary);
    const auto& c = std::get<ArrayValue>(fields[1].value.value);
    EXPECT_EQ(c.count, 2);
    EXPECT_EQ(c.elements[1], value(1.5f));
    EXPECT_THROW((void)parse_property_value("(D=1)", value(pair), env.parse_options()), Error);
}

TEST(PropertyTextTests, ArraysUseExistingElementsOrTheType)
{
    Env env;
    ArrayValue ints;
    ints.elements = {value(std::int32_t{1}), value(std::int32_t{2})};
    ints.count = 2;
    EXPECT_EQ(env.round_trip(value(ints)), value(ints));
    const PropertyValue grown = parse_property_value("(5,6,7)", value(ints), env.parse_options());
    EXPECT_EQ(std::get<ArrayValue>(grown.value).count, 3);
    EXPECT_EQ(std::get<ArrayValue>(parse_property_value("()", value(ints), env.parse_options()).value).count, 0);

    TypeDesc names_type = type_of(PropertyType::Array);
    names_type.inner = std::make_shared<TypeDesc>(type_of(PropertyType::Name));
    EXPECT_THROW((void)parse_property_value("(\"A\")", value(ArrayValue{}), env.parse_options()), Error);
    const PropertyValue names = parse_property_value("(\"A\",\"B\")", names_type, env.parse_options());
    EXPECT_EQ(std::get<ArrayValue>(names.value).elements[1], value(NameValue{env.name("B")}));

    // Nested: an array of structs with commas and parentheses inside strings.
    ArrayValue structs;
    structs.elements = {value(StructValue{"S", StructEncoding::Binary, {field("T", FString("a,b)"))}})};
    structs.count = 1;
    EXPECT_EQ(env.round_trip(value(structs)), value(structs));
}

TEST(PropertyTextTests, RawValuesOnlyAcceptTheirNotation)
{
    Env env;
    const PropertyValue raw = value(RawValue{{std::byte{1}, std::byte{2}}});
    EXPECT_EQ(env.round_trip(raw), raw);
    EXPECT_THROW((void)parse_property_value("3", raw, env.parse_options()), Error);
    ArrayValue raw_array;
    raw_array.count = 4;
    raw_array.raw = std::vector<std::byte>(16);
    EXPECT_EQ(env.round_trip(value(raw_array)), value(raw_array));
    EXPECT_THROW((void)parse_property_value("()", value(raw_array), env.parse_options()), Error);
}

TEST(PropertyTextTests, RejectsMalformedText)
{
    Env env;
    const PropertyParseOptions o = env.parse_options();
    EXPECT_THROW((void)parse_property_value("1.5", value(std::int32_t{0}), o), Error);
    EXPECT_THROW((void)parse_property_value("256", value(std::uint8_t{0}), o), Error);
    EXPECT_THROW((void)parse_property_value("Maybe", value(false), o), Error);
    EXPECT_THROW((void)parse_property_value("unquoted", value(FString()), o), Error);
    EXPECT_THROW((void)parse_property_value("\"a\"b\"", value(FString()), o), Error);
    EXPECT_THROW((void)parse_property_value("Missing'Pkg.Obj'", value(ObjectValue{}), o), Error);
    EXPECT_THROW((void)parse_property_value("(1,2", value(ArrayValue{}), o), Error);
    EXPECT_THROW((void)parse_property_value("(X=\"open)", value(StructValue{}), o), Error);
    EXPECT_THROW((void)default_property_value(type_of(PropertyType::Map), o), UnsupportedError);
}

namespace
{
    rc::Gen<PropertyValue> gen_value(Env& env, int depth)
    {
        using namespace rc;
        std::vector<Gen<PropertyValue>> gens = {
            gen::map(gen::arbitrary<bool>(), [](bool b) { return value(b); }),
            gen::map(gen::arbitrary<std::uint8_t>(), [](std::uint8_t b) { return value(b); }),
            gen::map(gen::arbitrary<std::int32_t>(), [](std::int32_t i) { return value(i); }),
            gen::map(gen::arbitrary<std::uint32_t>(), [](std::uint32_t bits) { return value(std::bit_cast<float>(bits)); }),
            // NaNs (every payload) are frequent enough to matter: the game stores some.
            gen::map(gen::arbitrary<std::uint32_t>(),
                     [](std::uint32_t bits) { return value(std::bit_cast<float>(bits | 0x7F800001u)); }),
            // Valid text (the writer rejects invalid UTF-8): printable ASCII, including quotes, backslashes,
            // commas and parentheses, plus a multi-byte character.
            gen::map(gen::container<std::string>(gen::inRange<char>(32, 127)),
                     [](std::string s) {
                         if (s.size() % 3 == 1) s += "\xC3\xA9";
                         return value(FString(std::move(s)));
                     }),
            gen::map(gen::inRange(-5, 6), [](int i) { return value(ObjectValue{ObjectIndex{i}}); }),
            gen::map(gen::element<std::string>("A", "B_1", "None", "ROLE_Authority"),
                     [&env](const std::string& n) { return value(NameValue{env.name(n)}); }),
        };
        if (depth < 2)
        {
            gens.push_back(gen::map(gen::resize(4, gen::container<std::vector<PropertyValue>>(gen_value(env, depth + 1))),
                                    [](std::vector<PropertyValue> elements) {
                                        // Elements of one array share a type: use the first element's type for all.
                                        ArrayValue a;
                                        for (const PropertyValue& e : elements)
                                        {
                                            if (e.value.index() == elements.front().value.index()) a.elements.push_back(e);
                                        }
                                        a.count = static_cast<std::int32_t>(a.elements.size());
                                        return value(a);
                                    }));
            gens.push_back(gen::map(gen::resize(4, gen::container<std::vector<PropertyValue>>(gen_value(env, depth + 1))),
                                    [](std::vector<PropertyValue> fields) {
                                        StructValue s{"S", StructEncoding::Binary, {}};
                                        for (std::size_t i = 0; i < fields.size(); ++i)
                                        {
                                            s.fields.push_back(Property{"F" + std::to_string(i), 0, fields[i], {}});
                                        }
                                        return value(s);
                                    }));
        }
        return gen::join(gen::elementOf(gens));
    }
}

RC_GTEST_PROP(PropertyTextProperties, FormatParseRoundTrip, ())
{
    // Compared serialized, because operator== treats identical NaNs as different.
    Env env;
    const PropertyValue v = *gen_value(env, 0);
    const auto bytes = [&](const PropertyValue& x) {
        BinaryWriter w;
        write_property_value(w, x, env.name("None"));
        return w.take();
    };
    RC_ASSERT(bytes(env.round_trip(v)) == bytes(v));
}

TEST(PropertyTextTests, NaNsKeepTheirBits)
{
    Env env;
    for (const std::uint32_t bits : {0x7FC00000u, 0xFFC00000u, 0xFFFFFFFFu, 0x7F800001u, 0xFFBFFFFFu})
    {
        const PropertyValue nan = value(std::bit_cast<float>(bits));
        const PropertyValue back = env.round_trip(nan);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(std::get<float>(back.value)), bits) << env.format(nan);
    }
    EXPECT_EQ(env.format(value(std::bit_cast<float>(0x7FC00000u))), "nan");
    EXPECT_EQ(env.format(value(std::bit_cast<float>(0xFFC00000u))), "-nan");
    EXPECT_EQ(env.format(value(std::bit_cast<float>(0xFFFFFFFFu))), "nan(0xFFFFFFFF)");
    EXPECT_THROW((void)parse_property_value("nan(0x3F800000)", value(0.0f), env.parse_options()), Error);
}
