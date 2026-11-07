#pragma once

/// @file
/// @brief Umbrella header: includes the complete public UnrealLib API.
///
/// Link against the `UnrealLib::UnrealLib` CMake target.

#include <UnrealLib/Core/Diagnostics.hpp>
#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Core/Guid.hpp>
#include <UnrealLib/Core/Log.hpp>
#include <UnrealLib/Core/Version.hpp>

#include <UnrealLib/Utilities/Text.hpp>

#include <UnrealLib/Serialization/Archive.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Serialization/BitStream.hpp>
#include <UnrealLib/Serialization/ByteOrder.hpp>
#include <UnrealLib/Serialization/CompactIndex.hpp>
#include <UnrealLib/Serialization/Compression.hpp>
#include <UnrealLib/Serialization/FString.hpp>
#include <UnrealLib/Serialization/LegacyVersion.hpp>
#include <UnrealLib/Serialization/SerializationContext.hpp>
#include <UnrealLib/Serialization/SerializationLimits.hpp>

#include <UnrealLib/Names/FName.hpp>
#include <UnrealLib/Names/NameTable.hpp>

#include <UnrealLib/Package/CompressedChunk.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Package/ObjectTables.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Package/PackageSummary.hpp>
#include <UnrealLib/Package/PackageValidator.hpp>

#include <UnrealLib/Objects/ObjectFlags.hpp>
#include <UnrealLib/Objects/ObjectHeader.hpp>

#include <UnrealLib/Properties/PropertyTag.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>

#include <UnrealLib/Bytecode/Bytecode.hpp>

#include <UnrealLib/Reflection/Fields.hpp>

#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Decompiler/Decompiler.hpp>
#include <UnrealLib/Decompiler/NativeTable.hpp>
#include <UnrealLib/Decompiler/Statements.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>

#include <UnrealLib/Assets/Animation.hpp>
#include <UnrealLib/Assets/AssetObject.hpp>
#include <UnrealLib/Assets/BulkData.hpp>
#include <UnrealLib/Assets/Image.hpp>
#include <UnrealLib/Assets/Material.hpp>
#include <UnrealLib/Assets/MeshTypes.hpp>
#include <UnrealLib/Assets/SkeletalMesh.hpp>
#include <UnrealLib/Assets/Sound.hpp>
#include <UnrealLib/Assets/StaticMesh.hpp>
#include <UnrealLib/Assets/Texture.hpp>

#include <UnrealLib/Writer/PackageWriter.hpp>
