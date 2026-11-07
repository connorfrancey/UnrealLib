// LZO1X decompressor — an independent implementation of the LZO1X stream format (the format produced by
// lzo1x_1_compress and decoded by lzo1x_decompress_safe). It is written for safety first: every input read, output
// write and look-behind distance is checked, and failures throw CompressionError instead of returning error codes.
//
// Instruction summary (t = instruction byte):
//   first byte > 17      : literal run of (t - 17) bytes (a value < 4 means "trailing literals" of a match)
//   0..15  after a match : M1 — 2-byte match, distance 1 + (t >> 2) + (next << 2)
//   0..15  otherwise     : literal run of t + 3 bytes (t == 0: extended length, base 15)
//   0..15  after a run   : 3-byte match, distance 0x801 + (t >> 2) + (next << 2)
//   16..31               : M4 — distance 0x4000 + ((t & 8) << 11) + (LE16 >> 2), length (t & 7) + 2
//                          (t & 7 == 0: extended length, base 7). Distance 0x4000 encodes end-of-stream.
//   32..63               : M3 — distance 1 + (LE16 >> 2), length (t & 31) + 2 (extended length base 31)
//   64..255              : M2 — distance 1 + ((t >> 2) & 7) + (next << 3), length (t >> 5) + 1
// The low two bits of the byte preceding the match's last byte encode 0–3 trailing literals.

#include "Lzo1x.hpp"

#include <cstring>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>

namespace UnrealLib::detail
{
    namespace
    {
        class Lzo1xDecoder
        {
        public:
            Lzo1xDecoder(std::span<const std::uint8_t> input, std::span<std::uint8_t> output)
                : m_in(input), m_out(output)
            {
            }

            std::size_t run()
            {
                enum class State
                {
                    Instruction,     // expecting a literal run or a match
                    AfterLiteralRun, // the previous instruction was a literal run
                    Match,           // `t` holds a match instruction (previous instruction was a match + literals)
                };

                State state = State::Instruction;
                std::size_t t = 0;

                if (m_in.empty())
                {
                    fail("empty LZO stream");
                }
                if (m_in[0] > 17)
                {
                    t = static_cast<std::size_t>(next()) - 17;
                    if (t < 4)
                    {
                        copy_literals(t);
                        t = next();
                        state = State::Match;
                    }
                    else
                    {
                        copy_literals(t);
                        state = State::AfterLiteralRun;
                    }
                }

                for (;;)
                {
                    std::size_t trailing = 0;
                    switch (state)
                    {
                    case State::Instruction:
                        t = next();
                        if (t >= 16)
                        {
                            state = State::Match;
                            continue;
                        }
                        if (t == 0)
                        {
                            t = extended_length(15);
                        }
                        copy_literals(t + 3);
                        state = State::AfterLiteralRun;
                        continue;

                    case State::AfterLiteralRun:
                        t = next();
                        if (t >= 16)
                        {
                            state = State::Match;
                            continue;
                        }
                        {
                            const std::size_t distance = 1 + 0x0800 + (t >> 2) + (static_cast<std::size_t>(next()) << 2);
                            copy_match(distance, 3);
                            trailing = t & 3u;
                        }
                        break;

                    case State::Match:
                        if (t >= 64)
                        {
                            const std::size_t distance = 1 + ((t >> 2) & 7u) + (static_cast<std::size_t>(next()) << 3);
                            copy_match(distance, (t >> 5) + 1);
                            trailing = t & 3u;
                        }
                        else if (t >= 32)
                        {
                            std::size_t length = t & 31u;
                            if (length == 0)
                            {
                                length = extended_length(31);
                            }
                            const std::size_t b0 = next();
                            const std::size_t b1 = next();
                            copy_match(1 + (b0 >> 2) + (b1 << 6), length + 2);
                            trailing = b0 & 3u;
                        }
                        else if (t >= 16)
                        {
                            const std::size_t high = (t & 8u) << 11;
                            std::size_t length = t & 7u;
                            if (length == 0)
                            {
                                length = extended_length(7);
                            }
                            const std::size_t b0 = next();
                            const std::size_t b1 = next();
                            const std::size_t distance = high + (b0 >> 2) + (b1 << 6);
                            if (distance == 0)
                            {
                                return finish();
                            }
                            copy_match(distance + 0x4000, length + 2);
                            trailing = b0 & 3u;
                        }
                        else
                        {
                            const std::size_t distance = 1 + (t >> 2) + (static_cast<std::size_t>(next()) << 2);
                            copy_match(distance, 2);
                            trailing = t & 3u;
                        }
                        break;
                    }

                    // A match was just copied: 0–3 literals may follow, after which another match instruction comes.
                    if (trailing == 0)
                    {
                        state = State::Instruction;
                    }
                    else
                    {
                        copy_literals(trailing);
                        t = next();
                        state = State::Match;
                    }
                }
            }

        private:
            [[noreturn]] void fail(std::string_view message) const
            {
                throw CompressionError(fmt::format("LZO1X: {} (input {}/{}, output {}/{})", message, m_ip,
                                                   m_in.size(), m_op, m_out.size()),
                                       m_ip);
            }

            std::uint8_t next()
            {
                if (m_ip >= m_in.size())
                {
                    fail("unexpected end of input");
                }
                return m_in[m_ip++];
            }

            // Reads a run of zero bytes (each adding 255) followed by a non-zero byte, plus `base`.
            std::size_t extended_length(std::size_t base)
            {
                std::size_t length = 0;
                while (next() == 0)
                {
                    length += 255;
                    if (length > m_out.size())
                    {
                        fail("extended length exceeds the output size");
                    }
                }
                return length + base + m_in[m_ip - 1];
            }

            void copy_literals(std::size_t count)
            {
                if (count > m_in.size() - m_ip)
                {
                    fail("literal run exceeds the input");
                }
                if (count > m_out.size() - m_op)
                {
                    fail("literal run overruns the output");
                }
                if (count != 0)
                {
                    std::memcpy(m_out.data() + m_op, m_in.data() + m_ip, count);
                }
                m_ip += count;
                m_op += count;
            }

            void copy_match(std::size_t distance, std::size_t length)
            {
                if (distance == 0 || distance > m_op)
                {
                    fail(fmt::format("match distance {} reaches before the start of the output", distance));
                }
                if (length > m_out.size() - m_op)
                {
                    fail("match overruns the output");
                }
                // Byte-by-byte: source and destination may overlap (run-length style matches).
                std::size_t from = m_op - distance;
                for (std::size_t i = 0; i < length; ++i)
                {
                    m_out[m_op++] = m_out[from++];
                }
            }

            std::size_t finish() const
            {
                if (m_ip != m_in.size())
                {
                    fail("trailing data after the end-of-stream marker");
                }
                return m_op;
            }

            std::span<const std::uint8_t> m_in;
            std::span<std::uint8_t> m_out;
            std::size_t m_ip = 0;
            std::size_t m_op = 0;
        };
    }

    std::size_t lzo1x_decompress(std::span<const std::uint8_t> input, std::span<std::uint8_t> output)
    {
        return Lzo1xDecoder(input, output).run();
    }
}
