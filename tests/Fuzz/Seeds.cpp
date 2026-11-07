// Seed inputs for the fuzz targets: valid (or nearly valid) inputs that reach deep code quickly.

#include <cstring>
#include <string>

#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Serialization/Compression.hpp>

#include "FuzzTargets.hpp"
#include "Support/SyntheticPackage.hpp"

namespace UnrealLib::Fuzz
{
    const NameTable& property_names(); // PropertiesTarget.cpp

    namespace
    {
        using Seed = std::vector<std::byte>;

        Seed text_seed(char selector, std::string_view text)
        {
            Seed s(1 + text.size());
            s[0] = static_cast<std::byte>(selector);
            std::memcpy(s.data() + 1, text.data(), text.size());
            return s;
        }

        Seed prefixed(std::initializer_list<int> prefix, std::span<const std::byte> body)
        {
            Seed s;
            for (const int b : prefix) s.push_back(static_cast<std::byte>(b));
            s.insert(s.end(), body.begin(), body.end());
            return s;
        }

        /// Cooked-object payload for file version 536: NetIndex, "None" (no properties), then @p native bytes.
        std::vector<std::byte> object_payload(std::size_t native_zeros)
        {
            std::vector<std::byte> p(4 + 8 + native_zeros, std::byte{0});
            return p;
        }
    }

    std::vector<Seed> seeds_package()
    {
        std::vector<Seed> out;
        for (const CompressionMethod method : {CompressionMethod::None, CompressionMethod::Zlib, CompressionMethod::Lzo})
        {
            Testing::SyntheticPackageSpec spec = Testing::make_basic_spec();
            spec.compression = method;
            spec.block_size = 64;
            out.push_back(Testing::build_synthetic_package(spec).file);
        }
        return out;
    }

    std::vector<Seed> seeds_assets()
    {
        using namespace Testing;
        SyntheticPackageSpec spec;
        spec.names = {"None", "Core", "Engine", "Package", "Class", "Object"};
        const ObjectIndex engine = add_import(spec, "Core", "Package", ObjectIndex{}, "Engine");
        for (const char* cls : {"Texture2D", "StaticMesh", "SoundNodeWave", "SkeletalMesh", "AnimSequence", "Polys",
                                "Model", "Material"})
        {
            const ObjectIndex class_index = add_import(spec, "Core", "Class", engine, cls);
            add_export(spec, class_index, ObjectIndex{}, ObjectIndex{}, std::string("My") + cls, 0x0007000400000000ull,
                       object_payload(64));
        }
        // Polys with one triangle: Num, Max, ElementOwner, then one FPoly.
        {
            BinaryWriter w;
            w.write_i32(0); // NetIndex
            w.write_i32(0); // "None" (no properties)
            w.write_i32(0);
            w.write_i32(1); // Num
            w.write_i32(1); // Max
            w.write_i32(0); // ElementOwner
            for (int v = 0; v < 12; ++v) w.write_f32(v == 5 ? 1.0f : 0.0f); // base, normal (0,0,1), U, V
            w.write_i32(3);
            for (int v = 0; v < 9; ++v) w.write_f32(static_cast<float>(v));
            w.write_u32(0);  // flags
            w.write_i32(0);  // actor
            w.write_i32(0);  // item name
            w.write_i32(0);
            w.write_i32(0);  // material
            w.write_i32(-1); // link
            w.write_i32(-1); // brush poly
            w.write_f32(32.0f);
            w.write_u32(0);
            const ObjectIndex polys = add_import(spec, "Core", "Class", engine, "Polys");
            add_export(spec, polys, ObjectIndex{}, ObjectIndex{}, "Triangle", 0x0007000400000000ull, w.take());
        }
        return {build_synthetic_package(spec).file};
    }

    std::vector<Seed> seeds_serialization()
    {
        // Byte 0 selects the context exactly as SerializationTarget.cpp does; the body is written in that context.
        static constexpr std::uint16_t versions[] = {61, 68, 178, 249, 343, 536, 587, 868};
        std::vector<Seed> out;
        for (const int selector : {5, 0, 2, 4, 0x85})
        {
            SerializationContext context;
            context.file_version = versions[selector % std::size(versions)];
            context.endian = (selector & 0x80) != 0 ? Endian::Big : Endian::Little;
            BinaryWriter w(context);
            w.write_u8(0); // CompactIndex
            w.write_compact_index(-123456);
            w.write_u8(1); // FString
            w.write_fstring(FString("seed"));
            w.write_u8(1);
            w.write_fstring(FString("wide \xC3\xA9", StringEncoding::Utf16, true));
            w.write_u8(2); // FName
            write_fname(w, FName{3, 1});
            w.write_u8(5); // GUID
            for (int i = 0; i < 16; ++i) w.write_u8(static_cast<std::uint8_t>(i));
            out.push_back(prefixed({selector}, w.data()));
        }
        return out;
    }

