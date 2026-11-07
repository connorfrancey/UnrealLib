# ----------------------------------------------------------------------------------------------------------------------
# Examples. InspectObjects and ExtractTexture will be added with milestones M3 (objects) and M7 (assets).
# ----------------------------------------------------------------------------------------------------------------------

add_executable(unreallib_example_load_package LoadPackage/main.cpp)
target_link_libraries(unreallib_example_load_package PRIVATE UnrealLib::UnrealLib)
set_target_properties(unreallib_example_load_package PROPERTIES
        OUTPUT_NAME UnrealLibExampleLoadPackage
        FOLDER "UnrealLib/Examples")
unreallib_set_compiler_options(unreallib_example_load_package)
