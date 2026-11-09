// T3D parser, serializer and validation.

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/T3D/T3D.hpp>

using namespace UnrealLib;
using namespace UnrealLib::T3D;

namespace
{
    constexpr std::string_view kMap = R"(Begin Map Name=Test
   Begin Level NAME=PersistentLevel
      Begin Actor Class=StaticMeshActor Name=StaticMeshActor_0 Archetype=StaticMeshActor'Engine.Default__StaticMeshActor'
         Begin Object Class=StaticMeshComponent Name=StaticMeshComponent0 Archetype=StaticMeshComponent'Engine.Default__StaticMeshActor:StaticMeshComponent0'
            StaticMesh=StaticMesh'Pkg.Mesh'
         End Object
         Components(0)=StaticMeshComponent0
         Location=(X=1.000000,Y=-2.500000,Z=3.000000)
         Tag="A = (quoted) string"
         Layer[2]=Foo
      End Actor
      Begin Actor Class=Brush Name=Brush_0
         Begin Brush Name=Model_0
            Begin PolyList
               Begin Polygon Flags=3584
                  Origin   -00128.000000,-00128.000000,+00128.000000
                  Vertex   -00128.000000,-00128.000000,+00128.000000
                  Vertex   +00128.000000,-00128.000000,+00128.000000
                  Vertex   +00128.000000,+00128.000000,+00128.000000
               End Polygon
            End PolyList
         End Brush
      End Actor
   End Level
End Map
)";
}

TEST(T3DText, ParsesNestedBlocksAttributesAndProperties)
{
    const T3DDocument doc = parse(kMap);
    ASSERT_EQ(doc.blocks.size(), 1u);
    const T3DBlock& map = doc.blocks[0];
    EXPECT_EQ(map.type, "Map");
    EXPECT_EQ(map.attribute("name"), "Test");
    ASSERT_EQ(map.children.size(), 1u);
    const T3DBlock& level = map.children[0];
    ASSERT_EQ(level.children.size(), 2u);

    const T3DBlock& actor = level.children[0];
    EXPECT_EQ(actor.line, 3u);
    EXPECT_EQ(actor.attribute("Class"), "StaticMeshActor");
    EXPECT_EQ(actor.attribute("Archetype"), "StaticMeshActor'Engine.Default__StaticMeshActor'");
    ASSERT_EQ(actor.children.size(), 1u);
    EXPECT_EQ(actor.children[0].property("StaticMesh")->value, "StaticMesh'Pkg.Mesh'");

    const T3DProperty* components = actor.property("Components", 0);
    ASSERT_NE(components, nullptr);
    EXPECT_EQ(components->value, "StaticMeshComponent0");
    EXPECT_FALSE(components->bracket_index);
    EXPECT_EQ(actor.property("location")->value, "(X=1.000000,Y=-2.500000,Z=3.000000)");
    EXPECT_EQ(actor.property("Tag")->value, "\"A = (quoted) string\"");
    const T3DProperty* layer = actor.property("Layer", 2);
    ASSERT_NE(layer, nullptr);
    EXPECT_TRUE(layer->bracket_index);
    EXPECT_EQ(actor.property("Layer"), nullptr); // unindexed lookup does not match Layer[2]

    const T3DBlock& polygon = level.children[1].children[0].children[0].children[0];
    EXPECT_EQ(polygon.type, "Polygon");
    EXPECT_EQ(polygon.attribute("Flags"), "3584");
    ASSERT_EQ(polygon.lines.size(), 4u);
    EXPECT_EQ(polygon.lines[1], "Vertex   -00128.000000,-00128.000000,+00128.000000");
}

TEST(T3DText, SerializeReproducesUnrealEdLayout)
{
    EXPECT_EQ(serialize(parse(kMap)), kMap);
}

TEST(T3DText, SerializeKeepsInterleavedOrder)
{
    const std::string text = "Begin Object Class=A Name=B\n"
                             "   X=1\n"
                             "   Begin Object Class=C Name=D\n"
                             "   End Object\n"
                             "   Y=2\n"
                             "   raw line\n"
                             "End Object\n";
    EXPECT_EQ(serialize(parse(text)), text);
}

TEST(T3DText, SerializeConstructedBlockWithoutLayout)
{
    T3DBlock block;
    block.type = "Object";
    block.attributes = {{"Class", "A"}, {"Name", "B"}};
    T3DBlock child;
    child.type = "Object";
    block.children.push_back(child);
    block.properties.push_back({"X", 1, true, "5"});
    block.lines.push_back("text");
    T3DDocument doc;
    doc.blocks.push_back(block);
    const std::string text = serialize(doc, "\t");
    EXPECT_EQ(text, "Begin Object Class=A Name=B\n\tX[1]=5\n\ttext\n\tBegin Object\n\tEnd Object\nEnd Object\n");
    EXPECT_EQ(parse(text), doc);
}

TEST(T3DText, AcceptsCrLfTabsAndCaseInsensitiveEnd)
{
    const T3DDocument doc = parse("begin Actor Class=A Name=B\r\n\tX=1\r\n\r\nEND actor\r\n");
    ASSERT_EQ(doc.blocks.size(), 1u);
    EXPECT_EQ(doc.blocks[0].property("X")->value, "1");
}

