#include <UnrealLib/Compiler/Compiler.hpp>

int main()
{
    UnrealLib::Linker linker;
    UnrealLib::Compiler::ScriptCompiler compiler(linker, {});
    const auto result = compiler.build_class_package("CompilerConsumer", "");
    return result.errors.empty() ? 1 : 0;
}
