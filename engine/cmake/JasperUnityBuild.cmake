# Unity builds of our own targets (docs/testing.md, "Unity build"): JASPERENGINE_UNITY_BUILD=ON
# merges the C/C++ sources of jasperengine, the editor executables, the project executable and the
# test targets 16 at a time. Vendored libraries are never unity-built.
#
# A source that cannot share a translation unit is fixed first (its file-local helpers go into a
# uniquely named namespace); otherwise it gets SKIP_UNITY_BUILD_INCLUSION next to a one-line
# reason. Objective-C/C++ files and generated sources are always compiled on their own.

include_guard(GLOBAL)

option(JASPERENGINE_UNITY_BUILD "Unity-build the engine, editor, project and test targets" OFF)

# jasperengine_target_unity_build(<target>...): a no-op unless JASPERENGINE_UNITY_BUILD is on
function(jasperengine_target_unity_build)
    if(NOT JASPERENGINE_UNITY_BUILD)
        return()
    endif()
    foreach(target IN LISTS ARGN)
        set_target_properties(${target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 16)
        # GCC warns about a type from an anonymous namespace used by a class in an included file,
        # meant for headers: in a unity unit every source is an included file (gtest fixtures)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-subobject-linkage>)
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS sources)
            if(source MATCHES "^\\$<")
                continue()
            endif()
            get_source_file_property(generated "${source}" GENERATED)
            if(generated OR source MATCHES "\\.(m|mm)$")
                set_source_files_properties("${source}" TARGET_DIRECTORY ${target} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
            endif()
        endforeach()
    endforeach()
endfunction()

# jasperengine_skip_unity_build(REASON <one line> SOURCES <file>...): these sources are compiled on
# their own in every target of the calling directory. The reason is required so the list stays
# justified; the property is inert when JASPERENGINE_UNITY_BUILD is off.
function(jasperengine_skip_unity_build)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "REASON" "SOURCES")
    if(NOT arg_REASON)
        message(FATAL_ERROR "jasperengine_skip_unity_build needs a REASON")
    endif()
    if(NOT arg_SOURCES)
        return()  # e.g. a platform without those sources
    endif()
    set_source_files_properties(${arg_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
endfunction()

# jasperengine_vendored_unity_build(<target>...): unity-builds vendored libraries, 16 sources a
# unit, in the trees that ship nothing (the editor tree, the tests, the development builds of
# tools/build.sh): docs/specs/S33-build-speed.md R4 and M9 (Jolt 248 -> 68 CPU-s, the same global
# symbols; RmlUi keeps too many same-named file-local helpers to merge). A shipped game
# (JASPERENGINE_VENDORED_UNITY OFF there) compiles them source by source as always, so its code
# generation does not change. Only libraries whose every source merges are listed.
function(jasperengine_vendored_unity_build)
    if(NOT JASPERENGINE_VENDORED_UNITY)
        return()
    endif()
    foreach(target IN LISTS ARGN)
        set_target_properties(${target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 16)
    endforeach()
endfunction()
