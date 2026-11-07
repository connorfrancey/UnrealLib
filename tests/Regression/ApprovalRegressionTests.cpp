// Approval (golden master) tests: deterministic text renderings of UnrealLib output, compared with the committed
// files in tests/Regression/approved/. They need no game files.
//
// What they guard:
//   * the UE3 bytecode token maps (generic and Mirror's Edge), opcode by opcode;
//   * the exact bytes the package writers produce for a fixed synthetic package, and how that package reads back;
//   * the defaultproperties-style formatting of decoded property values.
//
// A failure prints a text diff and leaves a .received.txt file next to the approved one. If the change is intended,
// rename the .received.txt file to .approved.txt and commit it (see tests/Support/TestMain.cpp).

#include <gtest/gtest.h>

#include <ApprovalTests.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Properties/PropertyTag.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;

namespace
{
    constexpr std::uint16_t mirrors_edge_version = 536;

    /// Classic 16-bytes-per-line hex dump with offsets and an ASCII column.
    std::string hex_dump(std::span<const std::byte> bytes)
    {
        std::string out;
        for (std::size_t line = 0; line < bytes.size(); line += 16)
        {
            out += fmt::format("{:08X} ", line);
            std::string ascii;
            for (std::size_t i = line; i < line + 16; ++i)
            {
                if (i < bytes.size())
                {
                    const auto b = std::to_integer<unsigned>(bytes[i]);
                    out += fmt::format(" {:02X}", b);
                    ascii += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
                }
                else
                {
                    out += "   ";
                }
            }
            out += "  |" + ascii + "|\n";
        }
        return out;
    }

    std::string describe(const TokenMap& map)
    {
        std::string out = fmt::format("extended_native = 0x{:02X}\nfirst_native    = 0x{:02X}\n\n",
                                      map.extended_native, map.first_native);
        for (std::size_t op = 0; op < map.kinds.size(); ++op)
        {
            out += fmt::format("0x{:02X} {}\n", op, to_string(map.kinds[op]));
        }
        return out;
    }

