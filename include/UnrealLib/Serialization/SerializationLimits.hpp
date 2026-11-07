#pragma once

/// @file
/// @brief Upper bounds that protect against malformed input causing uncontrolled allocations.

#include <cstdint>

namespace UnrealLib
{
    /// @brief Allocation and size limits enforced by every reader.
    ///
    /// Independently of these limits, a length/count read from the stream is also rejected when the remaining input
    /// cannot possibly hold that many elements, so most malformed counts fail before any allocation.
    struct SerializationLimits
    {
        /// Maximum length (in characters, including terminator) of a single serialized string.
        std::uint32_t max_string_length = 1u << 20;

        /// Maximum element count of a single serialized array or table.
        std::uint32_t max_array_count = 1u << 24;

        /// Maximum size, in bytes, of any single buffer allocated while deserializing.
        std::uint64_t max_allocation_bytes = 1ull << 30;

        /// Maximum total size, in bytes, of a decompressed package.
        std::uint64_t max_decompressed_size = 1ull << 31;

        /// Maximum size, in bytes, of a file loaded into memory.
        std::uint64_t max_file_size = 1ull << 31;
    };
}
