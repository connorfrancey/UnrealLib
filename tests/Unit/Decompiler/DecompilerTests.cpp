// Decompiler unit tests on hand-assembled bytecode (generic UE3 token map, file version 536 unless stated):
// one test per control-flow pattern of the structuring pass, the DynamicArrayAdd terminator regression, and the
// literal formatting helpers.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <UnrealLib/Bytecode/Bytecode.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Decompiler/Decompiler.hpp>
#include <UnrealLib/Decompiler/NativeTable.hpp>
#include <UnrealLib/Decompiler/Statements.hpp>
#include <UnrealLib/Reflection/Fields.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

using namespace UnrealLib;
using namespace UnrealLib::Decompiler;
using K = StatementKind;

namespace
{
    // Opcodes of the generic UE3 token map (tests/Regression/approved/...GenericUE3TokenMap.approved.txt).
    constexpr std::uint8_t opLocalVariable = 0x00, opReturn = 0x04, opSwitch = 0x05, opJump = 0x06,
                           opJumpIfNot = 0x07, opCase = 0x0A, opNothing = 0x0B, opLet = 0x0F,
                           opEndFunctionParms = 0x16, opIntZero = 0x25, opIntOne = 0x26, opTrue = 0x27,
                           opIterator = 0x2F, opIteratorPop = 0x30, opIteratorNext = 0x31, opDynamicArrayAdd = 0x54;

    /// Assembles bytecode. At version 536 object and name references have the same size in memory and on disk, so
    /// byte positions are memory offsets (jump targets).
    struct Assembler
    {
        std::vector<std::uint8_t> bytes;

        std::uint16_t here() const { return static_cast<std::uint16_t>(bytes.size()); }
        void op(std::uint8_t v) { bytes.push_back(v); }
        void u16(std::uint16_t v)
        {
            bytes.push_back(static_cast<std::uint8_t>(v & 0xFF));
            bytes.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        void i32(std::int32_t v)
        {
            for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        }
        void patch(std::size_t at, std::uint16_t target)
        {
            bytes[at] = static_cast<std::uint8_t>(target & 0xFF);
            bytes[at + 1] = static_cast<std::uint8_t>(target >> 8);
        }

        void statement() { op(opIntOne); }   // An expression statement (prints as `1;`)
        void statement0() { op(opIntZero); } // Another one, distinguishable by token kind
        /// `JumpIfNot(true) -> ?`; returns the position of the target to patch.
        std::size_t jump_if_not()
        {
            op(opJumpIfNot);
            const std::size_t at = bytes.size();
            u16(0);
            op(opTrue);
            return at;
        }
        std::size_t jump()
        {
            op(opJump);
            const std::size_t at = bytes.size();
            u16(0);
            return at;
        }
        void let_local(std::int32_t object, std::uint8_t value)
        {
            op(opLet);
            op(opLocalVariable);
            i32(object);
            op(value);
        }
    };

    Script decode(const std::vector<std::uint8_t>& bytes, std::uint16_t version = 536, std::size_t memory_size = 0)
    {
        std::vector<std::byte> data;
        for (const auto b : bytes) data.push_back(std::byte{b});
        SerializationContext ctx;
        ctx.file_version = version;
        BinaryReader reader{std::span<const std::byte>(data), ctx};
        const TokenMap map = TokenMap::ue3(version);
        BytecodeOptions options;
        options.token_map = &map;
        return decode_script(reader, memory_size != 0 ? memory_size : data.size(), options);
    }

    StructuredScript structured(const Assembler& a, const StructureOptions& options = {})
    {
        static Script script; // structure() keeps token indices; the Script must outlive the result's use
        script = decode(a.bytes);
        return structure(script, options);
    }

    std::vector<K> kinds(const std::vector<Statement>& statements)
    {
        std::vector<K> result;
        for (const Statement& s : statements) result.push_back(s.kind);
        return result;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Structuring patterns
// ---------------------------------------------------------------------------------------------------------------------

TEST(StructureTests, If)
{
    Assembler a;
    const auto to_end = a.jump_if_not();
    a.statement();
    a.patch(to_end, a.here());
    a.statement0();

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::If, K::Expression}));
    EXPECT_EQ(kinds(s.statements[0].body), (std::vector{K::Expression}));
    EXPECT_FALSE(s.statements[0].has_else);
    EXPECT_EQ(s.gotos, 0u);
}

TEST(StructureTests, IfElse)
{
    Assembler a;
    const auto to_else = a.jump_if_not();
    a.statement();
    const auto to_end = a.jump();
    a.patch(to_else, a.here());
    a.statement0();
    a.patch(to_end, a.here());
    a.statement();

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::If, K::Expression}));
    EXPECT_TRUE(s.statements[0].has_else);
    EXPECT_EQ(kinds(s.statements[0].body), (std::vector{K::Expression}));
    EXPECT_EQ(kinds(s.statements[0].else_body), (std::vector{K::Expression}));
    EXPECT_EQ(s.gotos, 0u);
}