    std::vector<Seed> seeds_properties()
    {
        const NameTable& names = property_names();
        const auto name = [&](std::string_view text) { return FName{*names.find(text), 0}; };
        const auto make_name = [&](std::string_view text) { return name(text); };
        const auto tagged = [&](std::string_view prop, PropertyType type, PropertyValue::Variant v,
                                std::string_view struct_name = {}) {
            TypeDesc t;
            t.type = type;
            t.struct_name = std::string(struct_name);
            Property p;
            p.name = std::string(prop);
            p.value.value = std::move(v);
            p.tag = make_property_tag(prop, 0, t, make_name);
            return p;
        };
        Property vector_field_x;
        vector_field_x.name = "X";
        vector_field_x.value.value = 1.0f;
        Property vector_field_y = vector_field_x;
        vector_field_y.name = "Y";
        Property vector_field_z = vector_field_x;
        vector_field_z.name = "Z";
        ArrayValue ints;
        ints.count = 2;
        ints.raw = std::vector<std::byte>(8, std::byte{7});
        const std::vector<Property> list = {
            tagged("A", PropertyType::Int, std::int32_t{42}),
            tagged("B", PropertyType::Float, 0.5f),
            tagged("C", PropertyType::Bool, true),
            tagged("Value", PropertyType::Name, NameValue{name("ROLE_Authority")}),
            tagged("X", PropertyType::Str, FString("text")),
            tagged("Y", PropertyType::Object, ObjectValue{ObjectIndex{-3}}),
            tagged("Z", PropertyType::Struct,
                   StructValue{"Vector", StructEncoding::Binary, {vector_field_x, vector_field_y, vector_field_z}},
                   "Vector"),
            tagged("A", PropertyType::Array, ints),
            tagged("B", PropertyType::Byte, std::uint8_t{7}),
        };
        SerializationContext context;
        context.file_version = 536;
        BinaryWriter w(context);
        write_properties(w, list, name("None"));
        return {w.take()};
    }

    std::vector<Seed> seeds_bytecode()
    {
        // Version index 3 (536). Return Nothing; IntConst 7 + Return; Jump + EndOfScript.
        return {prefixed({3, 2, 0}, Testing::bytes_of({0x04, 0x0B})),
                prefixed({3, 7, 0}, Testing::bytes_of({0x1D, 7, 0, 0, 0, 0x04, 0x0B})),
                prefixed({3, 4, 0}, Testing::bytes_of({0x06, 0x03, 0x00, 0x53}))};
    }

    std::vector<Seed> seeds_compression()
    {
        std::vector<std::byte> sample(300);
        for (std::size_t i = 0; i < sample.size(); ++i) sample[i] = static_cast<std::byte>((i * 7) % 13);
        std::vector<Seed> out;
        for (const auto& [selector, method] : {std::pair{0, CompressionMethod::Zlib}, {1, CompressionMethod::Lzo}})
        {
            out.push_back(prefixed({selector, 0x2C, 0x01}, compress(method, sample)));
        }
        return out;
    }

    std::vector<Seed> seeds_text()
    {
        return {text_seed(0, "Begin Map\n"
                             "   Begin Level NAME=PersistentLevel\n"
                             "      Begin Actor Class=Brush Name=Brush_0\n"
                             "         Begin Brush Name=Model_0\n"
                             "            Begin PolyList\n"
                             "               Begin Polygon Flags=3584 Link=0\n"
                             "                  Origin   -00128.000000,-00128.000000,+00128.000000\n"
                             "                  Vertex   -00128.000000,-00128.000000,+00128.000000\n"
                             "               End Polygon\n"
                             "            End PolyList\n"
                             "         End Brush\n"
                             "         Location=(X=1.0,Y=-2.5,Z=3.0)\n"
                             "         Tag=\"A = (quoted) string\"\n"
                             "         Layer[2]=Foo\n"
                             "      End Actor\n"
                             "   End Level\n"
                             "End Map\n"),
                text_seed(static_cast<char>(9 << 1 | 1), "(X=1.0,Y=nan(0xFFFFFFFF),Z=-inf)"),
                text_seed(static_cast<char>(12 << 1 | 1), "((X=1.0),(Y=2.0,Z=3.0))"),
                text_seed(static_cast<char>(6 << 1 | 1), "\"esc \\\" \\\\ (,)\""),
                text_seed(static_cast<char>(8 << 1 | 1), "Class'Pkg.Obj'.OnEvent")};
    }
}
