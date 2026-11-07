# ----------------------------------------------------------------------------------------------------------------------
# Documentation.cmake — Doxygen API reference (target: unreallib_docs). Enabled with -DUNREALLIB_BUILD_DOCS=ON.
# ----------------------------------------------------------------------------------------------------------------------

include_guard(GLOBAL)

find_package(Doxygen OPTIONAL_COMPONENTS dot)
if (NOT DOXYGEN_FOUND)
    message(WARNING "UnrealLib: UNREALLIB_BUILD_DOCS is ON but Doxygen was not found; 'unreallib_docs' is unavailable")
    return()
endif ()

set(DOXYGEN_PROJECT_NAME "UnrealLib")
set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
set(DOXYGEN_PROJECT_BRIEF "${PROJECT_DESCRIPTION}")
set(DOXYGEN_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/docs")
set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${PROJECT_SOURCE_DIR}/README.md")
set(DOXYGEN_EXTRACT_ALL NO)
set(DOXYGEN_EXTRACT_PRIVATE NO)
set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
set(DOXYGEN_WARN_NO_PARAMDOC NO)
set(DOXYGEN_RECURSIVE YES)
set(DOXYGEN_FILE_PATTERNS "*.hpp" "*.md")
set(DOXYGEN_EXCLUDE_SYMBOLS "detail" "UnrealLib::detail")
set(DOXYGEN_STRIP_FROM_INC_PATH "${PROJECT_SOURCE_DIR}/include" "${UNREALLIB_GENERATED_INCLUDE_DIR}")
set(DOXYGEN_STRIP_FROM_PATH "${PROJECT_SOURCE_DIR}")
set(DOXYGEN_GENERATE_TREEVIEW YES)
set(DOXYGEN_QUIET YES)

doxygen_add_docs(unreallib_docs
        "${PROJECT_SOURCE_DIR}/include"
        "${UNREALLIB_GENERATED_INCLUDE_DIR}"
        "${PROJECT_SOURCE_DIR}/README.md"
        "${PROJECT_SOURCE_DIR}/docs"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Generating UnrealLib API documentation with Doxygen")
