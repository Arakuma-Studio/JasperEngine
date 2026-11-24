# How much debug information the objects of a tree carry (docs/testing.md, "Build speed"). Included
# right after project() by the editor tree (CMakeLists.txt) and the standalone engine tree
# (engine/CMakeLists.txt), before any target.
#
#   -DJASPERENGINE_DEBUG_INFO=full    what the build type and a shipped game's split symbols ask for (the default)
#   -DJASPERENGINE_DEBUG_INFO=lines   line tables only: function names and file:line, no types or locals
#
# lines is CI's (ci.yml sets JASPERENGINE_DEBUG_INFO, which tools/build.sh and tools/smoke-test.sh pass
# on): backtraces and sanitizer reports still name each frame's function, file and line, and the
# objects, so the lanes' ccache entries, are a fraction of the size. It turns every -g a build
# would get into GCC's -g1 or Clang's -gline-tables-only (Clang's -g1; Emscripten's -g1 drops the
# DWARF); a build without debug information gets none. MSVC has no such level: /Z7 stays.
#
# A first configure takes the default from the environment (an exported game the editor builds
# inherits it); an explicit -D (tools/build.sh passes one to every tree) wins.

include_guard(GLOBAL)

set(jasperengineDebugInfoDefault full)
if(NOT "$ENV{JASPERENGINE_DEBUG_INFO}" STREQUAL "")
    set(jasperengineDebugInfoDefault "$ENV{JASPERENGINE_DEBUG_INFO}")
endif()
set(JASPERENGINE_DEBUG_INFO "${jasperengineDebugInfoDefault}" CACHE STRING "Debug information of the objects: full or lines (line tables only)")
set_property(CACHE JASPERENGINE_DEBUG_INFO PROPERTY STRINGS full lines)
if(NOT JASPERENGINE_DEBUG_INFO MATCHES "^(full|lines)$")
    message(FATAL_ERROR "JASPERENGINE_DEBUG_INFO is full or lines, got '${JASPERENGINE_DEBUG_INFO}'")
endif()

# what a shipped game's split symbols compile with (engine/CMakeLists.txt, JASPERENGINE_SPLIT_SYMBOLS)
set(JASPERENGINE_DEBUG_FLAG -g)
if(JASPERENGINE_DEBUG_INFO STREQUAL "lines")
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        message(STATUS "Debug information: full (MSVC has no line-tables-only level)")
    else()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(JASPERENGINE_DEBUG_FLAG -g1)
        else()
            set(JASPERENGINE_DEBUG_FLAG -gline-tables-only)
        endif()
        # after the build type's -g on the command line (CMAKE_<LANG>_FLAGS_<CONFIG> come first),
        # and the last -g level wins
        add_compile_options("$<$<AND:$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>,$<CONFIG:Debug,RelWithDebInfo>>:${JASPERENGINE_DEBUG_FLAG}>")
        message(STATUS "Debug information: line tables only (${JASPERENGINE_DEBUG_FLAG})")
    endif()
endif()
