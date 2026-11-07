# ----------------------------------------------------------------------------------------------------------------------
# unreallib_add_module(<Name>
#     SOURCES <files...>
#     [PUBLIC_DEPS <targets...>]
#     [PRIVATE_DEPS <targets...>])
#
# Creates the static library `unreallib_<name>` with the alias `UnrealLib::<Name>`.
# Sources are listed explicitly by the caller (no globbing).
# ----------------------------------------------------------------------------------------------------------------------

include_guard(GLOBAL)

function(unreallib_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    if (NOT ARG_SOURCES)
        message(FATAL_ERROR "unreallib_add_module(${name}): SOURCES is required")
    endif ()

    string(TOLOWER "${name}" lower)
    set(target "unreallib_${lower}")

    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(UnrealLib::${name} ALIAS ${target})

    target_include_directories(${target}
            PUBLIC
                "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
                "$<BUILD_INTERFACE:${UNREALLIB_GENERATED_INCLUDE_DIR}>"
            PRIVATE
                "${PROJECT_SOURCE_DIR}/src")

    if (ARG_PUBLIC_DEPS)
        target_link_libraries(${target} PUBLIC ${ARG_PUBLIC_DEPS})
    endif ()
    if (ARG_PRIVATE_DEPS)
        target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_DEPS})
    endif ()

    set_target_properties(${target} PROPERTIES
            OUTPUT_NAME "UnrealLib${name}"
            FOLDER "UnrealLib")
    unreallib_set_compiler_options(${target})

    set_property(GLOBAL APPEND PROPERTY UNREALLIB_MODULE_TARGETS ${target})
endfunction()
