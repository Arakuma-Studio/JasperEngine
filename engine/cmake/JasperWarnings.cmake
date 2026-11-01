# Compiler warnings for our own targets (docs/testing.md, "Warnings"). Never global: vendored
# libraries keep EXTERNAL_WARNING_FLAG (-w, /w) and SYSTEM includes, and an exported game, whose
# top-level CMakeLists.txt is engine/CMakeLists.txt, configures with both options off.
#
#   JASPERENGINE_WARNINGS   the warning set below (ON in the editor tree, OFF in engine/)
#   JASPERENGINE_WERROR     and make them errors (needs JASPERENGINE_WARNINGS)
#
# The groups switched off here (sign-compare, MSVC's conversion warnings) are the ratchet of
# docs/testing.md: turning one on is a change of its own, with its cleanup.

include_guard(GLOBAL)

# The editor tree declares JASPERENGINE_WARNINGS ON before including this file; option() keeps that.
option(JASPERENGINE_WARNINGS "Warnings on the engine, editor and test targets" OFF)
option(JASPERENGINE_WERROR "Treat warnings of the engine, editor and test targets as errors (needs JASPERENGINE_WARNINGS)" OFF)

if(JASPERENGINE_WERROR AND NOT JASPERENGINE_WARNINGS)
    message(FATAL_ERROR "JASPERENGINE_WERROR needs JASPERENGINE_WARNINGS=ON")
endif()

if(MSVC)
    set(JASPERENGINE_WARNING_FLAGS /W3 /wd4244 /wd4267 /wd4305)
    set(JASPERENGINE_WERROR_FLAGS /WX)
else()
    set(JASPERENGINE_WARNING_FLAGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare)
    # deprecations stay warnings: they come from SDKs (macOS OpenGL, Vulkan) as often as from us
    set(JASPERENGINE_WERROR_FLAGS -Werror -Wno-error=deprecated-declarations)
endif()

# Apple's linker (ld-prime, Xcode 15+) warns "ignoring duplicate libraries" when a static
# archive is named twice on a link line. CMake repeats archives on purpose (link order of
# static libraries that depend on each other), so the warning says nothing about our code.
set(JASPERENGINE_WARNING_LINK_FLAGS "")
if(APPLE)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "-Wl,-no_warn_duplicate_libraries" JASPERENGINE_LD_NO_WARN_DUPLICATE_LIBRARIES)
    if(JASPERENGINE_LD_NO_WARN_DUPLICATE_LIBRARIES)
        set(JASPERENGINE_WARNING_LINK_FLAGS "-Wl,-no_warn_duplicate_libraries")
    endif()
endif()

if(JASPERENGINE_WERROR)
    message(STATUS "Warnings on our targets: on, as errors")
elseif(JASPERENGINE_WARNINGS)
    message(STATUS "Warnings on our targets: on")
endif()

# jasperengine_target_warnings(<target>...): the warning set on these targets' compiled sources
# (C, C++, Objective-C/C++: not the .rc resources or the assembler), and on the links of the
# executables and shared libraries among them JASPERENGINE_WARNING_LINK_FLAGS. A no-op unless
# JASPERENGINE_WARNINGS is on.
function(jasperengine_target_warnings)
    if(NOT JASPERENGINE_WARNINGS)
        return()
    endif()
    set(flags ${JASPERENGINE_WARNING_FLAGS})
    if(JASPERENGINE_WERROR)
        list(APPEND flags ${JASPERENGINE_WERROR_FLAGS})
    endif()
    foreach(target IN LISTS ARGN)
        foreach(flag IN LISTS flags)
            target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:${flag}>")
        endforeach()
        if(JASPERENGINE_WARNING_LINK_FLAGS)
            get_target_property(type ${target} TYPE)
            if(type STREQUAL "EXECUTABLE" OR type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "MODULE_LIBRARY")
                target_link_options(${target} PRIVATE ${JASPERENGINE_WARNING_LINK_FLAGS})
            endif()
        endif()
    endforeach()
endfunction()
