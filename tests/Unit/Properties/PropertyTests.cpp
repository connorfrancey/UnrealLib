#include <gtest/gtest.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Properties/PropertyTag.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

using namespace UnrealLib;

namespace
{
    TypeDesc type_of(PropertyType type)
    {
        TypeDesc desc;
        desc.type = type;
        return desc;
    }

    SerializationContext ctx(std::uint16_t version = 536)
    {
        SerializationContext c;
        c.file_version = version;
        return c;
    }

    // Builds tagged property data against a name table, adding names as needed.
    struct Builder
    {
        NameTable names;
        BinaryWriter writer{ctx()};

        FName name(std::string_view text) { return FName{names.add(text), 0}; }

        void tag(std::string_view prop, std::string_view type, std::int32_t size, std::int32_t index = 0)
        {
            write_fname(writer, name(prop));
            write_fname(writer, name(type));
            writer.write_i32(size);
            writer.write_i32(index);
        }
        void none() { write_fname(writer, name("None")); }
    };

    std::vector<Property> parse(Builder& b, const PropertySchema* schema = nullptr, Diagnostics* diags = nullptr,
                                std::string owner = "TestClass")
    {
        BinaryReader reader(b.writer.data(), ctx());
        PropertyReadOptions options;
        options.names = &b.names;
        options.schema = schema;
        options.owner = std::move(owner);
        options.diagnostics = diags;
        auto result = read_properties(reader, options);
        EXPECT_TRUE(reader.at_end());
        return result;
    }

    std::vector<std::byte> rewrite(Builder& b, const std::vector<Property>& props)
    {
        BinaryWriter writer(ctx());
        write_properties(writer, props, FName{*b.names.find("None"), 0});
        return writer.take();
    }

    std::vector<std::byte> original(const Builder& b)
    {
        return {b.writer.data().begin(), b.writer.data().end()};
    }

    class TestSchema final : public PropertySchema
    {
    public:
        const StructLayout* find_struct(std::string_view name) const override
        {
            return name == "Pair" ? &pair : nullptr;
        }
        std::optional<TypeDesc> find_property(std::string_view owner, std::string_view property) const override
        {
            if (owner == "TestClass" && property == "Numbers")
            {
                TypeDesc array;
                array.type = PropertyType::Array;
                array.inner = std::make_shared<TypeDesc>(type_of(PropertyType::Int));
                return array;
            }
            return std::nullopt;
        }
        StructLayout pair{"Pair", true, {{"A", type_of(PropertyType::Int), 1}, {"B", type_of(PropertyType::Byte), 2}}};
    };
}

TEST(PropertyTagTests, TypeNamesRoundTrip)
{
    for (auto type : {PropertyType::Byte, PropertyType::Int, PropertyType::Bool, PropertyType::Float,
                      PropertyType::Object, PropertyType::Name, PropertyType::Delegate, PropertyType::Class,
                      PropertyType::Array, PropertyType::Struct, PropertyType::Str, PropertyType::Map,
                      PropertyType::Interface, PropertyType::Component})
    {
        EXPECT_EQ(parse_property_type(to_string(type)), type);
    }
    EXPECT_EQ(parse_property_type("intproperty"), PropertyType::Int);
    EXPECT_EQ(parse_property_type("Nonsense"), PropertyType::Unknown);
}

TEST(PropertyTagTests, BoolValueWidthDependsOnVersion)
{
    for (const std::uint16_t version : {std::uint16_t{536}, std::uint16_t{673}})
    {
        NameTable names;
        PropertyTag tag;
        tag.name = FName{names.add("bFlag"), 0};
        tag.type_name = FName{names.add("BoolProperty"), 0};
        tag.type = PropertyType::Bool;
        tag.bool_value = true;
        BinaryWriter writer(ctx(version));
        write_property_tag(writer, tag);
        EXPECT_EQ(writer.size(), 8u + 8u + 8u + (version >= 673 ? 1u : 4u)) << version;
        BinaryReader reader(writer.data(), ctx(version));
        EXPECT_EQ(read_property_tag(reader, names), std::optional<PropertyTag>(tag));
    }
}

TEST(PropertyTagTests, EnumNameOnlyFromVersion633)
{
    NameTable names;
    PropertyTag tag;
    tag.name = FName{names.add("Mode"), 0};
    tag.type_name = FName{names.add("ByteProperty"), 0};
    tag.type = PropertyType::Byte;
    tag.size = 0;
    tag.enum_name = FName{names.add("EMode"), 0};
    BinaryWriter writer(ctx(633));
    write_property_tag(writer, tag);
    BinaryReader reader(writer.data(), ctx(633));
    EXPECT_EQ(read_property_tag(reader, names)->enum_name, tag.enum_name);

    BinaryWriter old_writer(ctx(536));
    write_property_tag(old_writer, tag);
    EXPECT_EQ(old_writer.size(), 24u);
}

