#include <UnrealLib/Decompiler/NativeTable.hpp>

#include <utility>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Reflection/Fields.hpp>

namespace UnrealLib::Decompiler
{
    CallSyntax call_syntax(std::uint32_t function_flags, std::uint8_t precedence) noexcept
    {
        if ((function_flags & FunctionFlag::Operator) == 0)
        {
            return CallSyntax::Function;
        }
        if ((function_flags & FunctionFlag::PreOperator) != 0)
        {
            return CallSyntax::PreOperator;
        }
        return precedence == 0 ? CallSyntax::PostOperator : CallSyntax::Operator;
    }

    void NativeTable::add(std::uint16_t index, NativeFunction function)
    {
        m_entries[index] = std::move(function);
    }

    const NativeFunction* NativeTable::find(std::uint16_t index) const
    {
        const auto it = m_entries.find(index);
        return it == m_entries.end() ? nullptr : &it->second;
    }

    NativeTable NativeTable::from_packages(Linker& linker, std::span<const Package* const> packages,
                                           Diagnostics* diagnostics)
    {
        NativeTable table;
        for (const Package* package : packages)
        {
            for (std::size_t i = 0; i < package->exports().size(); ++i)
            {
                const ObjectRef ref{package, ObjectIndex::from_export(i)};
                if (package->class_name(ref.index) != "Function" || package->exports()[i].serial_size <= 0)
                {
                    continue;
                }
                try
                {
                    const FieldObject field = linker.read_field(ref);
                    if (!field.function || (field.function->flags & FunctionFlag::Native) == 0 ||
                        field.function->native_token == 0)
                    {
                        continue;
                    }
                    const FunctionData& f = *field.function;
                    NativeFunction entry;
                    entry.name = f.friendly_name ? package->names().to_string(*f.friendly_name)
                                                 : package->object_name(ref.index);
                    entry.syntax = call_syntax(f.flags, f.oper_precedence);
                    entry.precedence = f.oper_precedence;
                    table.add(f.native_token, std::move(entry));
                }
                catch (const Error& error)
                {
                    if (diagnostics != nullptr)
                    {
                        diagnostics->warning("decompiler.native-table",
                                         linker.qualified_path(ref) + ": " + error.what());
                    }
                }
            }
        }
        return table;
    }
}
