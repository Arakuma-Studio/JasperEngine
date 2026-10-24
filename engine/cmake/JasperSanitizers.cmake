# Sanitizer builds (docs/testing.md, "Sanitizers"). Included right after project() by the editor
# tree (CMakeLists.txt) and the standalone engine tree (engine/CMakeLists.txt), before any target:
# the flags reach every target of the tree, vendored libraries included, because code built
# without instrumentation gives ThreadSanitizer false positives and hides AddressSanitizer bugs.
#
#   -DJASPERENGINE_SANITIZE="address;undefined"   ASan + UBSan (also "address,undefined")
#   -DJASPERENGINE_SANITIZE=thread                 TSan
#
# Empty (the default, and what an exported game always gets) adds nothing.
#
# Portions adapted from OpenEngine (CMakeLists.txt: the MSVC AddressSanitizer fixes, the quoted
# generator expressions, the runtime-DLL staging), MIT:
# Copyright (c) 2026 Gamecrafters Guild Limited and contributors

include_guard(GLOBAL)

set(JASPERENGINE_SANITIZE "" CACHE STRING "Sanitizers for every target of this tree: address;undefined, thread, or empty")

set(jasperengineSanitize "${JASPERENGINE_SANITIZE}")
string(REPLACE "," ";" jasperengineSanitize "${jasperengineSanitize}")
list(TRANSFORM jasperengineSanitize STRIP)
list(REMOVE_ITEM jasperengineSanitize "")
list(REMOVE_DUPLICATES jasperengineSanitize)

foreach(sanitizer IN LISTS jasperengineSanitize)
    if(NOT sanitizer MATCHES "^(address|undefined|thread)$")
        message(FATAL_ERROR "JASPERENGINE_SANITIZE: unknown sanitizer '${sanitizer}' (expected address, undefined or thread)")
    endif()
endforeach()

if(jasperengineSanitize)
    if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "JASPERENGINE_SANITIZE is for desktop builds (Linux, Windows, macOS), not ${CMAKE_SYSTEM_NAME}: configure it empty")
    endif()
    if("thread" IN_LIST jasperengineSanitize AND "address" IN_LIST jasperengineSanitize)
        message(FATAL_ERROR "JASPERENGINE_SANITIZE: thread and address cannot be combined, their runtimes do not link together. Configure a separate build tree for each (tools/build.sh unit-tests with SANITIZE=address,undefined, then SANITIZE=thread).")
    endif()
    if(MSVC AND ("thread" IN_LIST jasperengineSanitize OR "undefined" IN_LIST jasperengineSanitize))
        message(FATAL_ERROR "JASPERENGINE_SANITIZE: MSVC has AddressSanitizer only (address); thread and undefined need GCC or Clang (Linux or macOS)")
    endif()
endif()

foreach(sanitizer IN ITEMS address undefined thread)
    string(TOUPPER "${sanitizer}" sanitizerUpper)
    if(sanitizer IN_LIST jasperengineSanitize)
        set(JASPERENGINE_SANITIZE_${sanitizerUpper} ON)
        # lets tests relax time budgets under a sanitizer
        add_compile_definitions(JASPERENGINE_SANITIZE_${sanitizerUpper}=1)
    else()
        set(JASPERENGINE_SANITIZE_${sanitizerUpper} OFF)
    endif()
endforeach()

if(jasperengineSanitize)
    string(REPLACE ";" "," jasperengineSanitizeFlag "${jasperengineSanitize}")
    message(STATUS "Sanitizers: ${jasperengineSanitizeFlag}")
    if(MSVC)
        # MSVC ASan needs the DLL runtime, conflicts with the /RTC run-time checks of Debug, and
        # wants a full link. The STL container annotations would make every library built
        # without ASan (the Windows SDK's, prebuilt ones) an ODR mismatch at link time.
        # (normal variables of the top directory: they reach every subdirectory, and turning the
        # option off again leaves no trace in the cache; engine/CMakeLists.txt keeps the DLL runtime)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        foreach(flagVar CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
            string(REPLACE "/RTC1" "" ${flagVar} "${${flagVar}}")
        endforeach()
        add_compile_options(/fsanitize=address)
        add_compile_definitions(_DISABLE_STL_ANNOTATION=1)
        add_link_options(/INCREMENTAL:NO)
    else()
        # One list item per flag: a space inside one item would reach the compiler as one argument
        set(jasperengineSanitizeOptions "-fsanitize=${jasperengineSanitizeFlag}" -fno-omit-frame-pointer)
        if(JASPERENGINE_SANITIZE_UNDEFINED)
            # stop at the first report, like ASan. vptr needs RTTI of every module (the editor
            # loads plugins), and Clang's function check trips on C callbacks of vendored code
            list(APPEND jasperengineSanitizeOptions -fno-sanitize-recover=undefined -fno-sanitize=vptr)
            if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                list(APPEND jasperengineSanitizeOptions -fno-sanitize=function)
            endif()
        endif()
        add_compile_options(${jasperengineSanitizeOptions})
        add_link_options("-fsanitize=${jasperengineSanitizeFlag}")
    endif()
endif()

# MSVC: the AddressSanitizer runtime DLL lives next to cl.exe, which a plain shell or CTest does
# not have on PATH; staging it next to each executable lets them start from anywhere.
set(JASPERENGINE_ASAN_RUNTIME_DLLS "")
if(MSVC AND JASPERENGINE_SANITIZE_ADDRESS)
    get_filename_component(jasperengineMsvcBin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(dll IN ITEMS clang_rt.asan_dynamic-x86_64.dll clang_rt.asan_dbg_dynamic-x86_64.dll)
        if(EXISTS "${jasperengineMsvcBin}/${dll}")
            list(APPEND JASPERENGINE_ASAN_RUNTIME_DLLS "${jasperengineMsvcBin}/${dll}")
        endif()
    endforeach()
    if(NOT JASPERENGINE_ASAN_RUNTIME_DLLS)
        message(WARNING "JASPERENGINE_SANITIZE=address: no clang_rt.asan_dynamic-x86_64.dll next to ${CMAKE_CXX_COMPILER}; the executables only start from a shell that has the MSVC tools on PATH")
    endif()
endif()

# Copies the MSVC ASan runtime next to an executable; nothing elsewhere.
function(jasperengine_stage_asan_runtime target)
    foreach(dll IN LISTS JASPERENGINE_ASAN_RUNTIME_DLLS)
        get_filename_component(dllName "${dll}" NAME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:${target}>/${dllName}"
            COMMENT "Copying the AddressSanitizer runtime ${dllName} next to ${target}"
            VERBATIM
        )
    endforeach()
endfunction()
