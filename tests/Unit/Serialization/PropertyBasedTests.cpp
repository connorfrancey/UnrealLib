// Property-based tests (RapidCheck): invariants checked on generated inputs, with failing cases shrunk to a minimal
// counterexample. Each RC_GTEST_PROP is an ordinary GoogleTest test (100 cases by default; RC_PARAMS="max_success=N"
// raises it, RC_PARAMS="seed=N" reproduces a failure).
//
//   * round trips: whatever the writers produce, the readers read back unchanged, in both byte orders;
//   * totality: arbitrary bytes make the readers return or throw UnrealLib::Error, never anything else.

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Names/NameTable.hpp>
#include <UnrealLib/Properties/PropertyTag.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Serialization/BitStream.hpp>
#include <UnrealLib/Serialization/CompactIndex.hpp>
#include <UnrealLib/Serialization/FString.hpp>

using namespace UnrealLib;

namespace
{
    constexpr std::uint16_t mirrors_edge_version = 536;

    SerializationContext context(std::uint16_t version, Endian endian = Endian::Little)
    {
        SerializationContext ctx;
        ctx.file_version = version;
        ctx.endian = endian;
        return ctx;
    }

    rc::Gen<Endian> gen_endian() { return rc::gen::element(Endian::Little, Endian::Big); }

