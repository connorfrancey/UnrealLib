#pragma once

/// @file
/// @brief Exception hierarchy used throughout UnrealLib.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace UnrealLib
{
    /// @brief Broad classification of an error.
    enum class ErrorCode : std::uint8_t
    {
        Unknown,         ///< Unclassified error.
        OutOfBounds,     ///< A read, seek or offset fell outside the available data.
        InvalidData,     ///< Data was structurally malformed.
        LimitExceeded,   ///< A configured allocation or size limit was exceeded.
        Unsupported,     ///< The input uses a feature/version/format that UnrealLib does not support.
        Io,              ///< A file-system or stream error.
        Compression,     ///< Compressed data could not be decoded.
        InvalidArgument, ///< A caller-supplied argument was invalid.
        InvalidState,    ///< An operation was attempted in an invalid state (e.g. package not loaded).
        NotFound,        ///< A requested entity (profile, object, file) does not exist.
    };

    /// @brief Returns a stable, human-readable name for @p code.
    [[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

    /// @brief Base class of every exception thrown by UnrealLib.
    ///
    /// Carries an ErrorCode and, when known, the absolute byte offset in the input at which the error was detected.
    class Error : public std::runtime_error
    {
    public:
        /// @param code    Error classification.
        /// @param message Human-readable description (without offset; the offset is appended automatically).
        /// @param offset  Absolute byte offset in the input data, if applicable.
        Error(ErrorCode code, std::string_view message, std::optional<std::uint64_t> offset = std::nullopt);

        /// @brief Error classification.
        [[nodiscard]] ErrorCode code() const noexcept { return m_code; }

        /// @brief Absolute byte offset at which the error was detected, if known.
        [[nodiscard]] const std::optional<std::uint64_t>& offset() const noexcept { return m_offset; }

        /// @brief The message without the appended offset.
        [[nodiscard]] const std::string& message() const noexcept { return m_message; }

    private:
        ErrorCode m_code;
        std::optional<std::uint64_t> m_offset;
        std::string m_message;
    };

    /// @brief Thrown when binary (de)serialization fails: truncated data, bad lengths, out-of-bounds reads.
    class SerializationError : public Error
    {
    public:
        using Error::Error;

        /// @brief Convenience constructor using ErrorCode::InvalidData.
        explicit SerializationError(std::string_view message, std::optional<std::uint64_t> offset = std::nullopt)
            : Error(ErrorCode::InvalidData, message, offset)
        {
        }
    };

    /// @brief Thrown when compressed data cannot be decoded.
    class CompressionError : public Error
    {
    public:
        /// @param message Description.
        /// @param offset  Offset (within the compressed input) where decoding failed, if known.
        explicit CompressionError(std::string_view message, std::optional<std::uint64_t> offset = std::nullopt)
            : Error(ErrorCode::Compression, message, offset)
        {
        }
    };

    /// @brief Thrown when a package is structurally invalid or cannot be loaded.
    class PackageError : public Error
    {
    public:
        using Error::Error;

        /// @brief Convenience constructor using ErrorCode::InvalidData.
        explicit PackageError(std::string_view message, std::optional<std::uint64_t> offset = std::nullopt)
            : Error(ErrorCode::InvalidData, message, offset)
        {
        }
    };

    /// @brief Thrown for file-system and stream failures.
    class IoError : public Error
    {
    public:
        /// @param message Description (should include the path).
        explicit IoError(std::string_view message) : Error(ErrorCode::Io, message) {}
    };

    /// @brief Thrown when the input uses a feature that UnrealLib deliberately does not (yet) support.
    class UnsupportedError : public Error
    {
    public:
        /// @param message Description of the unsupported feature.
        /// @param offset  Offset of the unsupported data, if applicable.
        explicit UnsupportedError(std::string_view message, std::optional<std::uint64_t> offset = std::nullopt)
            : Error(ErrorCode::Unsupported, message, offset)
        {
        }
    };
}
