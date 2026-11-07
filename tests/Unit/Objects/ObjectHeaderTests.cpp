#include <gtest/gtest.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>
#include <UnrealLib/Objects/ObjectHeader.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
using UnrealLib::Testing::bytes_of;

namespace
{
    SerializationContext ctx(std::uint16_t version)
    {
        SerializationContext c;
        c.file_version = version;
        return c;
    }

    ObjectHeader round_trip(std::uint16_t version, const ObjectHeader& header, const ObjectHeaderInput& input,
                            std::size_t expected_size)
    {
        BinaryWriter writer(ctx(version));
        write_object_header(writer, header, input);
        writer.write_u32(0xDEADBEEF); // first bytes of the properties
        EXPECT_EQ(writer.size(), expected_size + 4) << "version " << version;
        BinaryReader reader(writer.data(), ctx(version));
        ObjectHeader parsed = read_object_header(reader, input);
        EXPECT_EQ(parsed.properties_offset, expected_size);
        EXPECT_EQ(reader.read_u32(), 0xDEADBEEFu);
        parsed.properties_offset = 0; // compared above; callers compare the remaining fields
        return parsed;
    }
}

TEST(ObjectHeaderTests, PlainObjectHasOnlyNetIndex)
{
    // NetIndex -1 followed by the first property tag.
    const auto data = bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0x2A, 0, 0, 0, 0, 0, 0, 0});
    BinaryReader reader(data, ctx(536));
    const ObjectHeader header = read_object_header(reader, ObjectHeaderInput{ObjectFlag::ClassDefaultObject});
    EXPECT_FALSE(header.state_frame.has_value());
    EXPECT_FALSE(header.component.has_value());
    EXPECT_EQ(header.net_index, std::optional<std::int32_t>(-1));
    EXPECT_EQ(header.properties_offset, 4u);
}

TEST(ObjectHeaderTests, NoNetIndexBeforeVersion322)
{
    ObjectHeader header;
    const ObjectHeader parsed = round_trip(300, header, ObjectHeaderInput{}, 0);
    EXPECT_FALSE(parsed.net_index.has_value());
}

TEST(ObjectHeaderTests, StateFrameLayoutsAcrossVersions)
{
    StateFrame frame;
    frame.node = ObjectIndex{5};
    frame.state_node = ObjectIndex{-2};
    frame.probe_mask = 0x1234;
    frame.latent_action = 7;
    frame.state_stack = {PushedState{ObjectIndex{3}, ObjectIndex{4}, 99}};
    frame.offset = 17;

    ObjectHeader header;
    header.state_frame = frame;
    header.net_index = 12;
    const ObjectHeaderInput input{ObjectFlag::HasStack};

    // 536 (< 566): node, state (4+4), probe uint64 (8), latent uint32 (4), stack (4 + 12), offset (4), net index (4)
    EXPECT_EQ(round_trip(536, header, input, 4 + 4 + 8 + 4 + 4 + 12 + 4 + 4), header);
    // 600 (>= 566, < 691): latent action becomes uint16
    EXPECT_EQ(round_trip(600, header, input, 4 + 4 + 8 + 2 + 4 + 12 + 4 + 4), header);
    // 700 (>= 691): probe mask becomes uint32 as well
    EXPECT_EQ(round_trip(700, header, input, 4 + 4 + 4 + 2 + 4 + 12 + 4 + 4), header);

    // Null node: no trailing code offset.
    header.state_frame->node = ObjectIndex{};
    header.state_frame->offset.reset();
    EXPECT_EQ(round_trip(536, header, input, 4 + 4 + 8 + 4 + 4 + 12 + 4), header);
}

TEST(ObjectHeaderTests, ComponentTemplateName)
{
    ObjectHeader header;
    header.component = ComponentTemplate{ObjectIndex{-3}, FName{9, 1}};
    header.net_index = -1;

    ObjectHeaderInput cdo_component;
    cdo_component.is_component = true;
    cdo_component.is_class_default_template = true;
    EXPECT_EQ(round_trip(536, header, cdo_component, 4 + 8 + 4), header);

    // Not inside a class default object (>= 267): no TemplateName.
    ObjectHeaderInput instance_component;
    instance_component.is_component = true;
    header.component->template_name.reset();
    EXPECT_EQ(round_trip(536, header, instance_component, 4 + 4), header);

    // Before 267 the name is always present; before 322 there is no NetIndex; before 343 names have no number.
    header.component->template_name = FName{9, 0};
    header.net_index.reset();
    EXPECT_EQ(round_trip(250, header, instance_component, 4 + 4), header);

    // Before 224 components carry no template data at all.
    ObjectHeader old;
    old.net_index.reset();
    EXPECT_EQ(round_trip(223, old, instance_component, 0), old);
}

TEST(ObjectHeaderTests, ClassDefaultObjectsOnlyHaveNetIndex)
{
    // Mirror's Edge Engine.u Default__ActorComponent: NetIndex 1013 then the TickGroup property tag, i.e. no
    // component template data although the class derives from Core.Component.
    ObjectHeaderInput cdo;
    cdo.object_flags = ObjectFlag::ClassDefaultObject | ObjectFlag::HasStack;
    cdo.is_component = true;
    cdo.is_class_default_template = true;
    ObjectHeader header;
    header.net_index = 1013;
    EXPECT_EQ(round_trip(536, header, cdo, 4), header);

    BinaryWriter writer(ctx(536));
    header.component = ComponentTemplate{};
    EXPECT_THROW(write_object_header(writer, header, cdo), SerializationError);
}

TEST(ObjectHeaderTests, WriterRejectsInconsistentInput)
{
    BinaryWriter writer(ctx(536));
    ObjectHeader header;
    EXPECT_THROW(write_object_header(writer, header, ObjectHeaderInput{ObjectFlag::HasStack}), SerializationError);
    ObjectHeaderInput component;
    component.is_component = true;
    EXPECT_THROW(write_object_header(writer, header, component), SerializationError);
}

TEST(ObjectHeaderTests, TruncatedHeaderThrows)
{
    const auto data = bytes_of({1, 0, 0, 0, 2, 0});
    BinaryReader reader(data, ctx(536));
    EXPECT_THROW((void)read_object_header(reader, ObjectHeaderInput{ObjectFlag::HasStack}), SerializationError);
}