    std::string index_text(const Package& package, ObjectIndex index)
    {
        if (index.is_null()) return "null";
        return fmt::format("{} ({})", index.value, package.object_path(index));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Bytecode token maps
// ---------------------------------------------------------------------------------------------------------------------

TEST(ApprovalRegression, GenericUE3TokenMap)
{
    ApprovalTests::Approvals::verify(describe(TokenMap::ue3(mirrors_edge_version)));
}

TEST(ApprovalRegression, MirrorsEdgeTokenMap)
{
    // Differs from the generic map only by the profile's bytecode_overrides (0x4F).
    const GameProfile* profile = GameProfileRegistry::builtin().find("mirrorsedge-pc");
    ASSERT_NE(profile, nullptr);
    ApprovalTests::Approvals::verify(describe(TokenMap::ue3(mirrors_edge_version, profile)));
}

// ---------------------------------------------------------------------------------------------------------------------
// Synthetic package
// ---------------------------------------------------------------------------------------------------------------------

TEST(ApprovalRegression, SyntheticPackageBytes)
{
    const auto built = Testing::build_synthetic_package(Testing::make_basic_spec());
    ApprovalTests::Approvals::verify(hex_dump(built.file));
}

TEST(ApprovalRegression, SyntheticPackageListing)
{
    const auto built = Testing::build_synthetic_package(Testing::make_basic_spec());
    Package package;
    package.load_from_memory(built.file, "TestPkg");

    const PackageSummary& s = package.summary();
    std::string out;
    out += fmt::format("file_version     {}\nlicensee_version {}\nengine_version   {}\ncooker_version   {}\n",
                       s.file_version, s.licensee_version, s.engine_version, s.cooker_version);
    out += fmt::format("package_flags    0x{:08X}\nheader_size      {}\ncompressed       {}\nprofile          {}\n",
                       s.package_flags, s.header_size, s.is_compressed(), package.profile().id);

    out += fmt::format("\nnames ({})\n", package.names().size());
    for (std::size_t i = 0; i < package.names().size(); ++i)
    {
        out += fmt::format("  [{}] {}\n", i, package.names().to_string(FName{static_cast<std::int32_t>(i), 0}));
    }

    out += fmt::format("\nimports ({})\n", package.imports().size());
    for (std::size_t i = 0; i < package.imports().size(); ++i)
    {
        const auto index = ObjectIndex{-static_cast<std::int32_t>(i) - 1};
        out += fmt::format("  [{}] {} : {}.{}\n", index.value, package.object_path(index),
                           package.names().to_string(package.imports()[i].class_package),
                           package.names().to_string(package.imports()[i].class_name));
    }

    out += fmt::format("\nexports ({})\n", package.exports().size());
    for (std::size_t i = 0; i < package.exports().size(); ++i)
    {
        const auto index = ObjectIndex::from_export(i);
        const ExportEntry& e = package.exports()[i];
        out += fmt::format("  [{}] {} : {}\n", index.value, package.object_path(index), package.class_name(index));
        out += fmt::format("       super {}, outer {}, flags 0x{:016X}, serial {} bytes @ {}\n",
                           index_text(package, e.super_index), index_text(package, e.outer), e.object_flags,
                           e.serial_size, e.serial_offset);
    }
    ApprovalTests::Approvals::verify(out);
}

// ---------------------------------------------------------------------------------------------------------------------
// Property formatting
// ---------------------------------------------------------------------------------------------------------------------

TEST(ApprovalRegression, FormattedProperties)
{
    SerializationContext ctx;
    ctx.file_version = mirrors_edge_version;
    ctx.licensee_version = 43;

    NameTable names;
    const auto name = [&](std::string_view text) { return FName{names.add(text), 0}; };
    BinaryWriter w(ctx);
    const auto tag = [&](std::string_view prop, PropertyType type, std::int32_t size, std::int32_t index = 0) {
        write_fname(w, name(prop));
        write_fname(w, name(to_string(type)));
        w.write_i32(size);
        w.write_i32(index);
    };

    tag("Health", PropertyType::Int, 4);
    w.write_i32(100);
    tag("Speed", PropertyType::Float, 4);
    w.write_f32(487.5f);
    tag("bCanWallRun", PropertyType::Bool, 0);
    w.write_i32(1);   // Bool value lives in the tag (int32 before v673)
    tag("Tag", PropertyType::Name, 8);
    write_fname(w, FName{names.add("Faith"), 2});
    tag("Title", PropertyType::Str, 4 + 13 + 1);   // int32 length, 13 characters, NUL
    w.write_fstring(FString("Mirror's Edge"));
    tag("Kills", PropertyType::Int, 4, 1);   // Static-array element 1
    w.write_i32(-3);
    tag("Location", PropertyType::Struct, 12);
    write_fname(w, name("Vector"));
    w.write_f32(1.0f);
    w.write_f32(-2.5f);
    w.write_f32(1024.0f);
    tag("Rotation", PropertyType::Struct, 12);
    write_fname(w, name("Rotator"));
    w.write_i32(0);
    w.write_i32(16384);
    w.write_i32(-32768);
    write_fname(w, name("None"));

    const auto bytes = w.take();
    BinaryReader reader{std::span<const std::byte>(bytes), ctx};
    PropertyReadOptions options;
    options.names = &names;
    options.owner = "TdPawn";
    const auto properties = read_properties(reader, options);
    ASSERT_TRUE(reader.at_end());

    PropertyFormatOptions format;
    format.names = &names;
    std::string out;
    for (const Property& p : properties)
    {
        out += p.array_index == 0 ? p.name : fmt::format("{}[{}]", p.name, p.array_index);
        out += "=" + format_property_value(p.value, format) + "\n";
    }
    ApprovalTests::Approvals::verify(out);
}