TEST(StructureTests, While)
{
    Assembler a;
    const std::uint16_t top = a.here();
    const auto to_end = a.jump_if_not();
    a.statement();
    a.patch(a.jump(), top);
    a.patch(to_end, a.here());
    a.statement0();

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::While, K::Expression}));
    EXPECT_NE(s.statements[0].condition, no_token);
    EXPECT_EQ(kinds(s.statements[0].body), (std::vector{K::Expression}));
}

TEST(StructureTests, DoUntil)
{
    Assembler a;
    const std::uint16_t top = a.here();
    a.statement();
    a.statement0();
    a.patch(a.jump_if_not(), top); // backward conditional jump

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::DoUntil}));
    EXPECT_EQ(kinds(s.statements[0].body), (std::vector{K::Expression, K::Expression}));
}

TEST(StructureTests, WhileTrueWithBreak)
{
    // T: if (c) break; A; goto T; E:
    Assembler a;
    const std::uint16_t top = a.here();
    const auto skip_break = a.jump_if_not();
    const auto to_end = a.jump();
    a.patch(skip_break, a.here());
    a.statement();
    a.patch(a.jump(), top);
    a.patch(to_end, a.here());
    a.statement0();

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::While, K::Expression}));
    EXPECT_EQ(s.statements[0].condition, no_token); // while (true)
    ASSERT_EQ(kinds(s.statements[0].body), (std::vector{K::If, K::Expression}));
    EXPECT_EQ(kinds(s.statements[0].body[0].body), (std::vector{K::Break}));
    EXPECT_EQ(s.gotos, 0u);
}

TEST(StructureTests, ForLoopWithContinue)
{
    // L = 0; T: if (!c) goto E; if (c) continue; A; C: L = 1; goto T; E:
    Assembler a;
    a.let_local(1, opIntZero);
    const std::uint16_t top = a.here();
    const auto to_end = a.jump_if_not();
    const auto skip_continue = a.jump_if_not();
    const auto to_increment = a.jump();
    a.patch(skip_continue, a.here());
    a.statement();
    a.patch(to_increment, a.here());
    a.let_local(1, opIntOne);
    a.patch(a.jump(), top);
    a.patch(to_end, a.here());

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::For}));
    const Statement& loop = s.statements[0];
    EXPECT_NE(loop.init, no_token);      // L = 0 lifted (same variable as the increment)
    EXPECT_NE(loop.increment, no_token); // L = 1
    ASSERT_EQ(kinds(loop.body), (std::vector{K::If, K::Expression}));
    EXPECT_EQ(kinds(loop.body[0].body), (std::vector{K::Continue}));
    EXPECT_EQ(s.gotos, 0u);
}

TEST(StructureTests, LoopWithoutContinueStaysWhile)
{
    // The last statement is only an increment if a continue jumps to it.
    Assembler a;
    a.let_local(1, opIntZero);
    const std::uint16_t top = a.here();
    const auto to_end = a.jump_if_not();
    a.statement();
    a.let_local(1, opIntOne);
    a.patch(a.jump(), top);
    a.patch(to_end, a.here());

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::Expression, K::While}));
    EXPECT_EQ(kinds(s.statements[1].body), (std::vector{K::Expression, K::Expression}));
}

TEST(StructureTests, SwitchWithBreaksAndCompilerDefault)
{
    // switch (true) { case 1: A; break; case 1: B; break; } + the compiler's empty default case
    Assembler a;
    a.op(opSwitch);
    a.u16(0); // property size (versions 536-587: 16-bit)
    a.op(opTrue);
    a.op(opCase);
    const std::size_t to_case2 = a.bytes.size();
    a.u16(0);
    a.op(opIntOne);
    a.statement();
    const auto break1 = a.jump();
    a.patch(to_case2, a.here());
    a.op(opCase);
    const std::size_t to_default = a.bytes.size();
    a.u16(0);
    a.op(opIntOne);
    a.statement0();
    const auto break2 = a.jump();
    a.patch(to_default, a.here());
    a.op(opCase);
    a.u16(0xFFFF);
    a.patch(break1, a.here());
    a.patch(break2, a.here());
    a.statement();

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::Switch, K::Expression}));
    const Statement& sw = s.statements[0];
    ASSERT_EQ(kinds(sw.body), (std::vector{K::Case, K::Case})); // empty default dropped
    EXPECT_EQ(kinds(sw.body[0].body), (std::vector{K::Expression, K::Break}));
    EXPECT_EQ(kinds(sw.body[1].body), (std::vector{K::Expression, K::Break}));
    EXPECT_EQ(s.gotos, 0u);
}

