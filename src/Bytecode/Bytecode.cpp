#include <UnrealLib/Bytecode/Bytecode.hpp>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    namespace
    {
        constexpr std::pair<TokenKind, std::string_view> kind_names[] = {
            {TokenKind::Bad, "Bad"},
            {TokenKind::LocalVariable, "LocalVariable"},
            {TokenKind::InstanceVariable, "InstanceVariable"},
            {TokenKind::DefaultVariable, "DefaultVariable"},
            {TokenKind::StateVariable, "StateVariable"},
            {TokenKind::Return, "Return"},
            {TokenKind::Switch, "Switch"},
            {TokenKind::Jump, "Jump"},
            {TokenKind::JumpIfNot, "JumpIfNot"},
            {TokenKind::Stop, "Stop"},
            {TokenKind::Assert, "Assert"},
            {TokenKind::Case, "Case"},
            {TokenKind::Nothing, "Nothing"},
            {TokenKind::LabelTable, "LabelTable"},
            {TokenKind::GotoLabel, "GotoLabel"},
            {TokenKind::EatReturnValue, "EatReturnValue"},
            {TokenKind::Let, "Let"},
            {TokenKind::DynamicArrayElement, "DynamicArrayElement"},
            {TokenKind::New, "New"},
            {TokenKind::ClassContext, "ClassContext"},
            {TokenKind::MetaClassCast, "MetaClassCast"},
            {TokenKind::LetBool, "LetBool"},
            {TokenKind::EndParmValue, "EndParmValue"},
            {TokenKind::EndFunctionParms, "EndFunctionParms"},
            {TokenKind::Self, "Self"},
            {TokenKind::Skip, "Skip"},
            {TokenKind::Context, "Context"},
            {TokenKind::ArrayElement, "ArrayElement"},
            {TokenKind::VirtualFunction, "VirtualFunction"},
            {TokenKind::FinalFunction, "FinalFunction"},
            {TokenKind::IntConst, "IntConst"},
            {TokenKind::FloatConst, "FloatConst"},
            {TokenKind::StringConst, "StringConst"},
            {TokenKind::ObjectConst, "ObjectConst"},
            {TokenKind::NameConst, "NameConst"},
            {TokenKind::RotationConst, "RotationConst"},
            {TokenKind::VectorConst, "VectorConst"},
            {TokenKind::ByteConst, "ByteConst"},
            {TokenKind::IntZero, "IntZero"},
            {TokenKind::IntOne, "IntOne"},
            {TokenKind::True, "True"},
            {TokenKind::False, "False"},
            {TokenKind::NativeParameter, "NativeParameter"},
            {TokenKind::NoObject, "NoObject"},
            {TokenKind::IntConstByte, "IntConstByte"},
            {TokenKind::BoolVariable, "BoolVariable"},
            {TokenKind::DynamicCast, "DynamicCast"},
            {TokenKind::Iterator, "Iterator"},
            {TokenKind::IteratorPop, "IteratorPop"},
            {TokenKind::IteratorNext, "IteratorNext"},
            {TokenKind::StructCmpEq, "StructCmpEq"},
            {TokenKind::StructCmpNe, "StructCmpNe"},
            {TokenKind::UnicodeStringConst, "UnicodeStringConst"},
            {TokenKind::StructMember, "StructMember"},
            {TokenKind::DynamicArrayLength, "DynamicArrayLength"},
            {TokenKind::GlobalFunction, "GlobalFunction"},
            {TokenKind::PrimitiveCast, "PrimitiveCast"},
            {TokenKind::DynamicArrayInsert, "DynamicArrayInsert"},
            {TokenKind::ReturnNothing, "ReturnNothing"},
            {TokenKind::DelegateCmpEq, "DelegateCmpEq"},
            {TokenKind::DelegateCmpNe, "DelegateCmpNe"},
            {TokenKind::DelegateFunctionCmpEq, "DelegateFunctionCmpEq"},
            {TokenKind::DelegateFunctionCmpNe, "DelegateFunctionCmpNe"},
            {TokenKind::EmptyDelegate, "EmptyDelegate"},
            {TokenKind::DynamicArrayRemove, "DynamicArrayRemove"},
            {TokenKind::DebugInfo, "DebugInfo"},
            {TokenKind::DelegateFunction, "DelegateFunction"},
            {TokenKind::DelegateProperty, "DelegateProperty"},
            {TokenKind::LetDelegate, "LetDelegate"},
            {TokenKind::Conditional, "Conditional"},
            {TokenKind::DynamicArrayFind, "DynamicArrayFind"},
            {TokenKind::DynamicArrayFindStruct, "DynamicArrayFindStruct"},
            {TokenKind::OutVariable, "OutVariable"},
            {TokenKind::DefaultParameter, "DefaultParameter"},
            {TokenKind::EmptyParm, "EmptyParm"},
            {TokenKind::InstanceDelegate, "InstanceDelegate"},
            {TokenKind::Unresolved, "Unresolved"},
            {TokenKind::UndefinedVariable, "UndefinedVariable"},
            {TokenKind::InterfaceContext, "InterfaceContext"},
            {TokenKind::InterfaceCast, "InterfaceCast"},
            {TokenKind::EndOfScript, "EndOfScript"},
            {TokenKind::DynamicArrayAdd, "DynamicArrayAdd"},
            {TokenKind::DynamicArrayAddItem, "DynamicArrayAddItem"},
            {TokenKind::DynamicArrayRemoveItem, "DynamicArrayRemoveItem"},
            {TokenKind::DynamicArrayInsertItem, "DynamicArrayInsertItem"},
            {TokenKind::DynamicArrayIterator, "DynamicArrayIterator"},
            {TokenKind::DynamicArraySort, "DynamicArraySort"},
            {TokenKind::FilterEditorOnly, "FilterEditorOnly"},
            {TokenKind::NativeFunction, "NativeFunction"},
        };

        // UE3 version gates used by the token formats (upstream token classes / PackageObjectLegacyVersion).
        constexpr std::uint16_t vNameSizeTo8 = 343;
        constexpr std::uint16_t vObjectSizeTo8 = 587;
        constexpr std::uint16_t vContextPropertyAdded = 588;
        constexpr std::uint16_t vSwitchExpressionField = 611;
        constexpr std::uint16_t vIsLocalAddedToDelegateFunction = 181;
        constexpr std::uint16_t vAssertDebugFlag = 200;
        constexpr std::uint16_t vEatReturnValueProperty = 201;
        constexpr std::uint16_t vNewTemplate = 300;
        constexpr std::uint16_t vStructReferenceAddedToStructMember = 375;
        constexpr std::uint16_t vIsCopyAddedToStructMember = 381;
        constexpr std::uint16_t vIsModificationAddedToStructMember = 433;
        constexpr std::uint16_t vSkipSizeAddedToArrayFind = 375;
        constexpr std::uint16_t vSkipSizeAddedToArrayIntrinsics = 497;
        constexpr std::uint16_t vEndTokenAppendedToArrayIntrinsics = 649;
        constexpr std::uint16_t vDelegatePropertyObject = 611; // "> 610"

        class Decoder
        {
        public:
            Decoder(BinaryReader& reader, std::size_t memory_size, const BytecodeOptions& options)
                : m_reader(reader), m_memory_size(memory_size), m_options(options), m_map(*options.token_map),
                  m_version(reader.context().file_version), m_start(reader.position())
            {
                m_name_size = m_version >= vNameSizeTo8 ? 8 : 4;
                m_object_size = m_version >= vObjectSizeTo8 ? 8 : 4;
            }

            Script run()
            {
                while (m_memory < m_memory_size)
                {
                    m_script.statements.push_back(next(0));
                }
                if (m_memory != m_memory_size)
                {
                    fail(fmt::format("token stream ends at memory offset {}, past the script size {}", m_memory,
                                     m_memory_size));
                }
                m_script.storage_size = m_reader.position() - m_start;
                m_script.memory_size = m_memory;
                return std::move(m_script);
            }

        private:
            [[noreturn]] void fail(const std::string& message) const
            {
                throw SerializationError("bytecode: " + message, m_reader.absolute_position());
            }

            std::uint8_t u8()
            {
                m_memory += 1;
                return m_reader.read_u8();
            }
            std::uint16_t u16()
            {
                m_memory += 2;
                return m_reader.read_u16();
            }
            std::int32_t i32()
            {
                m_memory += 4;
                return m_reader.read_i32();
            }
            float f32()
            {
                m_memory += 4;
                return m_reader.read_f32();
            }
            ObjectIndex object()
            {
                m_memory += m_object_size;
                return ObjectIndex{m_reader.read_i32()};
            }
            FName name()
            {
                m_memory += m_name_size;
                const FName n = read_fname(m_reader);
                if (n.index < 0 || n.index >= m_options.name_count || n.number < 0)
                {
                    fail(fmt::format("invalid name reference {}", n.index));
                }
                return n;
            }

            std::uint32_t add_child(std::uint32_t parent, std::size_t depth)
            {
                const std::uint32_t child = next(depth + 1);
                m_script.tokens[parent].children.push_back(child);
                return child;
            }

            // Reads operands until (and including) EndFunctionParms; the terminator is not kept as a child.
            void params(std::uint32_t parent, std::size_t depth, TokenKind terminator = TokenKind::EndFunctionParms)
            {
                while (true)
                {
                    const std::uint32_t child = next(depth + 1);
                    if (m_script.tokens[child].kind == terminator)
                    {
                        return;
                    }
                    m_script.tokens[parent].children.push_back(child);
                }
            }

            // Upstream UByteCodeDecompiler.DeserializeDebugToken: a DebugInfo token (version 100) may follow calls.
            void maybe_debug(std::uint32_t parent, std::size_t depth)
            {
                if (m_memory + 1 + 4 >= m_memory_size || m_reader.remaining() < 5)
                {
                    return;
                }
                const auto peek = m_reader.peek_view(5);
                const auto op = static_cast<std::uint8_t>(peek[0]);
                if (op >= m_map.extended_native || m_map.kinds[op] != TokenKind::DebugInfo)
                {
                    return;
                }
                BinaryReader probe = m_reader.sub_reader(m_reader.position() + 1, 4);
                if (probe.read_i32() == 100)
                {
                    add_child(parent, depth);
                }
            }

            // Reads the EndFunctionParms terminating an intrinsic's operands; anything else is an error (reading a
            // statement as the terminator would silently drop it).
            void expect_end_parms(std::size_t depth)
            {
                const std::uint32_t end = next(depth + 1);
                if (m_script.tokens[end].kind != TokenKind::EndFunctionParms)
                {
                    fail(fmt::format("expected EndFunctionParms, found {} at memory offset 0x{:X}",
                                     to_string(m_script.tokens[end].kind), m_script.tokens[end].memory_offset));
                }
            }

            void array_method(std::uint32_t index, std::size_t depth, int params_after_skip, std::uint16_t skip_version)
            {
                add_child(index, depth);
                if (m_version >= skip_version)
                {
                    m_script.tokens[index].word = u16();
                }
                for (int i = 0; i < params_after_skip; ++i)
                {
                    add_child(index, depth);
                }
                if (m_version >= vEndTokenAppendedToArrayIntrinsics)
                {
                    expect_end_parms(depth);
                }
                maybe_debug(index, depth);
            }

            std::string ansi_string()
            {
                std::vector<std::uint8_t> bytes;
                while (true)
                {
                    const std::uint8_t c = u8();
                    if (c == 0)
                    {
                        break;
                    }
                    bytes.push_back(c);
                    if (bytes.size() > m_memory_size)
                    {
                        fail("unterminated string constant");
                    }
                }
                return Text::cp1252_to_utf8(bytes);
            }

            std::string unicode_string()
            {
                std::u16string units;
                while (true)
                {
                    const auto c = static_cast<char16_t>(u16());
                    if (c == 0)
                    {
                        break;
                    }
                    units.push_back(c);
                    if (units.size() > m_memory_size)
                    {
                        fail("unterminated unicode string constant");
                    }
                }
                return Text::utf16_to_utf8(units);
            }

            std::uint32_t next(std::size_t depth)
            {
                if (depth > m_options.max_depth)
                {
                    fail("expression nesting too deep");
                }
                if (m_script.tokens.size() >= m_options.max_tokens)
                {
                    fail("too many tokens");
                }
                if (m_memory >= m_memory_size)
                {
                    fail("token extends past the end of the script");
                }

                const auto index = static_cast<std::uint32_t>(m_script.tokens.size());
                m_script.tokens.emplace_back();
                const std::size_t start = m_reader.position();
                const std::size_t start_memory = m_memory;
                {
                    Token& t = m_script.tokens.back();
                    t.offset = static_cast<std::uint32_t>(start - m_start);
                    t.memory_offset = static_cast<std::uint32_t>(start_memory);
                    t.opcode = u8();
                    if (t.opcode >= m_map.extended_native)
                    {
                        t.kind = TokenKind::NativeFunction;
                        t.native_index = t.opcode >= m_map.first_native
                                             ? t.opcode
                                             : static_cast<std::uint16_t>(((t.opcode - m_map.extended_native) << 8) | u8());
                    }
                    else
                    {
                        t.kind = m_map.kinds[t.opcode];
                    }
                }
                // Note: `tok()` re-fetches the token because adding children may reallocate the vector.
                auto tok = [&]() -> Token& { return m_script.tokens[index]; };

                switch (tok().kind)
                {
                case TokenKind::Bad:
                    fail(fmt::format("bad opcode 0x{:02X}", tok().opcode));

                case TokenKind::LocalVariable:
                case TokenKind::InstanceVariable:
                case TokenKind::DefaultVariable:
                case TokenKind::StateVariable:
                case TokenKind::NativeParameter:
                case TokenKind::OutVariable:
                case TokenKind::ObjectConst:
                    tok().object = object();
                    break;

                case TokenKind::Return:
                case TokenKind::GotoLabel:
                case TokenKind::BoolVariable:
                case TokenKind::DynamicArrayLength:
                case TokenKind::InterfaceContext:
                    add_child(index, depth);
                    break;

                case TokenKind::Switch:
                    if (m_version >= vSwitchExpressionField)
                    {
                        tok().object = object();
                    }
                    tok().word2 = (m_version >= 536 && m_version <= 587) ? u16() : u8();
                    add_child(index, depth);
                    break;

                case TokenKind::Jump:
                case TokenKind::FilterEditorOnly:
                    tok().word = u16();
                    break;

                case TokenKind::JumpIfNot:
                    tok().word = u16();
                    add_child(index, depth);
                    break;

                case TokenKind::Assert:
                    tok().word = u16();
                    if (m_version >= vAssertDebugFlag)
                    {
                        tok().byte_value = u8();
                    }
                    add_child(index, depth);
                    break;

                case TokenKind::Case:
                    tok().word = u16();
                    if (tok().word != 0xFFFF)
                    {
                        add_child(index, depth);
                    }
                    break;

                case TokenKind::LabelTable:
                    while (true)
                    {
                        LabelEntry entry;
                        entry.name = name();
                        entry.offset = i32();
                        if (entry.name.index >= 0 && m_is_none(entry.name))
                        {
                            // The terminator's own fields, kept so that encoding writes it back as it was
                            tok().name = entry.name;
                            tok().int_value = entry.offset;
                            break;
                        }
                        tok().labels.push_back(entry);
                    }
                    break;

                case TokenKind::EatReturnValue:
                case TokenKind::ReturnNothing:
                    if (m_version >= vEatReturnValueProperty)
                    {
                        tok().object = object();
                    }
                    break;

                case TokenKind::Let:
                case TokenKind::LetBool:
                case TokenKind::LetDelegate:
                case TokenKind::DynamicArrayElement:
                case TokenKind::ArrayElement:
                    add_child(index, depth);
                    add_child(index, depth);
                    break;

                case TokenKind::New:
                    for (int i = 0; i < (m_version > vNewTemplate ? 5 : 4); ++i)
                    {
                        add_child(index, depth);
                    }
                    break;

                case TokenKind::Context:
                case TokenKind::ClassContext:
                    add_child(index, depth);
                    tok().word = u16();
                    if (m_version >= vContextPropertyAdded)
                    {
                        tok().object2 = object();
                        tok().word2 = u8();
                    }
                    else
                    {
                        tok().word2 = m_version > 512 ? u16() : u8();
                    }
                    add_child(index, depth);
                    break;

                case TokenKind::MetaClassCast:
                case TokenKind::DynamicCast:
                case TokenKind::InterfaceCast:
                    tok().object = object();
                    add_child(index, depth);
                    break;

                case TokenKind::Skip:
                    tok().word = u16();
                    add_child(index, depth);
                    break;

                case TokenKind::VirtualFunction:
                    if (m_version >= 178 && m_version <= 241)
                    {
                        tok().byte_value = u8();
                    }
                    tok().name = name();
                    params(index, depth);
                    maybe_debug(index, depth);
                    break;

                case TokenKind::FinalFunction:
                    tok().object = object();
                    params(index, depth);
                    maybe_debug(index, depth);
                    break;

                case TokenKind::GlobalFunction:
                    tok().name = name();
                    params(index, depth);
                    maybe_debug(index, depth);
                    break;

                case TokenKind::DelegateFunction:
                    if (m_version >= vIsLocalAddedToDelegateFunction)
                    {
                        tok().byte_value = u8();
                    }
                    tok().object = object();
                    tok().name = name();
                    params(index, depth);
                    maybe_debug(index, depth);
                    break;

                case TokenKind::NativeFunction:
                    params(index, depth);
                    maybe_debug(index, depth);
                    break;

                case TokenKind::DelegateCmpEq:
                case TokenKind::DelegateCmpNe:
                case TokenKind::DelegateFunctionCmpEq:
                case TokenKind::DelegateFunctionCmpNe:
                    params(index, depth);
                    break;

                case TokenKind::IntConst: tok().int_value = i32(); break;
                case TokenKind::FloatConst: tok().floats[0] = f32(); break;
                case TokenKind::StringConst: tok().string_value = ansi_string(); break;
                case TokenKind::UnicodeStringConst: tok().string_value = unicode_string(); break;
                case TokenKind::NameConst: tok().name = name(); break;
                case TokenKind::RotationConst:
                    for (auto& v : tok().ints) v = i32();
                    break;
                case TokenKind::VectorConst:
                    for (auto& v : tok().floats) v = f32();
                    break;
                case TokenKind::ByteConst:
                case TokenKind::IntConstByte: tok().byte_value = u8(); break;

                case TokenKind::Iterator:
                    add_child(index, depth);
                    tok().word = u16();
                    break;

                case TokenKind::StructCmpEq:
                case TokenKind::StructCmpNe:
                    tok().object = object();
                    add_child(index, depth);
                    add_child(index, depth);
                    break;

                case TokenKind::StructMember:
                    tok().object = object();
                    if (m_version >= vStructReferenceAddedToStructMember)
                    {
                        tok().object2 = object();
                    }
                    if (m_version >= vIsCopyAddedToStructMember)
                    {
                        tok().byte_value = u8();
                    }
                    if (m_version >= vIsModificationAddedToStructMember)
                    {
                        tok().byte_value2 = u8();
                    }
                    add_child(index, depth);
                    break;

                case TokenKind::PrimitiveCast:
                    tok().byte_value = u8();
                    add_child(index, depth);
                    break;

                case TokenKind::DynamicArrayInsert:
                case TokenKind::DynamicArrayRemove:
                    array_method(index, depth, 2, 0xFFFF);
                    break;
                case TokenKind::DynamicArrayFind:
                    array_method(index, depth, 1, vSkipSizeAddedToArrayFind);
                    break;
                case TokenKind::DynamicArrayFindStruct:
                    array_method(index, depth, 2, vSkipSizeAddedToArrayFind);
                    break;
                case TokenKind::DynamicArrayAddItem:
                case TokenKind::DynamicArrayRemoveItem:
                case TokenKind::DynamicArraySort:
                    array_method(index, depth, 1, vSkipSizeAddedToArrayIntrinsics);
                    break;
                case TokenKind::DynamicArrayInsertItem:
                    array_method(index, depth, 2, vSkipSizeAddedToArrayIntrinsics);
                    break;
                case TokenKind::DynamicArrayAdd:
                    add_child(index, depth);
                    add_child(index, depth);
                    // Upstream UELib reads EndFunctionParms here for every version, but Mirror's Edge (536) has none:
                    // the next statement follows the count directly. Gated like the other array intrinsics.
                    if (m_version >= vEndTokenAppendedToArrayIntrinsics)
                    {
                        expect_end_parms(depth);
                    }
                    maybe_debug(index, depth);
                    break;

                case TokenKind::DebugInfo:
                    for (auto& v : tok().ints) v = i32();
                    tok().byte_value = u8();
                    break;

                case TokenKind::DelegateProperty:
                    tok().name = name();
                    if (m_version >= vDelegatePropertyObject)
                    {
                        tok().object = object();
                    }
                    break;

                case TokenKind::Conditional:
                    add_child(index, depth);
                    tok().word = u16();
                    add_child(index, depth);
                    tok().word2 = u16();
                    add_child(index, depth);
                    break;

                case TokenKind::DefaultParameter:
                    tok().word = u16();
                    params(index, depth, TokenKind::EndParmValue);
                    break;

                case TokenKind::InstanceDelegate:
                    tok().name = name();
                    break;

                case TokenKind::DynamicArrayIterator:
                    add_child(index, depth);
                    add_child(index, depth);
                    tok().byte_value = u8();
                    add_child(index, depth);
                    tok().word = u16();
                    break;

                // Tokens without operands.
                case TokenKind::Stop:
                case TokenKind::Nothing:
                case TokenKind::EndParmValue:
                case TokenKind::EndFunctionParms:
                case TokenKind::Self:
                case TokenKind::IntZero:
                case TokenKind::IntOne:
                case TokenKind::True:
                case TokenKind::False:
                case TokenKind::NoObject:
                case TokenKind::IteratorPop:
                case TokenKind::IteratorNext:
                case TokenKind::EmptyDelegate:
                case TokenKind::EmptyParm:
                case TokenKind::Unresolved:
                case TokenKind::UndefinedVariable:
                case TokenKind::EndOfScript:
                    break;
                }

                Token& t = m_script.tokens[index];
                t.size = static_cast<std::uint32_t>(m_reader.position() - start);
                return index;
            }

            bool m_is_none(const FName& n) const { return m_none_index >= 0 ? n.index == m_none_index : false; }

        public:
            std::int32_t m_none_index = -1;

        private:
            BinaryReader& m_reader;
            std::size_t m_memory_size;
            const BytecodeOptions& m_options;
            const TokenMap& m_map;
            std::uint16_t m_version;
            std::size_t m_start;
            std::size_t m_memory = 0;
            std::size_t m_name_size = 8;
            std::size_t m_object_size = 4;
            Script m_script;
        };

        // The inverse of Decoder: writes a token tree back as bytecode. Operand fields are written as they are (jump
        // targets and skip sizes too); terminators that the decoder consumes without keeping (EndFunctionParms,
        // EndParmValue, the LabelTable's "None" entry) are written from the token kinds.
        class Encoder
        {
        public:
            Encoder(BinaryWriter& writer, const Script& script, const TokenMap& map)
                : m_writer(writer), m_script(script), m_map(map), m_version(writer.context().file_version)
            {
                m_name_size = m_version >= vNameSizeTo8 ? 8 : 4;
                m_object_size = m_version >= vObjectSizeTo8 ? 8 : 4;
                m_opcode_of.fill(-1);
                for (std::size_t op = 0; op < m_map.kinds.size(); ++op)
                {
                    const auto k = static_cast<std::size_t>(m_map.kinds[op]);
                    if (m_map.kinds[op] != TokenKind::Bad && m_opcode_of[k] < 0)
                    {
                        m_opcode_of[k] = static_cast<int>(op);
                    }
                }
            }

            std::size_t run()
            {
                for (const std::uint32_t s : m_script.statements)
                {
                    token(s, 0);
                }
                return m_memory;
            }

        private:
            [[noreturn]] void fail(const std::string& message) const
            {
                throw Error(ErrorCode::InvalidArgument, "encode_script: " + message);
            }

            void u8(std::uint8_t v)
            {
                m_memory += 1;
                m_writer.write_u8(v);
            }
            void u16(std::uint16_t v)
            {
                m_memory += 2;
                m_writer.write_u16(v);
            }
            void i32(std::int32_t v)
            {
                m_memory += 4;
                m_writer.write_i32(v);
            }
            void f32(float v)
            {
                m_memory += 4;
                m_writer.write_f32(v);
            }
            void object(ObjectIndex o)
            {
                m_memory += m_object_size;
                m_writer.write_i32(o.value);
            }
            void name(const FName& n)
            {
                m_memory += m_name_size;
                write_fname(m_writer, n);
            }

            // The opcode of a kind: the token's own when the map agrees (a kind can have several), else the map's first
            std::uint8_t opcode_for(TokenKind kind, std::uint8_t own) const
            {
                if (own < m_map.kinds.size() && m_map.kinds[own] == kind)
                {
                    return own;
                }
                const int op = m_opcode_of[static_cast<std::size_t>(kind)];
                if (op < 0)
                {
                    fail(fmt::format("no opcode for {} in the token map", to_string(kind)));
                }
                return static_cast<std::uint8_t>(op);
            }

            void terminator(TokenKind kind) { u8(opcode_for(kind, 0xFF)); }

            const Token& at(std::uint32_t index) const
            {
                if (index >= m_script.tokens.size())
                {
                    fail(fmt::format("token index {} out of range", index));
                }
                return m_script.tokens[index];
            }

            // Operand `i` of `t`, which must exist
            void child(const Token& t, std::size_t i, std::size_t depth)
            {
                if (t.children.size() <= i)
                {
                    fail(fmt::format("{} has {} operands, needs at least {}", to_string(t.kind), t.children.size(), i + 1));
                }
                token(t.children[i], depth + 1);
            }

            // Call operands, then the terminator; a DebugInfo after them is kept as the last child (Decoder::maybe_debug)
            void params(const Token& t, std::size_t depth, TokenKind end = TokenKind::EndFunctionParms)
            {
                std::size_t n = t.children.size();
                const bool debug = end == TokenKind::EndFunctionParms && n > 0 && at(t.children.back()).kind == TokenKind::DebugInfo;
                if (debug)
                {
                    --n;
                }
                for (std::size_t i = 0; i < n; ++i)
                {
                    token(t.children[i], depth + 1);
                }
                terminator(end);
                if (debug)
                {
                    token(t.children.back(), depth + 1);
                }
            }

            void array_method(const Token& t, std::size_t depth, std::size_t after_skip, std::uint16_t skip_version)
            {
                child(t, 0, depth);
                if (m_version >= skip_version)
                {
                    u16(t.word);
                }
                for (std::size_t i = 0; i < after_skip; ++i)
                {
                    child(t, 1 + i, depth);
                }
                if (m_version >= vEndTokenAppendedToArrayIntrinsics)
                {
                    terminator(TokenKind::EndFunctionParms);
                }
                if (t.children.size() > 1 + after_skip)
                {
                    token(t.children[1 + after_skip], depth + 1); // DebugInfo
                }
            }

            void native_opcode(std::uint16_t index)
            {
                if (index >= m_map.first_native && index < 0x100)
                {
                    u8(static_cast<std::uint8_t>(index));
                    return;
                }
                const unsigned high = index >> 8u;
                if (m_map.extended_native + high >= m_map.first_native)
                {
                    fail(fmt::format("native index {} can't be encoded", index));
                }
                u8(static_cast<std::uint8_t>(m_map.extended_native + high));
                u8(static_cast<std::uint8_t>(index & 0xFFu));
            }

            void token(std::uint32_t index, std::size_t depth)
            {
                if (depth > 256)
                {
                    fail("expression nesting too deep");
                }
                const Token& t = at(index);
                if (t.kind == TokenKind::NativeFunction)
                {
                    native_opcode(t.native_index);
                    params(t, depth);
                    return;
                }
                if (t.kind == TokenKind::Bad)
                {
                    fail("a Bad token can't be encoded");
                }
                u8(opcode_for(t.kind, t.opcode));
                switch (t.kind)
                {
                case TokenKind::Bad:
                case TokenKind::NativeFunction:
                    break;

                case TokenKind::LocalVariable:
                case TokenKind::InstanceVariable:
                case TokenKind::DefaultVariable:
                case TokenKind::StateVariable:
                case TokenKind::NativeParameter:
                case TokenKind::OutVariable:
                case TokenKind::ObjectConst:
                    object(t.object);
                    break;

                case TokenKind::Return:
                case TokenKind::GotoLabel:
                case TokenKind::BoolVariable:
                case TokenKind::DynamicArrayLength:
                case TokenKind::InterfaceContext:
                    child(t, 0, depth);
                    break;

                case TokenKind::Switch:
                    if (m_version >= vSwitchExpressionField)
                    {
                        object(t.object);
                    }
                    if (m_version >= 536 && m_version <= 587)
                    {
                        u16(t.word2);
                    }
                    else
                    {
                        u8(static_cast<std::uint8_t>(t.word2));
                    }
                    child(t, 0, depth);
                    break;

                case TokenKind::Jump:
                case TokenKind::FilterEditorOnly:
                    u16(t.word);
                    break;

                case TokenKind::JumpIfNot:
                    u16(t.word);
                    child(t, 0, depth);
                    break;

                case TokenKind::Assert:
                    u16(t.word);
                    if (m_version >= vAssertDebugFlag)
                    {
                        u8(t.byte_value);
                    }
                    child(t, 0, depth);
                    break;

                case TokenKind::Case:
                    u16(t.word);
                    if (t.word != 0xFFFF)
                    {
                        child(t, 0, depth);
                    }
                    break;

                case TokenKind::LabelTable:
                    for (const LabelEntry& e : t.labels)
                    {
                        name(e.name);
                        i32(e.offset);
                    }
                    name(t.name); // The "None" terminator, as decoded
                    i32(t.int_value);
                    break;

                case TokenKind::EatReturnValue:
                case TokenKind::ReturnNothing:
                    if (m_version >= vEatReturnValueProperty)
                    {
                        object(t.object);
                    }
                    break;

                case TokenKind::Let:
                case TokenKind::LetBool:
                case TokenKind::LetDelegate:
                case TokenKind::DynamicArrayElement:
                case TokenKind::ArrayElement:
                    child(t, 0, depth);
                    child(t, 1, depth);
                    break;

                case TokenKind::New:
                    for (std::size_t i = 0; i < (m_version > vNewTemplate ? 5u : 4u); ++i)
                    {
                        child(t, i, depth);
                    }
                    break;

                case TokenKind::Context:
                case TokenKind::ClassContext:
                    child(t, 0, depth);
                    u16(t.word);
                    if (m_version >= vContextPropertyAdded)
                    {
                        object(t.object2);
                        u8(static_cast<std::uint8_t>(t.word2));
                    }
                    else if (m_version > 512)
                    {
                        u16(t.word2);
                    }
                    else
                    {
                        u8(static_cast<std::uint8_t>(t.word2));
                    }
                    child(t, 1, depth);
                    break;

                case TokenKind::MetaClassCast:
                case TokenKind::DynamicCast:
                case TokenKind::InterfaceCast:
                    object(t.object);
                    child(t, 0, depth);
                    break;

                case TokenKind::Skip:
                    u16(t.word);
                    child(t, 0, depth);
                    break;

                case TokenKind::VirtualFunction:
                    if (m_version >= 178 && m_version <= 241)
                    {
                        u8(t.byte_value);
                    }
                    name(t.name);
                    params(t, depth);
                    break;

                case TokenKind::FinalFunction:
                    object(t.object);
                    params(t, depth);
                    break;

                case TokenKind::GlobalFunction:
                    name(t.name);
                    params(t, depth);
                    break;

                case TokenKind::DelegateFunction:
                    if (m_version >= vIsLocalAddedToDelegateFunction)
                    {
                        u8(t.byte_value);
                    }
                    object(t.object);
                    name(t.name);
                    params(t, depth);
                    break;

                case TokenKind::DelegateCmpEq:
                case TokenKind::DelegateCmpNe:
                case TokenKind::DelegateFunctionCmpEq:
                case TokenKind::DelegateFunctionCmpNe:
                    for (const std::uint32_t c : t.children)
                    {
                        token(c, depth + 1);
                    }
                    terminator(TokenKind::EndFunctionParms);
                    break;

                case TokenKind::IntConst: i32(t.int_value); break;
                case TokenKind::FloatConst: f32(t.floats[0]); break;
                case TokenKind::StringConst:
                {
                    const auto bytes = Text::utf8_to_cp1252(t.string_value);
                    if (!bytes)
                    {
                        fail("a string constant has characters outside Windows-1252");
                    }
                    for (const std::uint8_t c : *bytes)
                    {
                        u8(c);
                    }
                    u8(0);
                    break;
                }
                case TokenKind::UnicodeStringConst:
                {
                    const auto units = Text::utf8_to_utf16(t.string_value);
                    if (!units)
                    {
                        fail("a unicode string constant is not valid UTF-8");
                    }
                    for (const char16_t c : *units)
                    {
                        u16(static_cast<std::uint16_t>(c));
                    }
                    u16(0);
                    break;
                }
                case TokenKind::NameConst: name(t.name); break;
                case TokenKind::RotationConst:
                    for (const auto v : t.ints)
                    {
                        i32(v);
                    }
                    break;
                case TokenKind::VectorConst:
                    for (const auto v : t.floats)
                    {
                        f32(v);
                    }
                    break;
                case TokenKind::ByteConst:
                case TokenKind::IntConstByte: u8(t.byte_value); break;

                case TokenKind::Iterator:
                    child(t, 0, depth);
                    u16(t.word);
                    break;

                case TokenKind::StructCmpEq:
                case TokenKind::StructCmpNe:
                    object(t.object);
                    child(t, 0, depth);
                    child(t, 1, depth);
                    break;

                case TokenKind::StructMember:
                    object(t.object);
                    if (m_version >= vStructReferenceAddedToStructMember)
                    {
                        object(t.object2);
                    }
                    if (m_version >= vIsCopyAddedToStructMember)
                    {
                        u8(t.byte_value);
                    }
                    if (m_version >= vIsModificationAddedToStructMember)
                    {
                        u8(t.byte_value2);
                    }
                    child(t, 0, depth);
                    break;

                case TokenKind::PrimitiveCast:
                    u8(t.byte_value);
                    child(t, 0, depth);
                    break;

                case TokenKind::DynamicArrayInsert:
                case TokenKind::DynamicArrayRemove: array_method(t, depth, 2, 0xFFFF); break;
                case TokenKind::DynamicArrayFind: array_method(t, depth, 1, vSkipSizeAddedToArrayFind); break;
                case TokenKind::DynamicArrayFindStruct: array_method(t, depth, 2, vSkipSizeAddedToArrayFind); break;
                case TokenKind::DynamicArrayAddItem:
                case TokenKind::DynamicArrayRemoveItem:
                case TokenKind::DynamicArraySort: array_method(t, depth, 1, vSkipSizeAddedToArrayIntrinsics); break;
                case TokenKind::DynamicArrayInsertItem: array_method(t, depth, 2, vSkipSizeAddedToArrayIntrinsics); break;
                case TokenKind::DynamicArrayAdd:
                    child(t, 0, depth);
                    child(t, 1, depth);
                    if (m_version >= vEndTokenAppendedToArrayIntrinsics)
                    {
                        terminator(TokenKind::EndFunctionParms);
                    }
                    if (t.children.size() > 2)
                    {
                        token(t.children[2], depth + 1); // DebugInfo
                    }
                    break;

                case TokenKind::DebugInfo:
                    for (const auto v : t.ints)
                    {
                        i32(v);
                    }
                    u8(t.byte_value);
                    break;

                case TokenKind::DelegateProperty:
                    name(t.name);
                    if (m_version >= vDelegatePropertyObject)
                    {
                        object(t.object);
                    }
                    break;

                case TokenKind::Conditional:
                    child(t, 0, depth);
                    u16(t.word);
                    child(t, 1, depth);
                    u16(t.word2);
                    child(t, 2, depth);
                    break;

                case TokenKind::DefaultParameter:
                    u16(t.word);
                    params(t, depth, TokenKind::EndParmValue);
                    break;

                case TokenKind::InstanceDelegate: name(t.name); break;

                case TokenKind::DynamicArrayIterator:
                    child(t, 0, depth);
                    child(t, 1, depth);
                    u8(t.byte_value);
                    child(t, 2, depth);
                    u16(t.word);
                    break;

                case TokenKind::Stop:
                case TokenKind::Nothing:
                case TokenKind::EndParmValue:
                case TokenKind::EndFunctionParms:
                case TokenKind::Self:
                case TokenKind::IntZero:
                case TokenKind::IntOne:
                case TokenKind::True:
                case TokenKind::False:
                case TokenKind::NoObject:
                case TokenKind::IteratorPop:
                case TokenKind::IteratorNext:
                case TokenKind::EmptyDelegate:
                case TokenKind::EmptyParm:
                case TokenKind::Unresolved:
                case TokenKind::UndefinedVariable:
                case TokenKind::EndOfScript:
                    break;
                }
            }

            BinaryWriter& m_writer;
            const Script& m_script;
            const TokenMap& m_map;
            std::uint16_t m_version;
            std::size_t m_memory = 0;
            std::size_t m_name_size = 8;
            std::size_t m_object_size = 4;
            std::array<int, 256> m_opcode_of{};
        };
    }

    std::string_view to_string(TokenKind kind) noexcept
    {
        for (const auto& [value, text] : kind_names)
        {
            if (value == kind)
            {
                return text;
            }
        }
        return "Bad";
    }

    TokenKind parse_token_kind(std::string_view name) noexcept
    {
        for (const auto& [value, text] : kind_names)
        {
            if (text == name)
            {
                return value;
            }
        }
        return TokenKind::Bad;
    }

    TokenMap TokenMap::ue3(std::uint16_t file_version, const GameProfile* profile)
    {
        if (file_version < 178)
        {
            throw UnsupportedError(fmt::format("bytecode of file version {} (< 178) is not supported", file_version));
        }
        using K = TokenKind;
        TokenMap map;
        map.kinds.fill(K::Bad);
        const std::pair<int, K> entries[] = {
            {0x00, K::LocalVariable},      {0x01, K::InstanceVariable},  {0x02, K::DefaultVariable},
            {0x03, K::StateVariable},      {0x04, K::Return},            {0x05, K::Switch},
            {0x06, K::Jump},               {0x07, K::JumpIfNot},         {0x08, K::Stop},
            {0x09, K::Assert},             {0x0A, K::Case},              {0x0B, K::Nothing},
            {0x0C, K::LabelTable},         {0x0D, K::GotoLabel},         {0x0E, K::EatReturnValue},
            {0x0F, K::Let},                {0x10, K::DynamicArrayElement}, {0x11, K::New},
            {0x12, K::ClassContext},       {0x13, K::MetaClassCast},     {0x14, K::LetBool},
            {0x15, K::EndParmValue},       {0x16, K::EndFunctionParms},  {0x17, K::Self},
            {0x18, K::Skip},               {0x19, K::Context},           {0x1A, K::ArrayElement},
            {0x1B, K::VirtualFunction},    {0x1C, K::FinalFunction},     {0x1D, K::IntConst},
            {0x1E, K::FloatConst},         {0x1F, K::StringConst},       {0x20, K::ObjectConst},
            {0x21, K::NameConst},          {0x22, K::RotationConst},     {0x23, K::VectorConst},
            {0x24, K::ByteConst},          {0x25, K::IntZero},           {0x26, K::IntOne},
            {0x27, K::True},               {0x28, K::False},             {0x29, K::NativeParameter},
            {0x2A, K::NoObject},           {0x2C, K::IntConstByte},      {0x2D, K::BoolVariable},
            {0x2E, K::DynamicCast},        {0x2F, K::Iterator},          {0x30, K::IteratorPop},
            {0x31, K::IteratorNext},       {0x32, K::StructCmpEq},       {0x33, K::StructCmpNe},
            {0x34, K::UnicodeStringConst}, {0x35, K::StructMember},      {0x36, K::DynamicArrayLength},
            {0x37, K::GlobalFunction},     {0x38, K::PrimitiveCast},     {0x39, K::DynamicArrayInsert},
            {0x3A, K::ReturnNothing},      {0x3B, K::DelegateCmpEq},     {0x3C, K::DelegateCmpNe},
            {0x3D, K::DelegateFunctionCmpEq}, {0x3E, K::DelegateFunctionCmpNe}, {0x3F, K::EmptyDelegate},
            {0x40, K::DynamicArrayRemove}, {0x41, K::DebugInfo},         {0x42, K::DelegateFunction},
            {0x43, K::DelegateProperty},   {0x44, K::LetDelegate},       {0x45, K::Conditional},
            {0x46, K::DynamicArrayFind},   {0x47, K::DynamicArrayFindStruct}, {0x48, K::OutVariable},
            {0x49, K::DefaultParameter},   {0x4A, K::EmptyParm},         {0x4B, K::InstanceDelegate},
            {0x50, K::UndefinedVariable},  {0x51, K::InterfaceContext},  {0x52, K::InterfaceCast},
            {0x53, K::EndOfScript},        {0x54, K::DynamicArrayAdd},   {0x55, K::DynamicArrayAddItem},
            {0x56, K::DynamicArrayRemoveItem}, {0x57, K::DynamicArrayInsertItem}, {0x58, K::DynamicArrayIterator},
            {0x59, K::DynamicArraySort},   {0x5A, K::FilterEditorOnly},
        };
        for (const auto& [op, kind] : entries)
        {
            map.kinds[static_cast<std::size_t>(op)] = kind;
        }
        if (profile != nullptr)
        {
            for (const auto& [op, kind_name] : profile->bytecode_overrides)
            {
                if (op >= map.kinds.size())
                {
                    throw Error(ErrorCode::InvalidArgument,
                                fmt::format("bytecode override for native opcode 0x{:02X}", op));
                }
                const TokenKind kind = parse_token_kind(kind_name);
                if (kind == TokenKind::Bad && kind_name != "Bad")
                {
                    throw Error(ErrorCode::InvalidArgument, "unknown bytecode token name '" + kind_name + "'");
                }
                map.kinds[op] = kind;
            }
        }
        return map;
    }

    Script decode_script(BinaryReader& reader, std::size_t memory_size, const BytecodeOptions& options)
    {
        if (options.token_map == nullptr)
        {
            throw Error(ErrorCode::InvalidArgument, "decode_script: options.token_map is required");
        }
        Decoder decoder(reader, memory_size, options);
        decoder.m_none_index = options.none_index;
        return decoder.run();
    }

    std::size_t encode_script(BinaryWriter& writer, const Script& script, const TokenMap& token_map)
    {
        Encoder encoder(writer, script, token_map);
        return encoder.run();
    }
}
