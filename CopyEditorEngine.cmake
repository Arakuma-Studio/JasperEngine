# Mirror the editor's engine SDK (engine/) into ENGINE_DEST without local build products.
# Gradle/CMake output under workspaces/androidstudio can be several gigabytes and is not SDK input.
#
#   cmake -DENGINE_SOURCE=<engine/> -DENGINE_DEST=<dir> [-DMANIFEST=<file>] [-DPRUNE_ONLY=ON] -P CopyEditorEngine.cmake
#
# A file deleted or renamed in engine/ leaves the copy too: an export or a C++ script build
# would otherwise compile a stale header (S20 deleted core/action/ParticleSystem.h; old copies
# still had it and the exported project failed to build).
#  - The build tree's copy (ENGINE_DEST next to the editor) is also CMake's engine binary
#    directory in an in-tree build, so it holds build products too. MANIFEST lists the files
#    the last copy made; those no longer in engine/ are deleted, nothing else. A copy without a
#    manifest (made before it existed) loses the source-like files that engine/ no longer has,
#    outside CMakeFiles/ and generated/.
#  - PRUNE_ONLY=ON (the install step, before install(DIRECTORY)): ENGINE_DEST is a pure copy,
#    so every file that engine/ does not have (after the exclusions below) is deleted; nothing
#    is copied.
# Exclusions (both modes, and the install rule in CMakeLists.txt): .cxx, .gradle, build and .git
# anywhere, engine/tests (test sources, fixtures, golden images; not the tests directories of
# vendored libraries), engine/CMakePresets.json (a project configures without them), and with
# INSTALL_EXCLUDES=ON RmlUi's own test suites (engine/libs/rmlui/Tests).
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENGINE_SOURCE OR NOT DEFINED ENGINE_DEST)
    message(FATAL_ERROR "ENGINE_SOURCE and ENGINE_DEST are required")
endif()

# Whether engine/ has this file (relative path) and the copy keeps it
function(engine_keeps rel out)
    set(${out} FALSE PARENT_SCOPE)
    if(rel MATCHES "(^|/)(\\.cxx|\\.gradle|build|\\.git)(/|$)"
       OR rel MATCHES "^tests/" OR rel STREQUAL "CMakePresets.json"
       OR (INSTALL_EXCLUDES AND rel MATCHES "^libs/rmlui/Tests/"))
        return()
    endif()
    if(EXISTS "${ENGINE_SOURCE}/${rel}" AND NOT IS_DIRECTORY "${ENGINE_SOURCE}/${rel}")
        set(${out} TRUE PARENT_SCOPE)
    endif()
endfunction()

# The listed files (relative) that engine/ no longer has
function(not_in_engine files out)
    set(gone)
    foreach(rel IN LISTS files)
        engine_keeps("${rel}" kept)
        if(NOT kept)
            list(APPEND gone "${rel}")
        endif()
    endforeach()
    set(${out} "${gone}" PARENT_SCOPE)
endfunction()

# Deletes each listed file (relative to ENGINE_DEST) and the directories that empties
function(remove_stale files)
    foreach(rel IN LISTS files)
        file(REMOVE "${ENGINE_DEST}/${rel}")
        message(STATUS "Removed from the engine copy (gone from engine/): ${rel}")
        get_filename_component(dir "${ENGINE_DEST}/${rel}" DIRECTORY)
        while(NOT dir STREQUAL ENGINE_DEST AND IS_DIRECTORY "${dir}")
            file(GLOB left "${dir}/*" "${dir}/.*")
            if(left)
                break()
            endif()
            file(REMOVE_RECURSE "${dir}")   # empty: checked above
            get_filename_component(dir "${dir}" DIRECTORY)
        endwhile()
    endforeach()
endfunction()

if(PRUNE_ONLY)
    if(NOT IS_DIRECTORY "${ENGINE_DEST}")
        return()
    endif()
    file(GLOB_RECURSE dest_files RELATIVE "${ENGINE_DEST}" LIST_DIRECTORIES false "${ENGINE_DEST}/*")
    not_in_engine("${dest_files}" stale)
    remove_stale("${stale}")
    return()
endif()

# In an in-tree editor build this directory is also CMake's engine binary
# directory, so keep its generated build files intact.
file(MAKE_DIRECTORY "${ENGINE_DEST}")
# Older copies may already contain Android build output. Remove only these
# generated subtrees; the engine CMake binary directory must remain intact.
file(REMOVE_RECURSE
     "${ENGINE_DEST}/workspaces/androidstudio/build"
     "${ENGINE_DEST}/workspaces/androidstudio/.gradle"
     "${ENGINE_DEST}/workspaces/androidstudio/app/build"
     "${ENGINE_DEST}/workspaces/androidstudio/app/.cxx")

# What the last copy made and engine/ no longer has
set(stale)
if(DEFINED MANIFEST AND EXISTS "${MANIFEST}")
    file(STRINGS "${MANIFEST}" stale)
else()
    # no manifest yet: the source-like files outside the build products
    file(GLOB_RECURSE dest_files RELATIVE "${ENGINE_DEST}" LIST_DIRECTORIES false "${ENGINE_DEST}/*")
    foreach(rel IN LISTS dest_files)
        if(rel MATCHES "(^|/)CMakeFiles/" OR rel MATCHES "^generated/")
            continue()
        endif()
        if(rel MATCHES "\\.(h|hh|hpp|hxx|inl|ipp|c|cc|cpp|cxx|m|mm|lua|glsl|vert|frag|comp|hlsl|metal|wgsl)$")
            list(APPEND stale "${rel}")
        endif()
    endforeach()
endif()
not_in_engine("${stale}" stale)
remove_stale("${stale}")

# IDE output of the workspaces (Android Studio, Gradle, Xcode, Finder) is never part of the SDK:
# exports would copy it into every game (export/ExportFiles.h isIdeOutput keeps the same names)
file(COPY "${ENGINE_SOURCE}/" DESTINATION "${ENGINE_DEST}"
     PATTERN ".cxx" EXCLUDE
     PATTERN ".gradle" EXCLUDE
     PATTERN ".idea" EXCLUDE
     PATTERN ".kotlin" EXCLUDE
     PATTERN ".externalNativeBuild" EXCLUDE
     PATTERN "captures" EXCLUDE
     PATTERN "local.properties" EXCLUDE
     PATTERN "xcuserdata" EXCLUDE
     PATTERN "DerivedData" EXCLUDE
     PATTERN ".DS_Store" EXCLUDE
     REGEX "\\.iml$" EXCLUDE
     PATTERN "build" EXCLUDE
     PATTERN ".git" EXCLUDE
     REGEX "/engine/tests$" EXCLUDE
     REGEX "/engine/CMakePresets.json$" EXCLUDE)
# a copy made before the exclusion still has one
file(REMOVE "${ENGINE_DEST}/CMakePresets.json")

# what this copy made: the engine's files that it keeps
if(DEFINED MANIFEST)
    file(GLOB_RECURSE source_files RELATIVE "${ENGINE_SOURCE}" LIST_DIRECTORIES false "${ENGINE_SOURCE}/*")
    set(listing "")
    foreach(rel IN LISTS source_files)
        engine_keeps("${rel}" kept)
        if(kept)
            string(APPEND listing "${rel}\n")
        endif()
    endforeach()
    file(WRITE "${MANIFEST}" "${listing}")
endif()