TEST(StructureTests, ForEach)
{
    // foreach F() { A; } -> Iterator(F) -> E; A; IteratorNext; E: IteratorPop
    Assembler a;
    a.op(opIterator);
    a.op(opIntOne); // the iterator expression (normally an iterator function call)
    const std::size_t to_end = a.bytes.size();
    a.u16(0);
    a.statement();
    a.op(opIteratorNext);
    a.patch(to_end, a.here());
    a.op(opIteratorPop);
    a.statement0();

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::ForEach, K::Expression}));
    EXPECT_EQ(kinds(s.statements[0].body), (std::vector{K::Expression}));
    EXPECT_EQ(s.problems, 0u);
}

TEST(StructureTests, UnmatchedJumpBecomesLabelledGoto)
{
    Assembler a;
    const auto skip = a.jump();
    a.statement();
    a.patch(skip, a.here());
    a.statement0();

    const auto s = structured(a);
    ASSERT_EQ(kinds(s.statements), (std::vector{K::Goto, K::Expression, K::Label, K::Expression}));
    EXPECT_EQ(s.statements[2].text, jump_label(s.statements[0].target));
    EXPECT_EQ(s.gotos, 1u);
    EXPECT_EQ(s.problems, 0u);
}

TEST(StructureTests, TrailingEmptyReturnIsDropped)
{
    Assembler a;
    a.statement();
    a.op(opReturn);
    a.op(opNothing);

    EXPECT_EQ(kinds(structured(a).statements), (std::vector{K::Expression}));
    StructureOptions keep;
    keep.drop_trailing_return = false;
    EXPECT_EQ(kinds(structured(a, keep).statements), (std::vector{K::Expression, K::Return}));
}

// ---------------------------------------------------------------------------------------------------------------------
// Decoder regression: DynamicArrayAdd terminator
// ---------------------------------------------------------------------------------------------------------------------

TEST(BytecodeRegressionTests, DynamicArrayAddHasNoTerminatorBefore649)
{
    // Mirror's Edge (536): `Arr.Add(1); 0;` -- the next statement follows the count directly. Reading an
    // EndFunctionParms here used to swallow that statement.
    Assembler a;
    a.op(opDynamicArrayAdd);
    a.op(opLocalVariable);
    a.i32(1);
    a.op(opIntOne);
    a.statement0();

    const Script script = decode(a.bytes, 536);
    ASSERT_EQ(script.statements.size(), 2u);
    EXPECT_EQ(script.tokens[script.statements[0]].kind, TokenKind::DynamicArrayAdd);
    EXPECT_EQ(script.tokens[script.statements[1]].kind, TokenKind::IntZero);
}

TEST(BytecodeRegressionTests, DynamicArrayAddTerminatorFrom649IsChecked)
{
    // From 649 objects take 8 bytes in memory (4 on disk): memory size = storage + 4.
    Assembler with_end;
    with_end.op(opDynamicArrayAdd);
    with_end.op(opLocalVariable);
    with_end.i32(1);
    with_end.op(opIntOne);
    with_end.op(opEndFunctionParms);
    with_end.statement0();
    const Script script = decode(with_end.bytes, 649, with_end.bytes.size() + 4);
    ASSERT_EQ(script.statements.size(), 2u);

    Assembler missing_end; // A statement where the terminator belongs is an error, not a silently dropped statement
    missing_end.op(opDynamicArrayAdd);
    missing_end.op(opLocalVariable);
    missing_end.i32(1);
    missing_end.op(opIntOne);
    missing_end.statement0();
    missing_end.statement0();
    EXPECT_THROW((void)decode(missing_end.bytes, 649, missing_end.bytes.size() + 4), SerializationError);
}

// ---------------------------------------------------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------------------------------------------------

TEST(DecompilerFormatTests, Floats)
{
    EXPECT_EQ(format_float(1.0f), "1.0");
    EXPECT_EQ(format_float(0.25f), "0.25");
    EXPECT_EQ(format_float(-3.5f), "-3.5");
    EXPECT_EQ(format_float(0.1f), "0.1");          // shortest text that reads back to the same float
    EXPECT_EQ(format_float(100000.0f), "100000.0");
    EXPECT_EQ(format_float(0.0000001f), "0.0000001"); // fixed notation: no exponent in UnrealScript
}

TEST(DecompilerFormatTests, Strings)
{
    EXPECT_EQ(quote_string("plain"), "\"plain\"");
    EXPECT_EQ(quote_string("a \"b\" \\ c\n"), "\"a \\\"b\\\" \\\\ c\\n\"");
}

