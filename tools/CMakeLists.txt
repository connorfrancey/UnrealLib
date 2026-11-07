# ----------------------------------------------------------------------------------------------------------------------
# Command-line tools (CLI11). PackageExtractor will be added with the asset milestone (M7).
# ----------------------------------------------------------------------------------------------------------------------

add_library(unreallib_tools_common STATIC
        Common/PackageJson.cpp
        Common/PackageJson.hpp)
target_link_libraries(unreallib_tools_common PUBLIC UnrealLib::UnrealLib nlohmann_json::nlohmann_json)
unreallib_set_compiler_options(unreallib_tools_common)

function(unreallib_add_tool target output_name)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE unreallib_tools_common CLI11::CLI11)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name} FOLDER "UnrealLib/Tools")
    unreallib_set_compiler_options(${target})
endfunction()

unreallib_add_tool(unreallib_package_inspector UnrealPackageInspector PackageInspector/main.cpp)
unreallib_add_tool(unreallib_package_validator UnrealPackageValidator PackageValidator/main.cpp)
unreallib_add_tool(unreallib_script_decompiler UnrealScriptDecompiler ScriptDecompiler/main.cpp)
