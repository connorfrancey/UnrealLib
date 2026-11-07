#pragma once

/// @file
/// @brief Package-version gates used by the serializers.
///
/// Values are taken verbatim from upstream UELib `src/Branch/PackageObjectLegacyVersion.cs` (enum
/// PackageObjectLegacyVersion). Only gates that UnrealLib actually consumes are listed; add new ones together with
/// the code that uses them and keep the upstream name.

#include <cstdint>

namespace UnrealLib
{
    /// @brief Package file-version thresholds (feature is present when `file_version >= value`, unless the name says
    ///        "Deprecated"/"Removed", in which case the feature is absent from that version on).
    enum class LegacyVersion : std::uint16_t
    {
        HeritageTableDeprecated = 68,
        Release64 = 64,                         ///< Name table entries become length-prefixed FStrings.
        CompactIndexDeprecated = 178,           ///< Indices are plain int32 from here on (UE3).
        AddedComponentMapToExports = 178,       ///< == UE3.
        AddedStateStackToUStateFrame = 189,
        ObjectFlagsSizeExpandedTo64Bits = 195,
        ArchetypeAddedToExports = 220,
        TemplateDataAddedToUComponent = 224,
        AddedEngineVersion = 245,
        ExportFlagsAddedToExports = 247,
        AddedTotalHeaderSize = 249,
        SerialSizeConditionRemoved = 249,
        ClassDefaultCheckAddedToTemplateName = 267,
        AddedFolderName = 269,
        AddedCookerVersion = 277,
        NetObjectCountAdded = 322,
        CompressionAdded = 334,
        NumberAddedToName = 343,
        AddedDependsTable = 415,
        PackageFlagsAddedToExports = 475,
        AddedPackageSource = 482,
        AddedAdditionalPackagesToCook = 516,
        ComponentMapDeprecated = 543,
        StateFrameLatentActionReduced = 566,
        AddedThumbnailTable = 584,
        AddedImportExportGuidsTable = 623,
        ProbeMaskReducedAndIgnoreMaskRemoved = 691,
        AddedTextureAllocations = 767,
    };

    /// @brief Numeric value of a LegacyVersion gate.
    [[nodiscard]] constexpr std::uint16_t to_value(LegacyVersion version) noexcept
    {
        return static_cast<std::uint16_t>(version);
    }
}
