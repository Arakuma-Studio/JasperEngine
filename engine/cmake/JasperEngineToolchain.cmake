# The C++ standard and the compiler floors of every JasperEngine build: the editor, the
# standalone engine, exported games (desktop, Web, Android) and the HTML UI tests.
# JASPERENGINE_CXX_STANDARD is the only standard knob; the functions below make every C++
# target, vendored ones included, compile at it (one standard for the whole binary: libc++
# templates instantiated at two standards broke Wasm signatures).
include_guard(DIRECTORY)
# engine/CMakeLists.txt includes this before its cmake_minimum_required(), and the functions keep
# the policies of their definition (IN_LIST needs CMP0057); include() scopes this setting here
cmake_policy(VERSION 3.20)

# Oldest compilers with the portable C++20 subset (docs/specs/S01-cxx20.md, D7), named after
# CMAKE_CXX_COMPILER_ID. Clang covers clang-cl, the Android NDK and Emscripten. The editor gets
# them as compile definitions for the same check in the Play plugin's CMakeLists.txt
# (Generator::getToolchainFloorCheck).
set(JASPERENGINE_CXX_FLOOR_GNU 11)
set(JASPERENGINE_CXX_FLOOR_Clang 16)
set(JASPERENGINE_CXX_FLOOR_AppleClang 15)
set(JASPERENGINE_CXX_FLOOR_MSVC 19.30)

# Targets allowed off JASPERENGINE_CXX_STANDARD, each with its reason commented where it is
# added. Only a library whose public headers expose no std:: types and that is not linked into
# Web builds qualifies.
if(NOT DEFINED JASPERENGINE_CXX_STANDARD_EXCEPTIONS)
    set(JASPERENGINE_CXX_STANDARD_EXCEPTIONS "")
endif()

# Fails the configure below the floors. ID and VERSION default to the C++ compiler of the
# build (tools/tests/check_toolchain_floor.cmake passes them to test the floors).
function(jasperengine_check_cxx_toolchain)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "ID;VERSION" "")
    if(NOT DEFINED ARG_ID)
        set(ARG_ID "${CMAKE_CXX_COMPILER_ID}")
        set(ARG_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    set(floor "${JASPERENGINE_CXX_FLOOR_${ARG_ID}}")
    if(NOT floor)
        # other compilers are not supported, but not refused either
        return()
    endif()
    if(ARG_VERSION VERSION_LESS floor)
        message(FATAL_ERROR
            "JasperEngine requires C++20: the ${ARG_ID} compiler ${ARG_VERSION} is older than "
            "the supported minimum ${ARG_ID} ${floor}. Update it, or choose another compiler in "
            "Preferences > External Tools.")
    endif()
endfunction()

# Sets the C++ and Objective-C++ standard of the given targets (vendored libraries, after their
# add_subdirectory; their own CMakeLists.txt are not edited). Interface and imported targets
# compile nothing and are skipped.
function(jasperengine_apply_cxx_standard)
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            message(FATAL_ERROR "jasperengine_apply_cxx_standard: no target named '${target}'")
        endif()
        get_target_property(type ${target} TYPE)
        get_target_property(imported ${target} IMPORTED)
        get_target_property(aliased ${target} ALIASED_TARGET)
        if(type STREQUAL "INTERFACE_LIBRARY" OR imported OR aliased)
            continue()
        endif()
        set_target_properties(${target} PROPERTIES
            CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
            CXX_STANDARD_REQUIRED ON
            OBJCXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
            OBJCXX_STANDARD_REQUIRED ON
        )
    endforeach()
endfunction()

# Every target with C++ or Objective-C++ sources created in <dir> and its subdirectories
function(_jasperengine_collect_cxx_targets out_var dir)
    set(result "")
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(NOT type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
            continue()
        endif()
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS sources)
            if(source MATCHES "\\.(cpp|cc|cxx|mm)$")
                list(APPEND result ${target})
                break()
            endif()
        endforeach()
    endforeach()
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        _jasperengine_collect_cxx_targets(sub_targets "${subdir}")
        list(APPEND result ${sub_targets})
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# Applies the standard to every C++ target created in the given directories (a vendored
# library's whole tree, e.g. glslang's and spirv-cross's many targets under shadercompiler)
function(jasperengine_apply_cxx_standard_in_directories)
    foreach(dir IN LISTS ARGN)
        _jasperengine_collect_cxx_targets(targets "${dir}")
        jasperengine_apply_cxx_standard(${targets})
    endforeach()
endfunction()

# Run at the end of every top-level configure: fails if any C++ target of the build compiles at
# another standard than JASPERENGINE_CXX_STANDARD (or the compiler default), apart from
# JASPERENGINE_CXX_STANDARD_EXCEPTIONS.
function(jasperengine_check_cxx_standards)
    _jasperengine_collect_cxx_targets(targets "${CMAKE_SOURCE_DIR}")
    set(offending "")
    set(count 0)
    foreach(target IN LISTS targets)
        if(target IN_LIST JASPERENGINE_CXX_STANDARD_EXCEPTIONS)
            continue()
        endif()
        math(EXPR count "${count} + 1")
        get_target_property(standard ${target} CXX_STANDARD)
        if(NOT standard STREQUAL "${JASPERENGINE_CXX_STANDARD}")
            if(NOT standard)
                set(standard "compiler default")
            endif()
            list(APPEND offending "${target} (${standard})")
        endif()
    endforeach()
    if(offending)
        list(JOIN offending ", " offending_text)
        message(FATAL_ERROR
            "C++ standard check: these targets do not compile at C++${JASPERENGINE_CXX_STANDARD}: "
            "${offending_text}. Pass them to jasperengine_apply_cxx_standard() after their "
            "add_subdirectory().")
    endif()
    message(STATUS "C++ standard check: ${count} C++ targets at C++${JASPERENGINE_CXX_STANDARD}")
endfunction()

# Visual Studio generators: MSBuild's /m builds projects in parallel, the files of one project one
# at a time without cl's /MP. JASPERENGINE_PARALLEL_BUILD_JOBS (> 0: the editor's job cap) caps it;
# empty or 0 lets cl use every CPU. The editor and jasperengine.py then build one project at a time
# (cmake --build --parallel 1): /m:<n> with /MP<n> ran up to n x n compilers. Ninja and the other
# generators parallelise files themselves. Applied once per configure, to the directory it is
# called in and the ones added after (the editor build calls it before adding the engine).
function(jasperengine_msvc_parallel_compile)
    if(NOT MSVC OR NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
        return()
    endif()
    get_property(applied GLOBAL PROPERTY JASPERENGINE_MSVC_PARALLEL_COMPILE)
    if(applied)
        return()
    endif()
    set_property(GLOBAL PROPERTY JASPERENGINE_MSVC_PARALLEL_COMPILE ON)
    set(jobs "")
    if(JASPERENGINE_PARALLEL_BUILD_JOBS GREATER 0)
        set(jobs "${JASPERENGINE_PARALLEL_BUILD_JOBS}")
    endif()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/MP${jobs}>)
endfunction()
