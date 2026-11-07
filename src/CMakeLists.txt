# ----------------------------------------------------------------------------------------------------------------------
# UnrealLib modules. Dependency order (no cycles):
#   Core -> Utilities -> Serialization -> Names -> Package -> Objects -> Linker
# Sources are listed explicitly; add new files here when a module grows.
# ----------------------------------------------------------------------------------------------------------------------

unreallib_add_module(Core
        SOURCES
            Core/Diagnostics.cpp
            Core/Error.cpp
            Core/Guid.cpp
            Core/Log.cpp
        PUBLIC_DEPS
            fmt::fmt
        PRIVATE_DEPS
            spdlog::spdlog)

unreallib_add_module(Utilities
        SOURCES
            Utilities/Text.cpp
        PUBLIC_DEPS
            UnrealLib::Core)

unreallib_add_module(Serialization
        SOURCES
            Serialization/Archive.cpp
            Serialization/BinaryReader.cpp
            Serialization/BinaryWriter.cpp
            Serialization/BitStream.cpp
            Serialization/CompactIndex.cpp
            Serialization/Compression.cpp
            Serialization/Lzo1x.cpp
            Serialization/Lzo1x.hpp
        PUBLIC_DEPS
            UnrealLib::Core
            UnrealLib::Utilities
        PRIVATE_DEPS
            ${UNREALLIB_ZLIB_TARGET})

unreallib_add_module(Names
        SOURCES
            Names/FName.cpp
            Names/NameTable.cpp
        PUBLIC_DEPS
            UnrealLib::Serialization)

unreallib_add_module(Package
        SOURCES
            Package/CompressedChunk.cpp
            Package/GameProfile.cpp
            Package/ObjectTables.cpp
            Package/Package.cpp
            Package/PackageSummary.cpp
            Package/PackageValidator.cpp
        PUBLIC_DEPS
            UnrealLib::Names
            UnrealLib::Serialization)

unreallib_add_module(Objects
        SOURCES
            Objects/ObjectHeader.cpp
        PUBLIC_DEPS
            UnrealLib::Package)

unreallib_add_module(Linker
        SOURCES
            Linker/Linker.cpp
        PUBLIC_DEPS
            UnrealLib::Objects
            UnrealLib::Package)

# Umbrella target: link this to get the whole library.
add_library(unreallib INTERFACE)
add_library(UnrealLib::UnrealLib ALIAS unreallib)
target_link_libraries(unreallib INTERFACE
        UnrealLib::Core
        UnrealLib::Utilities
        UnrealLib::Serialization
        UnrealLib::Names
        UnrealLib::Package
        UnrealLib::Objects
        UnrealLib::Linker)
