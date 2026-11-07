# ----------------------------------------------------------------------------------------------------------------------
# CompilerOptions.cmake — warning / conformance flags applied to UnrealLib's own targets only (never to dependencies).
# ----------------------------------------------------------------------------------------------------------------------

include_guard(GLOBAL)

# AddressSanitizer with the MSVC STL: container annotations need stl_asan.lib (part of the optional VS ASan component)
# and must be identical in every object file (/failifmismatch:annotate_*), including dependencies that are built
# without ASan. Disabling them for all targets keeps ASan's heap/stack/global checks while making mixed builds link.
# Directory scope on purpose: this file is included before Dependencies.cmake, so CPM packages see it too.
if (UNREALLIB_ENABLE_ASAN AND MSVC)
    add_compile_definitions(_DISABLE_STL_ANNOTATION)
endif ()

# Directory with clang's Windows runtime libraries (clang_rt.*). Depending on how LLVM was built this is
# <resource-dir>/lib/<target-triple> (what -print-runtime-dir reports) or <resource-dir>/lib/windows.
function(unreallib_clang_runtime_dir out_var)
    if (NOT DEFINED CACHE{UNREALLIB_CLANG_RUNTIME_DIR})
        execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
                OUTPUT_VARIABLE _resource
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
        execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-runtime-dir
                OUTPUT_VARIABLE _runtime
                OUTPUT_STRIP_TRAILING_WHITESPACE)
        file(TO_CMAKE_PATH "${_resource}" _resource)
        file(TO_CMAKE_PATH "${_runtime}" _runtime)
        set(_found "")
        foreach (_candidate IN ITEMS "${_runtime}" "${_resource}/lib/windows")
            file(GLOB _asan "${_candidate}/clang_rt.asan_dynamic*")
            if (_asan)
                set(_found "${_candidate}")
                break()
            endif ()
        endforeach ()
        if (NOT _found)
            message(FATAL_ERROR "UnrealLib: clang's AddressSanitizer runtime was not found under '${_resource}'")
        endif ()
        set(UNREALLIB_CLANG_RUNTIME_DIR "${_found}" CACHE INTERNAL "clang runtime library directory")
    endif ()
    set(${out_var} "${UNREALLIB_CLANG_RUNTIME_DIR}" PARENT_SCOPE)
endfunction()

function(unreallib_set_compiler_options target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if (MSVC)
        # MSVC and clang-cl
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /EHsc /utf-8)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /Zc:preprocessor)
        endif ()
        if (UNREALLIB_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif ()
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else ()
        target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
                -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
        if (UNREALLIB_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif ()
    endif ()

    if (UNREALLIB_ENABLE_ASAN)
        if (MSVC)
            # MSVC needs the optional "C++ AddressSanitizer" Visual Studio component.
            target_compile_options(${target} PRIVATE /fsanitize=address)
            if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
                # clang-cl: CMake links with the linker directly, so add LLVM's runtime directory and ship the
                # runtime DLL next to executables (tests are run, and discovered, from the build tree).
                unreallib_clang_runtime_dir(_runtime_dir)
                target_link_options(${target} PRIVATE "/LIBPATH:${_runtime_dir}")
                get_target_property(_type ${target} TYPE)
                if (_type STREQUAL "EXECUTABLE")
                    # What clang's driver would add when linking with -fsanitize=address and the dynamic CRT.
                    target_link_options(${target} PRIVATE
                            "clang_rt.asan_dynamic-x86_64.lib"
                            "/WHOLEARCHIVE:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
                    file(GLOB _asan_dll "${_runtime_dir}/clang_rt.asan_dynamic-*.dll")
                    if (_asan_dll)
                        add_custom_command(TARGET ${target} POST_BUILD
                                COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_asan_dll} "$<TARGET_FILE_DIR:${target}>"
                                VERBATIM)
                    endif ()
                endif ()
            endif ()
        else ()
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif ()
    endif ()
endfunction()
