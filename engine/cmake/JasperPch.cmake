# Precompiled headers of our own C++ targets (docs/testing.md "Build speed",
# docs/specs/S33-build-speed.md R2): engine/cmake/JasperPch.h (standard headers) for the engine,
# test and project targets, editor/EditorPch.h (those plus json.hpp) for the editor's.
#
# JASPERENGINE_PCH is ON by default in the editor tree and OFF when engine/ is the top level (every
# exported game, web, Android and player build configures as it always did; tools/build.sh turns
# it on for its Linux development and test trees). GCC and Clang with Ninja or Makefiles only: MSVC
# builds have never used them, and the Xcode generator is not used for our development builds.
#
# Only our own C++ sources get it: C, Objective-C(++) and the vendored sources compiled inside our
# targets (ImGui, the platform backends under libs/) are compiled as before.

include_guard(GLOBAL)

get_filename_component(jasperenginePchEngineDir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL jasperenginePchEngineDir)
    set(jasperenginePchDefault OFF)
else()
    set(jasperenginePchDefault ON)
endif()
option(JASPERENGINE_PCH "Precompiled headers for the engine, editor and test targets (GCC and Clang)" ${jasperenginePchDefault})

set(JASPERENGINE_PCH_ACTIVE OFF)
if(JASPERENGINE_PCH)
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(STATUS "Precompiled headers: off (JASPERENGINE_PCH is for GCC and Clang, not ${CMAKE_CXX_COMPILER_ID})")
    elseif(CMAKE_GENERATOR STREQUAL "Xcode")
        message(STATUS "Precompiled headers: off (not with the Xcode generator)")
    else()
        set(JASPERENGINE_PCH_ACTIVE ON)
        message(STATUS "Precompiled headers: on")
    endif()
endif()

# the vendored trees: engine/libs and the editor's libs
set(JASPERENGINE_PCH_ENGINE_LIBS "${jasperenginePchEngineDir}/libs")
set(JASPERENGINE_PCH_ROOT_LIBS "${CMAKE_SOURCE_DIR}/libs")
set(JASPERENGINE_PCH_HEADER "${CMAKE_CURRENT_LIST_DIR}/JasperPch.h")
set(JASPERENGINE_EDITOR_PCH_HEADER "${CMAKE_CURRENT_LIST_DIR}/../../editor/EditorPch.h")

# jasperengine_target_pch(<target> [EDITOR]): a no-op unless JASPERENGINE_PCH is active
function(jasperengine_target_pch target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "EDITOR" "" "")
    if(NOT JASPERENGINE_PCH_ACTIVE)
        return()
    endif()
    if(arg_EDITOR)
        set(header "${JASPERENGINE_EDITOR_PCH_HEADER}")
    else()
        set(header "${JASPERENGINE_PCH_HEADER}")
    endif()
    cmake_path(NORMAL_PATH header)
    target_precompile_headers(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${header}>")
    # vendored C++ compiled inside the target keeps its own command line; and a source with
    # options of its own (-mavx2, -ffp-contract=off, -Os, ...) would not match the header's (GCC:
    # "created and used with differing settings"): checked once every directory is read, since
    # those properties are often set after the target
    # (a deferred call's arguments are expanded when it runs: the target's name goes in now)
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL _jasperengine_pch_skip_sources [[${target}]])")
endfunction()

function(_jasperengine_pch_skip_sources target)
    get_target_property(sources ${target} SOURCES)
    # (relative sources are the target's directory's; this runs in the top-level one)
    get_target_property(sourceDir ${target} SOURCE_DIR)
    foreach(source IN LISTS sources)
        if(source MATCHES "^\\$<")
            continue()
        endif()
        if(NOT IS_ABSOLUTE "${source}")
            set(source "${sourceDir}/${source}")
        endif()
        set(skip OFF)
        cmake_path(IS_PREFIX JASPERENGINE_PCH_ENGINE_LIBS "${source}" NORMALIZE inEngineLibs)
        cmake_path(IS_PREFIX JASPERENGINE_PCH_ROOT_LIBS "${source}" NORMALIZE inRootLibs)
        # Objective-C(++): where OBJCXX is not enabled (engine/ as the top level) CMake builds a .mm
        # as CXX, and clang refuses the C++ header there ("Objective-C was disabled in PCH file but
        # is currently enabled", BF4 on macOS)
        if(source MATCHES "\\.(m|mm)$")
            set(skip ON)
        elseif(inEngineLibs OR inRootLibs)
            set(skip ON)
        else()
            foreach(property COMPILE_OPTIONS COMPILE_FLAGS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES)
                get_source_file_property(value "${source}" TARGET_DIRECTORY ${target} ${property})
                if(value)
                    set(skip ON)
                endif()
            endforeach()
        endif()
        if(skip)
            set_source_files_properties("${source}" TARGET_DIRECTORY ${target} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endforeach()
endfunction()
