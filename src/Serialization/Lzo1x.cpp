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

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

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

    namespace
    {
        class Lzo1xEncoder
        {
        public:
            explicit Lzo1xEncoder(std::span<const std::uint8_t> input) : m_in(input) {}

            std::vector<std::uint8_t> run()
            {
                m_out.reserve(m_in.size() + m_in.size() / 16 + 64);
                m_head.fill(-1);
                m_prev.assign(m_in.size(), -1);
                std::size_t literal_start = 0;
                std::size_t ip = 0;
                while (ip + 3 <= m_in.size())
                {
                    std::size_t best_length = 0, best_distance = 0;
                    find_match(ip, best_length, best_distance);
                    if (best_length < 3)
                    {
                        insert(ip);
                        ++ip;
                        continue;
                    }
                    emit_literals(literal_start, ip);
                    emit_match(best_distance, best_length);
                    for (std::size_t k = 0; k < best_length; ++k) insert(ip + k);
                    ip += best_length;
                    literal_start = ip;
                }
                emit_literals(literal_start, m_in.size());
                // End-of-stream marker: M4 with distance 0 (0x11 0x00 0x00).
                m_out.push_back(0x11);
                m_out.push_back(0x00);
                m_out.push_back(0x00);
                return std::move(m_out);
            }

        private:
            static constexpr std::size_t hash_bits = 15;
            static constexpr std::size_t max_distance = 0xBFFF;
            static constexpr std::size_t max_chain = 32;
            static constexpr std::size_t max_match = 2048;

            std::size_t hash(std::size_t p) const noexcept
            {
                const std::uint32_t v = static_cast<std::uint32_t>(m_in[p]) |
                                        static_cast<std::uint32_t>(m_in[p + 1]) << 8 |
                                        static_cast<std::uint32_t>(m_in[p + 2]) << 16;
                return (v * 2654435761u) >> (32 - hash_bits);
            }

            void insert(std::size_t p)
            {
                if (p + 3 > m_in.size()) return;
                const std::size_t h = hash(p);
                m_prev[p] = m_head[h];
                m_head[h] = static_cast<std::int64_t>(p);
            }

            void find_match(std::size_t ip, std::size_t& best_length, std::size_t& best_distance) const
            {
                std::int64_t candidate = m_head[hash(ip)];
                const std::size_t limit = std::min(max_match, m_in.size() - ip);
                for (std::size_t chain = 0; candidate >= 0 && chain < max_chain; ++chain)
                {
                    const auto c = static_cast<std::size_t>(candidate);
                    const std::size_t distance = ip - c;
                    if (distance > max_distance) break;
                    std::size_t length = 0;
                    while (length < limit && m_in[c + length] == m_in[ip + length]) ++length;
                    if (length > best_length)
                    {
                        best_length = length;
                        best_distance = distance;
                        if (length == limit) break;
                    }
                    candidate = m_prev[c];
                }
            }

            void extended(std::size_t n)
            {
                while (n > 255)
                {
                    m_out.push_back(0);
                    n -= 255;
                }
                m_out.push_back(static_cast<std::uint8_t>(n));
            }

            void emit_literals(std::size_t from, std::size_t to)
            {
                const std::size_t count = to - from;
                if (count == 0) return;
                if (m_out.empty())
                {
                    // First instruction: one byte 17 + count (count <= 238) introduces the run.
                    if (count <= 238)
                    {
                        m_out.push_back(static_cast<std::uint8_t>(17 + count));
                    }
                    else
                    {
                        m_out.push_back(0);
                        extended(count - 18);
                    }
                }
                else if (count <= 3)
                {
                    // Folded into the low two bits of the previous match's distance byte.
                    m_out[m_trailing_byte] = static_cast<std::uint8_t>(m_out[m_trailing_byte] | count);
                }
                else if (count <= 18)
                {
                    m_out.push_back(static_cast<std::uint8_t>(count - 3));
                }
                else
                {
                    m_out.push_back(0);
                    extended(count - 18);
                }
                m_out.insert(m_out.end(), m_in.begin() + static_cast<std::ptrdiff_t>(from),
                             m_in.begin() + static_cast<std::ptrdiff_t>(to));
            }

            void emit_match(std::size_t distance, std::size_t length)
            {
                if (distance <= 0x4000)
                {
                    const std::size_t d = distance - 1;
                    if (length - 2 <= 31)
                    {
                        m_out.push_back(static_cast<std::uint8_t>(32 | (length - 2)));
                    }
                    else
                    {
                        m_out.push_back(32);
                        extended(length - 2 - 31);
                    }
                    m_trailing_byte = m_out.size();
                    m_out.push_back(static_cast<std::uint8_t>((d & 63) << 2));
                    m_out.push_back(static_cast<std::uint8_t>(d >> 6));
                }
                else
                {
                    const std::size_t d = distance - 0x4000;
                    const auto high = static_cast<std::uint8_t>((d >> 11) & 8);
                    if (length - 2 <= 7)
                    {
                        m_out.push_back(static_cast<std::uint8_t>(16 | high | (length - 2)));
                    }
                    else
                    {
                        m_out.push_back(static_cast<std::uint8_t>(16 | high));
                        extended(length - 2 - 7);
                    }
                    m_trailing_byte = m_out.size();
                    m_out.push_back(static_cast<std::uint8_t>((d & 63) << 2));
                    m_out.push_back(static_cast<std::uint8_t>((d >> 6) & 0xFF));
                }
            }

            std::span<const std::uint8_t> m_in;
            std::vector<std::uint8_t> m_out;
            std::array<std::int64_t, std::size_t{1} << hash_bits> m_head{};
            std::vector<std::int64_t> m_prev;
            std::size_t m_trailing_byte = 0;
        };
    }

    std::vector<std::uint8_t> lzo1x_compress(std::span<const std::uint8_t> input)
    {
        return Lzo1xEncoder(input).run();
    }
}