TEST(T3DText, ReportsErrorsWithLineNumbers)
{
    const auto message = [](std::string_view text) {
        try
        {
            (void)parse(text);
        }
        catch (const Error& e)
        {
            EXPECT_EQ(e.code(), ErrorCode::InvalidData);
            return std::string(e.what());
        }
        return std::string("no error");
    };
    EXPECT_NE(message("Begin Map\n").find("line 1"), std::string::npos);
    EXPECT_NE(message("Begin Map\nEnd Level\n").find("line 2"), std::string::npos);
    EXPECT_NE(message("End Map\n").find("line 1"), std::string::npos);
    EXPECT_NE(message("X=1\n").find("outside"), std::string::npos);
    EXPECT_NE(message("Begin Map\n X=\"open\nEnd Map\n").find("unterminated"), std::string::npos);
    EXPECT_NE(message("Begin Map Name\nEnd Map\n").find("Key=Value"), std::string::npos);
    EXPECT_NE(message("Begin Map\n X(a)=1\nEnd Map\n").find("index"), std::string::npos);
    EXPECT_NE(message("Begin Map\n X-Y=1\nEnd Map\n").find("property name"), std::string::npos);
    EXPECT_NE(message("Begin\nEnd\n").find("block type"), std::string::npos);
}

TEST(T3DText, ValidateFindsSemanticProblems)
{
    const T3DDocument doc = parse("Begin Map\n"
                                  "   Begin Actor Class=A\n"
                                  "      Begin Object Class=C Name=Sub\n"
                                  "      End Object\n"
                                  "      Begin Object Class=C Name=sub\n"
                                  "      End Object\n"
                                  "      Components(0)=Sub\n"
                                  "      Components(1)=Missing\n"
                                  "   End Actor\n"
                                  "End Map\n");
    const Diagnostics d = validate(doc);
    const auto has = [&](std::string_view code) {
        return std::any_of(d.entries().begin(), d.entries().end(),
                           [&](const Diagnostic& e) { return e.code == code; });
    };
    EXPECT_TRUE(has("t3d.map-without-level"));
    EXPECT_TRUE(has("t3d.actor-without-name"));
    EXPECT_FALSE(has("t3d.actor-without-class"));
    EXPECT_TRUE(has("t3d.duplicate-name"));
    EXPECT_TRUE(has("t3d.unresolved-subobject"));
    EXPECT_EQ(std::count_if(d.entries().begin(), d.entries().end(),
                            [](const Diagnostic& e) { return e.code == "t3d.unresolved-subobject"; }),
              1);
    EXPECT_FALSE(has("t3d.degenerate-polygon"));
    EXPECT_TRUE(validate(parse(kMap)).entries().empty());
}

TEST(T3DText, ValidateFindsDegeneratePolygons)
{
    const Diagnostics d = validate(parse("Begin PolyList\n"
                                         "   Begin Polygon\n"
                                         "      Origin   +00000.000000,+00000.000000,+00000.000000\n"
                                         "      Vertex   +00000.000000,+00000.000000,+00000.000000\n"
                                         "      Vertex   +00001.000000,+00000.000000,+00000.000000\n"
                                         "   End Polygon\n"
                                         "End PolyList\n"));
    ASSERT_EQ(d.entries().size(), 1u);
    EXPECT_EQ(d.entries()[0].code, "t3d.degenerate-polygon");
    EXPECT_NE(d.entries()[0].message.find("line 2"), std::string::npos);
}

namespace
{
    std::string identifier(const std::string& seed)
    {
        std::string out = "N";
        for (const char c : seed)
        {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') out += c;
        }
        return out;
    }

    T3DBlock random_block(int depth)
    {
        T3DBlock b;
        b.type = identifier(*rc::gen::arbitrary<std::string>());
        const int attrs = *rc::gen::inRange(0, 3);
        for (int i = 0; i < attrs; ++i)
        {
            b.attributes.emplace_back(identifier(*rc::gen::arbitrary<std::string>()),
                                      identifier(*rc::gen::arbitrary<std::string>()));
        }
        const int entries = *rc::gen::inRange(0, 6);
        for (int i = 0; i < entries; ++i)
        {
            const int kind = *rc::gen::inRange(0, depth < 3 ? 3 : 2);
            if (kind == 0)
            {
                T3DProperty p;
                p.name = identifier(*rc::gen::arbitrary<std::string>());
                if (*rc::gen::arbitrary<bool>())
                {
                    p.index = *rc::gen::inRange(0, 64);
                    p.bracket_index = *rc::gen::arbitrary<bool>();
                }
                p.value = *rc::gen::element<std::string>("1.5", "(X=1,Y=(A=2))", "\"a = b\"", "Class'P.O'", "None",
                                                         "");
                b.properties.push_back(p);
                b.layout += 'p';
            }
            else if (kind == 1)
            {
                b.lines.push_back("Vertex " + identifier(*rc::gen::arbitrary<std::string>()));
                b.layout += 'l';
            }
            else
            {
                b.children.push_back(random_block(depth + 1));
                b.layout += 'c';
            }
        }
        return b;
    }
}

RC_GTEST_PROP(T3DProperties, SerializeParseRoundTrip, ())
{
    T3DDocument doc;
    const int blocks = *rc::gen::inRange(0, 3);
    for (int i = 0; i < blocks; ++i) doc.blocks.push_back(random_block(0));
    const std::string text = serialize(doc);
    const T3DDocument back = parse(text);
    RC_ASSERT(back == doc);
    RC_ASSERT(serialize(back) == text);
}