TEST(PropertyTagTests, RejectsOldVersionsAndBadTags)
{
    Builder b;
    b.tag("X", "IntProperty", 4);
    {
        BinaryReader reader(b.writer.data(), ctx(200));
        EXPECT_THROW((void)read_property_tag(reader, b.names), UnsupportedError);
    }
    {
        // Size larger than the remaining data.
        BinaryReader reader(b.writer.data(), ctx());
        EXPECT_THROW((void)read_property_tag(reader, b.names), SerializationError);
    }
    Builder c;
    c.tag("X", "NotAType", 0);
    BinaryReader reader(c.writer.data(), ctx());
    EXPECT_THROW((void)read_property_tag(reader, c.names), SerializationError);
}

TEST(PropertyValueTests, ScalarsAndRoundTrip)
{
    Builder b;
    b.tag("Count", "IntProperty", 4);
    b.writer.write_i32(-7);
    b.tag("Scale", "FloatProperty", 4);
    b.writer.write_f32(1.5f);
    b.tag("bOn", "BoolProperty", 0);
    b.writer.write_i32(1);
    b.tag("Small", "ByteProperty", 1, 2);
    b.writer.write_u8(9);
    b.tag("Mode", "ByteProperty", 8);
    write_fname(b.writer, b.name("MODE_Fast"));
    b.tag("Tag", "NameProperty", 8);
    write_fname(b.writer, FName{b.names.add("Foo"), 3});
    b.tag("Text", "StrProperty", 4 + 3);
    b.writer.write_fstring(FString("hi"));
    b.tag("Target", "ObjectProperty", 4);
    b.writer.write_i32(-3);
    b.tag("OnHit", "DelegateProperty", 12);
    b.writer.write_i32(0);
    write_fname(b.writer, b.name("HitFn"));
    b.none();

    const auto props = parse(b);
    ASSERT_EQ(props.size(), 9u);
    EXPECT_EQ(std::get<std::int32_t>(props[0].value.value), -7);
    EXPECT_EQ(std::get<float>(props[1].value.value), 1.5f);
    EXPECT_TRUE(std::get<bool>(props[2].value.value));
    EXPECT_EQ(std::get<std::uint8_t>(props[3].value.value), 9);
    EXPECT_EQ(props[3].array_index, 2);
    EXPECT_EQ(b.names.to_string(std::get<EnumValue>(props[4].value.value).value), "MODE_Fast");
    EXPECT_EQ(b.names.to_string(std::get<NameValue>(props[5].value.value).value), "Foo_2");
    EXPECT_EQ(std::get<FString>(props[6].value.value).text, "hi");
    EXPECT_EQ(std::get<ObjectValue>(props[7].value.value).value.value, -3);
    EXPECT_EQ(b.names.to_string(std::get<DelegateValue>(props[8].value.value).function), "HitFn");

    EXPECT_EQ(rewrite(b, props), original(b));
    EXPECT_EQ(find_property(props, "small", 2), &props[3]);
    EXPECT_EQ(find_property(props, "small"), nullptr);

    PropertyFormatOptions format;
    format.names = &b.names;
    EXPECT_EQ(format_property_value(props[1].value, format), "1.5");
    EXPECT_EQ(format_property_value(props[6].value, format), "\"hi\"");
    EXPECT_EQ(format_property_value(props[7].value, format), "#-3");
}

TEST(PropertyValueTests, BuiltinBinaryAndTaggedStructs)
{
    Builder b;
    // Binary Vector.
    b.tag("Location", "StructProperty", 12);
    write_fname(b.writer, b.name("Vector"));
    b.writer.write_f32(1.0f);
    b.writer.write_f32(2.0f);
    b.writer.write_f32(3.0f);
    // Tagged struct with one int.
    BinaryWriter inner(ctx());
    write_fname(inner, b.name("Value"));
    write_fname(inner, b.name("IntProperty"));
    inner.write_i32(4);
    inner.write_i32(0);
    inner.write_i32(42);
    write_fname(inner, b.name("None"));
    b.tag("Settings", "StructProperty", static_cast<std::int32_t>(inner.size()));
    write_fname(b.writer, b.name("MySettings"));
    b.writer.write_bytes(inner.data());
    b.none();

    const auto props = parse(b);
    ASSERT_EQ(props.size(), 2u);
    const auto& vec = std::get<StructValue>(props[0].value.value);
    EXPECT_EQ(vec.encoding, StructEncoding::Binary);
    ASSERT_EQ(vec.fields.size(), 3u);
    EXPECT_EQ(vec.fields[2].name, "Z");
    EXPECT_EQ(std::get<float>(vec.fields[2].value.value), 3.0f);
    const auto& settings = std::get<StructValue>(props[1].value.value);
    EXPECT_EQ(settings.encoding, StructEncoding::Tagged);
    EXPECT_EQ(settings.struct_name, "MySettings");
    ASSERT_EQ(settings.fields.size(), 1u);
    EXPECT_EQ(std::get<std::int32_t>(settings.fields[0].value.value), 42);
    EXPECT_EQ(rewrite(b, props), original(b));

    PropertyFormatOptions format;
    format.names = &b.names;
    EXPECT_EQ(format_property_value(props[0].value, format), "(X=1.0,Y=2.0,Z=3.0)");
}

