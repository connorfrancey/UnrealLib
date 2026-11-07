#pragma once

/// @file
/// @brief UE3 object flags (EObjectFlags) as stored on disk in export entries (file version >= 195).
///
/// Values are the raw 64-bit on-disk values. They are derived from upstream UELib (`ObjectFlagsLO`/`ObjectFlagsHO` in
/// `UnrealFlags.cs` and the UE3 mapping in `DefaultEngineBranch.cs`), whose exposed value has the two 32-bit halves
/// swapped: an upstream "LO" flag `x` is `x << 32` here and an upstream "HO" flag `y` is `y` here.
/// Cross-checked against Mirror's Edge Core.u: class exports carry Native|Standalone|LoadFor*|Public
/// (0x040F000400000000) and `Default__*` objects carry ClassDefaultObject (0x200).
///
/// Only flags with a known, consistent UE3 meaning are listed.

#include <cstdint>

namespace UnrealLib::ObjectFlag
{
    inline constexpr std::uint64_t Transactional = 0x0000000100000000ull;      ///< Supports undo/redo.
    inline constexpr std::uint64_t Public = 0x0000000400000000ull;             ///< Visible outside its package.
    inline constexpr std::uint64_t Transient = 0x0000400000000000ull;          ///< Not saved.
    inline constexpr std::uint64_t LoadForClient = 0x0001000000000000ull;      ///< Loaded on clients.
    inline constexpr std::uint64_t LoadForServer = 0x0002000000000000ull;      ///< Loaded on servers.
    inline constexpr std::uint64_t LoadForEdit = 0x0004000000000000ull;        ///< Loaded in the editor.
    inline constexpr std::uint64_t Standalone = 0x0008000000000000ull;         ///< Kept even if unreferenced.
    inline constexpr std::uint64_t NotForClient = 0x0010000000000000ull;       ///< Not loaded on clients.
    inline constexpr std::uint64_t NotForServer = 0x0020000000000000ull;       ///< Not loaded on servers.
    inline constexpr std::uint64_t NotForEdit = 0x0040000000000000ull;         ///< Not loaded in the editor.
    inline constexpr std::uint64_t HasStack = 0x0200000000000000ull;           ///< Serialized with a UStateFrame.
    inline constexpr std::uint64_t Native = 0x0400000000000000ull;             ///< Native (C++) object/class.
    inline constexpr std::uint64_t ClassDefaultObject = 0x0000000000000200ull; ///< Class default object (Default__X).
    inline constexpr std::uint64_t ArchetypeObject = 0x0000000000000400ull;    ///< Archetype template.

    /// Either template kind.
    inline constexpr std::uint64_t TemplateObject = ClassDefaultObject | ArchetypeObject;
}
