# The linker of development and test builds (docs/testing.md "Build speed",
# docs/specs/S33-build-speed.md R3). With debug information GNU ld spends 20-53 s on each of the
# editor, its tests, the shared engine and the core tests; lld links them in a fraction of that.
#
# JASPERENGINE_LINKER: AUTO (default), DEFAULT, LLD, MOLD or GOLD, applied as CMAKE_LINKER_TYPE.
# AUTO uses lld when it is installed, for Debug and RelWithDebInfo builds that are not a shipped
# game (jasperengineShippedGame, engine/CMakeLists.txt); every Release and MinSizeRel build and
# every shipped game links with the toolchain's default linker, exactly as before. Emscripten,
# Android (the NDK links with lld already), Apple and MSVC toolchains always keep theirs.
# Include it before the first target; JASPERENGINE_LINKER_SHIPPED is the caller's verdict.

include_guard(DIRECTORY)

set(JASPERENGINE_LINKER AUTO CACHE STRING "Linker of our builds: AUTO, DEFAULT, LLD, MOLD or GOLD")
set_property(CACHE JASPERENGINE_LINKER PROPERTY STRINGS AUTO DEFAULT LLD MOLD GOLD)

function(_jasperengine_choose_linker shipped)
    string(TOUPPER "${JASPERENGINE_LINKER}" choice)
    if(NOT choice MATCHES "^(AUTO|DEFAULT|LLD|MOLD|GOLD)$")
        message(FATAL_ERROR "JASPERENGINE_LINKER must be AUTO, DEFAULT, LLD, MOLD or GOLD (got ${JASPERENGINE_LINKER})")
    endif()
    if(choice STREQUAL "DEFAULT")
        return()
    endif()
    set(selectable ON)
    if(EMSCRIPTEN OR ANDROID OR APPLE OR MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"
       OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$" OR CMAKE_VERSION VERSION_LESS 3.29)
        set(selectable OFF)
    endif()
    if(choice STREQUAL "AUTO")
        if(NOT selectable OR shipped OR NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
            return()
        endif()
        find_program(JASPERENGINE_LLD_EXECUTABLE NAMES ld.lld)
        if(NOT JASPERENGINE_LLD_EXECUTABLE)
            message(STATUS "Linker: the toolchain's default (no ld.lld for JASPERENGINE_LINKER=AUTO)")
            return()
        endif()
        set(choice LLD)
    elseif(NOT selectable)
        message(FATAL_ERROR "JASPERENGINE_LINKER=${choice} needs GCC or Clang on Linux or Windows (MinGW) with CMake 3.29 or newer; "
            "use AUTO or DEFAULT for this toolchain")
    endif()
    if(choice STREQUAL "MOLD" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.1)
        message(FATAL_ERROR "JASPERENGINE_LINKER=MOLD: GCC ${CMAKE_CXX_COMPILER_VERSION} cannot select mold (-fuse-ld=mold needs GCC 12.1); use LLD")
    endif()
    set(program_LLD ld.lld)
    set(program_MOLD mold)
    set(program_GOLD ld.gold)
    find_program(jasperengineLinkerProgram NAMES ${program_${choice}} NO_CACHE)
    if(NOT jasperengineLinkerProgram)
        message(FATAL_ERROR "JASPERENGINE_LINKER=${choice}: ${program_${choice}} is not installed (docker/linux.Dockerfile has lld)")
    endif()
    set(CMAKE_LINKER_TYPE ${choice} PARENT_SCOPE)
    message(STATUS "Linker: ${choice} (${jasperengineLinkerProgram})")
endfunction()

# jasperengine_select_linker(<shipped>): sets CMAKE_LINKER_TYPE for the targets created after it
macro(jasperengine_select_linker shipped)
    _jasperengine_choose_linker(${shipped})
endmacro()

# jasperengine_link_whole_archive(<target> <static library>...): every member of each library is
# linked into <target>, not only those its code references (the editor's shared engine carries all of
# Box2D and Jolt for C++ scripts). The libraries stay in <target>'s link libraries for ordering.
function(jasperengine_link_whole_archive target)
    foreach(lib IN LISTS ARGN)
        # MSVC-style drivers (cl, clang-cl) take link.exe's option as is; a GNU-style clang++
        # targeting MSVC passes it to its link.exe-style linker
        if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            target_link_options(${target} PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:${lib}>")
        elseif(CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
            target_link_options(${target} PRIVATE "LINKER:/WHOLEARCHIVE:$<TARGET_FILE:${lib}>")
        elseif(APPLE)
            target_link_options(${target} PRIVATE "LINKER:-force_load,$<TARGET_FILE:${lib}>")
        else()
            # one SHELL: item per library: CMake de-duplicates repeated options, which would drop
            # the second library's --whole-archive
            target_link_options(${target} PRIVATE "SHELL:-Wl,--whole-archive \"$<TARGET_FILE:${lib}>\" -Wl,--no-whole-archive")
        endif()
        add_dependencies(${target} ${lib})
    endforeach()
endfunction()
