# ----------------------------------------------------------------------------------------------------------------------
# Examples: LoadPackage (summary, exports, validation), InspectObjects (properties, decompiled scripts),
# ExtractTexture (texture -> PNG).
# ----------------------------------------------------------------------------------------------------------------------

function(unreallib_add_example target output_name source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE UnrealLib::UnrealLib)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name} FOLDER "UnrealLib/Examples")
    unreallib_set_compiler_options(${target})
endfunction()

unreallib_add_example(unreallib_example_load_package UnrealLibExampleLoadPackage LoadPackage/main.cpp)
unreallib_add_example(unreallib_example_inspect_objects UnrealLibExampleInspectObjects InspectObjects/main.cpp)
unreallib_add_example(unreallib_example_extract_texture UnrealLibExampleExtractTexture ExtractTexture/main.cpp)