TEST(DecompilerFormatTests, CallSyntax)
{
    EXPECT_EQ(call_syntax(0, 0), CallSyntax::Function);
    EXPECT_EQ(call_syntax(FunctionFlag::Operator, 20), CallSyntax::Operator);
    EXPECT_EQ(call_syntax(FunctionFlag::Operator, 0), CallSyntax::PostOperator);
    // PreOperator wins even with precedence 0 (upstream checks the post-operator rule first).
    EXPECT_EQ(call_syntax(FunctionFlag::Operator | FunctionFlag::PreOperator, 0), CallSyntax::PreOperator);
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoding (encode_script, the inverse of decode_script). The corpus round trip is in MirrorsEdgeReflectionTests.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
    std::vector<std::uint8_t> encode(const Script& script, std::size_t* memory = nullptr, std::uint16_t version = 536)
    {
        SerializationContext ctx;
        ctx.file_version = version;
        BinaryWriter writer(ctx);
        const std::size_t m = encode_script(writer, script, TokenMap::ue3(version));
        if (memory) *memory = m;
        std::vector<std::uint8_t> out;
        for (const std::byte b : writer.data()) out.push_back(static_cast<std::uint8_t>(b));
        return out;
    }
}

TEST(EncodeTests, AssembledCodeRoundTrips)
{
    Assembler a;
    a.let_local(-3, opIntOne);          // local = 1
    const auto skip = a.jump_if_not();  // if (true)
    a.op(0x61);                         // extended native 0x161(...)
    a.op(0x23);
    a.op(opIntZero);
    a.op(opEndFunctionParms);
    a.patch(skip, a.here());
    a.op(0x1D);                         // return 7 (IntConst)
    a.i32(7);
    const Script script = decode(a.bytes);
    std::size_t memory = 0;
    EXPECT_EQ(encode(script, &memory), a.bytes);
    EXPECT_EQ(memory, a.bytes.size());
}

TEST(EncodeTests, NewTokensNeedOnlyTheirKind)
{
    // return 7; written from a hand-built tree: no opcodes set (0 is LocalVariable's), the map supplies them
    Script script;
    Token ret;
    ret.kind = TokenKind::Return;
    ret.children = {1};
    Token seven;
    seven.kind = TokenKind::IntConst;
    seven.int_value = 7;
    script.tokens = {ret, seven};
    script.statements = {0};
    std::size_t memory = 0;
    EXPECT_EQ(encode(script, &memory), (std::vector<std::uint8_t>{opReturn, 0x1D, 7, 0, 0, 0}));
    EXPECT_EQ(memory, 6u);

    script.tokens[0].children.clear();  // A Return without its expression
    EXPECT_THROW((void)encode(script), Error);
}

TEST(EncodeTests, AssembleFollowsJumpsWhenCodeMoves)
{
    // if (true) { 1; } 0;  ->  insert a statement before the `if`: the JumpIfNot target moves with its statement
    Assembler a;
    const auto skip = a.jump_if_not();
    a.statement();
    a.patch(skip, a.here());
    a.statement0();
    Script script = decode(a.bytes);
    Token nothing;
    nothing.kind = TokenKind::Nothing;
    script.tokens.push_back(nothing);
    script.statements.insert(script.statements.begin(), static_cast<std::uint32_t>(script.tokens.size() - 1));

    SerializationContext ctx;
    ctx.file_version = 536;
    BinaryWriter writer(ctx);
    const std::size_t memory = assemble_script(writer, script, TokenMap::ue3(536));
    EXPECT_EQ(memory, a.bytes.size() + 1);
    std::vector<std::uint8_t> out;
    for (const std::byte b : writer.data()) out.push_back(static_cast<std::uint8_t>(b));
    std::vector<std::uint8_t> want{opNothing};
    want.insert(want.end(), a.bytes.begin(), a.bytes.end());
    want[2] = static_cast<std::uint8_t>(want[2] + 1);   // The JumpIfNot target, one byte further
    EXPECT_EQ(out, want);

    // A new jump by token index: to the IntZero statement
    Token jump;
    jump.kind = TokenKind::Jump;
    jump.target = static_cast<std::int32_t>(script.statements.back());
    script.tokens.push_back(jump);
    script.statements.insert(script.statements.begin(), static_cast<std::uint32_t>(script.tokens.size() - 1));
    BinaryWriter w2(ctx);
    assemble_script(w2, script, TokenMap::ue3(536));
    const auto data = w2.data();
    EXPECT_EQ(static_cast<std::uint8_t>(data[0]), opJump);
    EXPECT_EQ(static_cast<std::uint8_t>(data[1]), static_cast<std::uint8_t>(3 + 1 + a.bytes.size() - 1));   // past jump, nothing, if
}