TEST(PropertyValueTests, ArraysUseSchemaOrStayRaw)
{
    Builder b;
    b.tag("Numbers", "ArrayProperty", 4 + 8);
    b.writer.write_i32(2);
    b.writer.write_i32(10);
    b.writer.write_i32(20);
    b.tag("Unknown", "ArrayProperty", 4 + 4);
    b.writer.write_i32(1);
    b.writer.write_i32(99);
    b.none();

    TestSchema schema;
    const auto typed = parse(b, &schema);
    const auto& numbers = std::get<ArrayValue>(typed[0].value.value);
    ASSERT_TRUE(numbers.decoded());
    ASSERT_EQ(numbers.elements.size(), 2u);
    EXPECT_EQ(std::get<std::int32_t>(numbers.elements[1].value), 20);
    const auto& unknown = std::get<ArrayValue>(typed[1].value.value);
    EXPECT_FALSE(unknown.decoded());
    EXPECT_EQ(unknown.raw->size(), 4u);
    EXPECT_EQ(rewrite(b, typed), original(b));

    const auto untyped = parse(b);
    EXPECT_FALSE(std::get<ArrayValue>(untyped[0].value.value).decoded());
    EXPECT_EQ(rewrite(b, untyped), original(b));
}

TEST(PropertyValueTests, ArrayOfTaggedStructsIsDetectedWithoutSchema)
{
    Builder b;
    BinaryWriter element(ctx());
    write_fname(element, b.name("Value"));
    write_fname(element, b.name("IntProperty"));
    element.write_i32(4);
    element.write_i32(0);
    element.write_i32(5);
    write_fname(element, b.name("None"));
    b.tag("Entries", "ArrayProperty", static_cast<std::int32_t>(4 + 2 * element.size()));
    b.writer.write_i32(2);
    b.writer.write_bytes(element.data());
    b.writer.write_bytes(element.data());
    b.none();

    const auto props = parse(b);
    const auto& entries = std::get<ArrayValue>(props[0].value.value);
    ASSERT_TRUE(entries.decoded());
    ASSERT_EQ(entries.elements.size(), 2u);
    EXPECT_EQ(std::get<StructValue>(entries.elements[0].value).fields.size(), 1u);
    EXPECT_EQ(rewrite(b, props), original(b));
}

TEST(PropertyValueTests, CustomBinaryStructWithStaticArray)
{
    Builder b;
    b.tag("P", "StructProperty", 6);
    write_fname(b.writer, b.name("Pair"));
    b.writer.write_i32(7);
    b.writer.write_u8(1);
    b.writer.write_u8(2);
    b.none();
    TestSchema schema;
    const auto props = parse(b, &schema);
    const auto& pair = std::get<StructValue>(props[0].value.value);
    ASSERT_EQ(pair.fields.size(), 3u);
    EXPECT_EQ(pair.fields[2].name, "B");
    EXPECT_EQ(pair.fields[2].array_index, 1);
    EXPECT_EQ(rewrite(b, props), original(b));
}

TEST(PropertyValueTests, UndecodableValueIsKeptRawWithDiagnostic)
{
    Builder b;
    // A "Vector" with the wrong size: the binary layout does not fit and it is not a tag list either.
    b.tag("Bad", "StructProperty", 5);
    write_fname(b.writer, b.name("Vector"));
    for (int i = 0; i < 5; ++i) b.writer.write_u8(0xEE);
    b.tag("After", "IntProperty", 4);
    b.writer.write_i32(1);
    b.none();
    Diagnostics diags;
    const auto props = parse(b, nullptr, &diags);
    ASSERT_EQ(props.size(), 2u);
    EXPECT_EQ(std::get<RawValue>(props[0].value.value).bytes.size(), 5u);
    EXPECT_EQ(std::get<std::int32_t>(props[1].value.value), 1);
    ASSERT_EQ(diags.entries().size(), 1u);
    EXPECT_EQ(diags.entries()[0].code, "property.raw-value");
    EXPECT_EQ(rewrite(b, props), original(b));
}

TEST(PropertyValueTests, DepthLimitAndMissingNames)
{
    Builder b;
    b.none();
    BinaryReader reader(b.writer.data(), ctx());
    EXPECT_THROW((void)read_properties(reader, PropertyReadOptions{}), Error);
}
