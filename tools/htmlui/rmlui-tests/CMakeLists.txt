# Standalone build of RmlUi's own test suites against the engine's fork (phase-3 spec 5.1).
#
# Not part of the engine build: tools/htmlui/run-rmlui-tests.sh assembles a source tree in
# build/rmlui-tests/src (this file as its CMakeLists.txt; Include, Source/Core, Source/Debugger and
# Tests from engine/libs/rmlui; from a pinned upstream RmlUi 6.3 checkout what the engine does not
# vendor: the FreeType font engine Source/Core/FontEngineDefault, CMake/, Samples/shell,
# Samples/assets, Backends/RmlUi_Backend.h) and builds it here. The engine library itself has no
# FreeType: only this test build compiles RmlUi's default font engine, which the tests measure
# text with (Shell::LoadFonts loads the LatoLatin sample fonts).
#
# The upstream library CMake lists every source file by name; this one globs the fork's sources,
# so files added by fork patches build without a list to update. The tests' own CMake files
# (Tests/**/CMakeLists.txt) are upstream's, vendored unchanged.
cmake_minimum_required(VERSION 3.16)
project(RmlUiForkTests VERSION 6.3 LANGUAGES C CXX)

include("${PROJECT_SOURCE_DIR}/CMake/Utilities.cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# upstream's defaults for set_common_target_options
set(RMLUI_COMPILER_OPTIONS ON)
set(RMLUI_WARNINGS_AS_ERRORS OFF)
set(RMLUI_RTTI_AND_EXCEPTIONS ON)

find_package(Freetype REQUIRED)

# --- rmlui_core: the fork's Core plus upstream's FreeType font engine ------------------------
file(GLOB RMLUI_CORE_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/Source/Core/*.cpp"
    "${PROJECT_SOURCE_DIR}/Source/Core/Elements/*.cpp"
    "${PROJECT_SOURCE_DIR}/Source/Core/Layout/*.cpp"
    "${PROJECT_SOURCE_DIR}/Source/Core/FontEngineDefault/*.cpp"
)
add_library(rmlui_core STATIC ${RMLUI_CORE_SOURCES})
set_common_target_options(rmlui_core)
target_include_directories(rmlui_core PUBLIC "${PROJECT_SOURCE_DIR}/Include")
target_compile_definitions(rmlui_core
    PUBLIC RMLUI_STATIC_LIB
    # calc() (upstream PR #983, FORK.md P3-1): the engine defines it PUBLIC too (engine/libs/rmlui/CMakeLists.txt)
    RMLUI_MATH_EXPRESSIONS
    PRIVATE RMLUI_VERSION="6.3" RMLUI_FONT_ENGINE_FREETYPE
)
target_link_libraries(rmlui_core PRIVATE Freetype::Freetype)

# --- rmlui_debugger -------------------------------------------------------------------------
file(GLOB RMLUI_DEBUGGER_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/Source/Debugger/*.cpp")
add_library(rmlui_debugger STATIC ${RMLUI_DEBUGGER_SOURCES})
set_common_target_options(rmlui_debugger)
target_link_libraries(rmlui_debugger PUBLIC rmlui_core)

# --- rmlui_shell: upstream's sample shell on a window-less backend ---------------------------
# The tests draw with their own dummy render interface (TestsShell) unless RMLUI_TESTS_USE_SHELL=1
# asks for a window, which the null backend refuses: no window system, GL or GLFW is needed.
add_library(rmlui_shell STATIC
    "${PROJECT_SOURCE_DIR}/Samples/shell/src/PlatformExtensions.cpp"
    "${PROJECT_SOURCE_DIR}/Samples/shell/src/RendererExtensions.cpp"
    "${PROJECT_SOURCE_DIR}/Samples/shell/src/Shell.cpp"
    "${PROJECT_SOURCE_DIR}/Samples/shell/src/ShellFileInterface.cpp"
    "${PROJECT_SOURCE_DIR}/Backends/RmlUi_Backend_Null.cpp"
)
set_common_target_options(rmlui_shell)
target_include_directories(rmlui_shell PUBLIC "${PROJECT_SOURCE_DIR}/Samples/shell/include" "${PROJECT_SOURCE_DIR}/Backends")
target_link_libraries(rmlui_shell PUBLIC rmlui_core rmlui_debugger)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # PlatformExtensions.cpp includes <X11/Xlib.h> on Unix (for nothing the tests use)
    find_package(X11 REQUIRED)
    target_include_directories(rmlui_shell PRIVATE ${X11_INCLUDE_DIR})
endif()

# --- the tests: upstream's CMake files ------------------------------------------------------
enable_testing()
add_subdirectory(Tests)
