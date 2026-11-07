#pragma once

/// @file
/// @brief PropertySchema backed by the reflection data (UClass/UScriptStruct/U*Property exports) of linked packages.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Reflection/Fields.hpp>
#include <UnrealLib/Utilities/Text.hpp>

namespace UnrealLib
{
    /// @brief Type information from script packages.
    ///
    /// Classes and structs are looked up by name among the exports of the packages added with add_package()
    /// (normally the game's script packages: Core, Engine, …). Supers and property types are followed through the
    /// Linker, so they may live in other packages. Everything is read lazily and cached.
    ///
    /// Struct layouts: fields of the supers first, then the struct's own properties in children-list order; a
    /// struct is binary when it is Immutable, or ImmutableWhenCooked and @p cooked is set (UE3 >= 279 semantics;
    /// flags are not inherited).
    ///
    /// Not thread-safe; references into the Linker must stay valid for the schema's lifetime.
    class ReflectionSchema final : public PropertySchema
    {
    public:
        /// @param linker Linker used to resolve and read reflection objects.
        /// @param cooked Whether the data being decoded comes from cooked packages (selects ImmutableWhenCooked).
        explicit ReflectionSchema(Linker& linker, bool cooked = true);

        /// @brief Indexes the class and struct exports of @p package.
        void add_package(const Package& package);

        [[nodiscard]] const StructLayout* find_struct(std::string_view name) const override;
        [[nodiscard]] std::optional<TypeDesc> find_property(std::string_view owner,
                                                            std::string_view property) const override;

        /// @brief The reflection object of @p ref (cached). Returns nullptr if it cannot be read.
        [[nodiscard]] const FieldObject* field(ObjectRef ref) const;

        /// @brief The class or struct export named @p name, if indexed.
        [[nodiscard]] std::optional<ObjectRef> find_type(std::string_view name) const;

        /// @brief Problems met while reading reflection data (reported once per object).
        [[nodiscard]] const Diagnostics& diagnostics() const noexcept { return m_diagnostics; }

    private:
        [[nodiscard]] std::optional<ObjectRef> resolve_export(ObjectRef ref) const;
        [[nodiscard]] std::optional<TypeDesc> type_of(ObjectRef property) const;
        [[nodiscard]] std::optional<ObjectRef> find_member(ObjectRef owner, std::string_view name) const;

        Linker& m_linker;
        bool m_cooked;
        std::unordered_map<std::string, ObjectRef, Text::CaseInsensitiveHash, Text::CaseInsensitiveEqual> m_types;
        mutable std::map<std::pair<const Package*, std::int32_t>, std::unique_ptr<FieldObject>> m_fields;
        mutable std::unordered_map<std::string, std::unique_ptr<StructLayout>, Text::CaseInsensitiveHash,
                                   Text::CaseInsensitiveEqual>
            m_layouts;
        mutable std::map<std::pair<std::string, std::string>, std::optional<TypeDesc>> m_property_cache;
        mutable Diagnostics m_diagnostics;
    };
}