    std::vector<std::byte> to_bytes(const std::vector<std::uint8_t>& values)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(values.size());
        for (const auto v : values) bytes.push_back(std::byte{v});
        return bytes;
    }

    /// UTF-8 encoding of @p code_point (a Unicode scalar value).
    void append_utf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    /// Unicode scalar values except NUL (which ends a serialized FString) and the surrogate range.
    rc::Gen<std::uint32_t> gen_code_point()
    {
        return rc::gen::oneOf(rc::gen::inRange<std::uint32_t>(1, 0xD800),
                              rc::gen::inRange<std::uint32_t>(0xE000, 0x110000));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Primitive round trips
// ---------------------------------------------------------------------------------------------------------------------

RC_GTEST_PROP(PropertyBasedSerialization, IntegersRoundTrip,
              (std::int8_t i8, std::int16_t i16, std::int32_t i32, std::int64_t i64, std::uint16_t u16,
               std::uint32_t u32, std::uint64_t u64))
{
    const Endian endian = *gen_endian();
    BinaryWriter writer(context(mirrors_edge_version, endian));
    writer.write_i8(i8);
    writer.write_i16(i16);
    writer.write_i32(i32);
    writer.write_i64(i64);
    writer.write_u16(u16);
    writer.write_u32(u32);
    writer.write_u64(u64);
    const auto bytes = writer.take();
    RC_ASSERT(bytes.size() == 1u + 2 + 4 + 8 + 2 + 4 + 8);

    BinaryReader reader{std::span<const std::byte>(bytes), context(mirrors_edge_version, endian)};
    RC_ASSERT(reader.read_i8() == i8);
    RC_ASSERT(reader.read_i16() == i16);
    RC_ASSERT(reader.read_i32() == i32);
    RC_ASSERT(reader.read_i64() == i64);
    RC_ASSERT(reader.read_u16() == u16);
    RC_ASSERT(reader.read_u32() == u32);
    RC_ASSERT(reader.read_u64() == u64);
    RC_ASSERT(reader.at_end());
}

RC_GTEST_PROP(PropertyBasedSerialization, FloatBitPatternsRoundTrip, (std::uint32_t f32_bits, std::uint64_t f64_bits))
{
    // Bit patterns rather than values, so NaN payloads, signed zeros and denormals are covered too.
    const Endian endian = *gen_endian();
    BinaryWriter writer(context(mirrors_edge_version, endian));
    writer.write_f32(std::bit_cast<float>(f32_bits));
    writer.write_f64(std::bit_cast<double>(f64_bits));
    const auto bytes = writer.take();

    BinaryReader reader{std::span<const std::byte>(bytes), context(mirrors_edge_version, endian)};
    RC_ASSERT(std::bit_cast<std::uint32_t>(reader.read_f32()) == f32_bits);
    RC_ASSERT(std::bit_cast<std::uint64_t>(reader.read_f64()) == f64_bits);
}

RC_GTEST_PROP(PropertyBasedSerialization, EndiannessOnlyReversesBytes, (std::uint32_t value))
{
    BinaryWriter little(context(mirrors_edge_version, Endian::Little));
    BinaryWriter big(context(mirrors_edge_version, Endian::Big));
    little.write_u32(value);
    big.write_u32(value);
    const auto l = little.take();
    const auto b = big.take();
    RC_ASSERT(l.size() == 4u);
    RC_ASSERT(b.size() == 4u);
    for (std::size_t i = 0; i < 4; ++i) RC_ASSERT(l[i] == b[3 - i]);
    RC_ASSERT(std::to_integer<std::uint32_t>(l[0]) == (value & 0xFFu));   // Little-endian: low byte first
}

// ---------------------------------------------------------------------------------------------------------------------
// CompactIndex
// ---------------------------------------------------------------------------------------------------------------------

RC_GTEST_PROP(PropertyBasedSerialization, CompactIndexRoundTrips, (std::int32_t value))
{
    RC_PRE(value != std::numeric_limits<std::int32_t>::min());   // Not representable (documented)

    std::array<std::uint8_t, compact_index_max_size> encoded{};
    const std::size_t size = encode_compact_index(value, encoded);
    RC_ASSERT(size >= 1u);
    RC_ASSERT(size <= compact_index_max_size);
    RC_ASSERT(size == compact_index_size(value));

    BinaryWriter writer;
    writer.write_compact_index(value);
    const auto bytes = writer.take();
    RC_ASSERT(bytes.size() == size);
    for (std::size_t i = 0; i < size; ++i) RC_ASSERT(std::to_integer<std::uint8_t>(bytes[i]) == encoded[i]);

    BinaryReader reader{std::span<const std::byte>(bytes)};
    RC_ASSERT(reader.read_compact_index() == value);
    RC_ASSERT(reader.at_end());
}

RC_GTEST_PROP(PropertyBasedSerialization, CompactIndexSizeGrowsWithMagnitude, (std::int32_t a, std::int32_t b))
{
    RC_PRE(a != std::numeric_limits<std::int32_t>::min() && b != std::numeric_limits<std::int32_t>::min());
    const auto magnitude = [](std::int32_t v) { return v < 0 ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v); };
    if (magnitude(a) <= magnitude(b))
    {
        RC_ASSERT(compact_index_size(a) <= compact_index_size(b));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// FString
// ---------------------------------------------------------------------------------------------------------------------

RC_GTEST_PROP(PropertyBasedSerialization, AnsiFStringRoundTrips, ())
{
    // Printable ASCII is representable in Windows-1252, so the ANSI encoding is kept.
    const auto text = *rc::gen::container<std::string>(rc::gen::inRange<char>(0x20, 0x7F));
    const Endian endian = *gen_endian();

    BinaryWriter writer(context(mirrors_edge_version, endian));
    writer.write_fstring(FString(text, StringEncoding::Ansi, true));
    const auto bytes = writer.take();
    RC_ASSERT(bytes.size() == 4 + text.size() + 1);   // int32 length (v536 >= 178), bytes, NUL

    BinaryReader reader{std::span<const std::byte>(bytes), context(mirrors_edge_version, endian)};
    const FString read = reader.read_fstring();
    RC_ASSERT(read.text == text);
    RC_ASSERT(read.encoding == StringEncoding::Ansi);
    RC_ASSERT(reader.at_end());
}

RC_GTEST_PROP(PropertyBasedSerialization, Utf16FStringRoundTrips, ())
{
    // Any Unicode text (including characters outside the BMP, written as surrogate pairs) survives UTF-16.
    const auto code_points = *rc::gen::nonEmpty(rc::gen::container<std::vector<std::uint32_t>>(gen_code_point()));
    std::string text;
    for (const auto cp : code_points) append_utf8(text, cp);
    const Endian endian = *gen_endian();

    BinaryWriter writer(context(mirrors_edge_version, endian));
    writer.write_fstring(FString(text, StringEncoding::Utf16, true));
    const auto bytes = writer.take();

    BinaryReader reader{std::span<const std::byte>(bytes), context(mirrors_edge_version, endian)};
    const FString read = reader.read_fstring();
    RC_ASSERT(read.text == text);
    RC_ASSERT(read.encoding == StringEncoding::Utf16);
    RC_ASSERT(reader.at_end());
}

RC_GTEST_PROP(PropertyBasedSerialization, ArbitraryBytesNeverEscapeAsOtherThanError, (std::vector<std::uint8_t> raw))
{
    // A malformed or truncated string (huge or negative length, odd UTF-16 size, missing NUL...) must be reported as
    // an UnrealLib::Error. Anything else (std::bad_alloc, a crash) fails the property.
    const auto bytes = to_bytes(raw);
    BinaryReader reader{std::span<const std::byte>(bytes), context(mirrors_edge_version, *gen_endian())};
    try
    {
        while (!reader.at_end())
        {
            const std::size_t before = reader.position();
            (void)reader.read_fstring();
            RC_ASSERT(reader.position() > before);   // Progress: a successful read consumes bytes
        }
    }
    catch (const Error&)
    {
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Bit streams
// ---------------------------------------------------------------------------------------------------------------------

RC_GTEST_PROP(PropertyBasedSerialization, BitFieldsRoundTrip, ())
{
    const auto fields = *rc::gen::container<std::vector<std::pair<std::uint64_t, unsigned>>>(
        rc::gen::pair(rc::gen::arbitrary<std::uint64_t>(), rc::gen::inRange(0u, 65u)));

    BitWriter writer;
    std::size_t total = 0;
    for (const auto& [value, count] : fields)
    {
        writer.write_bits(value, count);
        total += count;
    }
    RC_ASSERT(writer.bit_count() == total);

    BitReader reader(writer.data(), writer.bit_count());
    for (const auto& [value, count] : fields)
    {
        const std::uint64_t mask = count == 64 ? ~0ull : ((1ull << count) - 1);
        RC_ASSERT(reader.read_bits(count) == (value & mask));
    }
    RC_ASSERT(reader.bits_remaining() == 0u);
    RC_ASSERT_THROWS_AS((void)reader.read_bit(), SerializationError);
}

RC_GTEST_PROP(PropertyBasedSerialization, SerializeIntRoundTrips, ())
{
    // FBitWriter::SerializeInt: a value in [0, max) round trips, using no more than ceil(log2(max)) bits.
    const auto max = *rc::gen::inRange<std::uint32_t>(1, std::numeric_limits<std::uint32_t>::max());
    const auto value = *rc::gen::inRange<std::uint32_t>(0, max);

    BitWriter writer;
    writer.write_int(value, max);
    RC_ASSERT(writer.bit_count() <= static_cast<std::size_t>(std::bit_width(max - 1)));

    BitReader reader(writer.data(), writer.bit_count());
    RC_ASSERT(reader.read_int(max) == value);
    RC_ASSERT(reader.bits_remaining() == 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// Property tags
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
    struct TagNames
    {
        NameTable names;
        FName none, name, struct_name, enum_name;

        TagNames()
        {
            none = FName{names.add("None"), 0};
            name = FName{names.add("Location"), 0};
            struct_name = FName{names.add("Vector"), 0};
            enum_name = FName{names.add("EPhysics"), 0};
        }
        FName type(PropertyType t) { return FName{names.add(to_string(t)), 0}; }
    };
}

RC_GTEST_PROP(PropertyBasedSerialization, PropertyTagsRoundTrip, ())
{
    // Every tag layout the supported UE3 versions use (FName numbers from 343; EnumName from 633; byte-sized bool
    // values from 673).
    const auto version = *rc::gen::inRange<std::uint16_t>(343, 869);
    const auto type = *rc::gen::element(PropertyType::Byte, PropertyType::Int, PropertyType::Bool,
                                        PropertyType::Float, PropertyType::Object, PropertyType::Name,
                                        PropertyType::Delegate, PropertyType::Class, PropertyType::Array,
                                        PropertyType::Struct, PropertyType::Str, PropertyType::Map,
                                        PropertyType::Interface, PropertyType::Component);

    TagNames n;
    PropertyTag tag;
    tag.name = FName{n.name.index, *rc::gen::inRange(0, 8)};
    tag.type_name = n.type(type);
    tag.type = type;
    tag.size = type == PropertyType::Bool ? 0 : *rc::gen::inRange(0, 64);
    tag.array_index = *rc::gen::inRange(0, 16);
    if (type == PropertyType::Struct) tag.struct_name = n.struct_name;
    if (type == PropertyType::Byte && version >= PropertyTagVersion::EnumNameAddedToBytePropertyTag)
        tag.enum_name = n.enum_name;
    if (type == PropertyType::Bool) tag.bool_value = *rc::gen::arbitrary<bool>();

    BinaryWriter writer(context(version));
    write_property_tag(writer, tag);
    for (std::int32_t i = 0; i < tag.size; ++i) writer.write_u8(0xAB);   // The value the tag describes
    write_property_terminator(writer, n.none);
    const auto bytes = writer.take();

    BinaryReader reader{std::span<const std::byte>(bytes), context(version)};
    const auto read = read_property_tag(reader, n.names);
    RC_ASSERT(read.has_value());
    RC_ASSERT(*read == tag);
    reader.skip(static_cast<std::size_t>(read->size));
    RC_ASSERT(!read_property_tag(reader, n.names).has_value());   // "None"
    RC_ASSERT(reader.at_end());
}

RC_GTEST_PROP(PropertyBasedSerialization, ArbitraryTagBytesThrowOnlyError, (std::vector<std::uint8_t> raw))
{
    TagNames n;
    for (const auto t : {PropertyType::Int, PropertyType::Bool, PropertyType::Struct, PropertyType::Byte})
        (void)n.type(t);
    const auto bytes = to_bytes(raw);
    BinaryReader reader{std::span<const std::byte>(bytes), context(mirrors_edge_version)};
    try
    {
        while (const auto tag = read_property_tag(reader, n.names))
        {
            RC_ASSERT(tag->size >= 0);
            RC_ASSERT(static_cast<std::size_t>(tag->size) <= reader.remaining());   // Checked before returning
            reader.skip(static_cast<std::size_t>(tag->size));
        }
    }
    catch (const Error&)
    {
    }
}
