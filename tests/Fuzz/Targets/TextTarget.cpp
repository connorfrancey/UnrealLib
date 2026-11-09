// Fuzz target "text": the T3D parser (with its serialize/parse round-trip invariant and validation) and
// parse_property_value() on arbitrary text against templates of every value kind.

#include <string>
#include <string_view>

#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/T3D/T3D.hpp>

#include "../FuzzTargets.hpp"

namespace
{
    using namespace UnrealLib;

    PropertyValue value(PropertyValue::Variant v) { return PropertyValue{std::move(v)}; }

    Property field(std::string name, PropertyValue::Variant v)
    {
        Property p;
        p.name = std::move(name);
        p.value.value = std::move(v);
        return p;
    }

    const std::vector<PropertyValue>& templates()
    {
        static const std::vector<PropertyValue> list = [] {
            ArrayValue ints;
            ints.elements = {value(std::int32_t{1})};
            ints.count = 1;
            StructValue vector{"Vector", StructEncoding::Binary, {field("X", 0.0f), field("Y", 0.0f), field("Z", 0.0f)}};
            ArrayValue vectors;
            vectors.elements = {value(vector)};
            vectors.count = 1;
            return std::vector<PropertyValue>{value(true),
                                              value(std::uint8_t{0}),
                                              value(EnumValue{FName{0, 0}}),
                                              value(std::int32_t{0}),
                                              value(0.0f),
                                              value(NameValue{FName{0, 0}}),
                                              value(FString()),
                                              value(ObjectValue{}),
                                              value(DelegateValue{}),
                                              value(vector),
                                              value(StructValue{"Tagged", StructEncoding::Tagged, {}}),
                                              value(ints),
                                              value(vectors),
                                              value(RawValue{{std::byte{1}}})};
        }();
        return list;
    }
}

UNREALLIB_FUZZ_TARGET(text)
{
    using namespace UnrealLib;
    if (size == 0) return;
    const std::string_view text(reinterpret_cast<const char*>(data) + 1, size - 1);

    if ((data[0] & 1) == 0)
    {
        const T3D::T3DDocument doc = T3D::parse(text);
        (void)T3D::validate(doc);
        const std::string once = T3D::serialize(doc);
        const T3D::T3DDocument back = T3D::parse(once);
        Fuzz::fuzz_check(back == doc, "parse(serialize(parse(x))) == parse(x)");
        Fuzz::fuzz_check(T3D::serialize(back) == once, "serialize is stable");
        return;
    }

    NameTable names;
    names.add("None");
    PropertyParseOptions options;
    options.make_name = [&](std::string_view t) {
        if (t.size() > 1024) throw Error(ErrorCode::InvalidData, "name too long");
        return FName{names.add(t), 0};
    };
    options.resolve_object = [](std::string_view t) -> std::optional<ObjectIndex> {
        if (t == "Obj" || t == "Class'Pkg.Obj'") return ObjectIndex{1};
        return std::nullopt;
    };
    const PropertyValue& like = templates()[(data[0] >> 1) % templates().size()];
    (void)parse_property_value(text, like, options);
}
