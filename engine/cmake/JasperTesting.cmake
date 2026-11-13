# googletest targets (docs/testing.md). Included by the standalone engine tree when it builds a
# test target (JASPERENGINE_TESTS: jasperengine-core-tests, JASPERENGINE_HTMLUI_TESTS) and by the
# editor tree (JASPERENGINE_TESTS: jasperengine-editor-tests, JASPERENGINE_HTMLUI_EDITOR_TESTS).
# googletest lives in the repository (libs/googletest), not in the engine SDK: an editor install
# or an exported game has neither the tests nor this framework.

include_guard(GLOBAL)

include(GoogleTest)
include(${CMAKE_CURRENT_LIST_DIR}/JasperWarnings.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/JasperSanitizers.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/JasperUnityBuild.cmake)
# the UTF-8 code page and DPI awareness of the editor and games (engine/platform/win/jasperengine.manifest):
# tests run in the code page users run in
if(WIN32)
    include(${CMAKE_CURRENT_LIST_DIR}/../platform/win/WindowsManifest.cmake)
endif()

set(JASPERENGINE_GOOGLETEST_DIR "${CMAKE_CURRENT_LIST_DIR}/../../libs/googletest")
set(JASPERENGINE_TEST_SUPPORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../tests/support")

# ctest from the top of the build tree
enable_testing()

# googletest and googlemock, once per tree, outside the default build and without warnings
function(jasperengine_add_googletest)
    if(TARGET gtest)
        return()
    endif()
    if(NOT EXISTS "${JASPERENGINE_GOOGLETEST_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "googletest is not at ${JASPERENGINE_GOOGLETEST_DIR}: the unit tests build from the repository, not from an editor install or an exported project")
    endif()
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    # the CRT comes from CMAKE_MSVC_RUNTIME_LIBRARY, like every other target of the tree
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${JASPERENGINE_GOOGLETEST_DIR} ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)
    foreach(lib IN ITEMS gtest gtest_main gmock gmock_main)
        if(MSVC)
            target_compile_options(${lib} PRIVATE /w)
        else()
            target_compile_options(${lib} PRIVATE -w)
        endif()
        set_target_properties(${lib} PROPERTIES
            CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
            CXX_STANDARD_REQUIRED ON
            FOLDER "tests/googletest"
        )
    endforeach()
endfunction()

# jasperengine_add_test_target(<name>
#     SOURCES <files...>        the tests (they see gtest, gmock and engine/tests/support)
#     [EDITOR]                  editor tree: the editor's sources without main.cpp join the target
#                               with jasperengine-editor's exact command lines (ccache shares the
#                               objects); the tests compile in an object library of their own
#     [LINK <libs...>]          more libraries for the tests
#     [INCLUDES <dirs...>]      more include directories for the tests
#     [DEFINITIONS <defs...>]   more compile definitions for the tests
#     [ASSET_DIR <dir>]         TestSystem's asset root (default: <test dir>/fixtures)
#     [OUTPUT_DIR <dir>]        images of golden mismatches (default: <test binary dir>/test-output)
#     [LABELS <labels...>]      CTest labels of every test; by default they follow the suite
#                               names (docs/testing.md): *Gpu suites gpu, *Slow suites slow, unit
#                               for the rest
#     [GTEST_MAIN])             gtest_main instead of TestMain.cpp: no engine, no test support
#
# Defines JASPER_TEST_SOURCE_DIR (the test dir: the directory of the calling list file, whose
# golden/ holds the target's golden images) and JASPER_TEST_BINARY_DIR (the current binary dir;
# EDITOR: <build>/tests/<name>, the editor tree's top is its current binary dir) for the tests,
# registers them with CTest (gtest_discover_tests at test time) and applies the warnings of
# JasperWarnings.cmake.
function(jasperengine_add_test_target name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "EDITOR;GTEST_MAIN" "ASSET_DIR;OUTPUT_DIR" "SOURCES;LINK;INCLUDES;DEFINITIONS;LABELS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "jasperengine_add_test_target(${name}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "jasperengine_add_test_target(${name}): no SOURCES")
    endif()
    # (inside a function, CMAKE_CURRENT_LIST_DIR is the calling list file's directory)
    set(testSourceDir "${CMAKE_CURRENT_LIST_DIR}")
    if(arg_EDITOR)
        set(testBinaryDir "${CMAKE_BINARY_DIR}/tests/${name}")
    else()
        set(testBinaryDir "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    if(NOT arg_ASSET_DIR)
        set(arg_ASSET_DIR "${testSourceDir}/fixtures")
    endif()
    if(NOT arg_OUTPUT_DIR)
        set(arg_OUTPUT_DIR "${testBinaryDir}/test-output")
    endif()

    jasperengine_add_googletest()
    if(NOT arg_GTEST_MAIN)
        jasperengine_add_test_support()
    endif()

    set(testSources ${arg_SOURCES})
    if(NOT arg_GTEST_MAIN)
        list(APPEND testSources ${JASPERENGINE_TEST_SUPPORT_DIR}/TestMain.cpp)
    endif()
    set(testDefinitions
        JASPER_TEST_SOURCE_DIR="${testSourceDir}"
        JASPER_TEST_BINARY_DIR="${testBinaryDir}"
        JASPER_TEST_ASSET_DIR="${arg_ASSET_DIR}"
        JASPER_TEST_OUTPUT_DIR="${arg_OUTPUT_DIR}"
        ${arg_DEFINITIONS}
    )
    if(arg_GTEST_MAIN)
        set(testLibs gmock gtest_main gtest)
    else()
        set(testLibs jasperengine-test-support gmock gtest)
    endif()

    if(arg_EDITOR)
        # The tests see googletest; the editor sources keep the editor's command lines
        add_library(${name}-objects OBJECT ${testSources})
        target_link_libraries(${name}-objects PRIVATE ${testLibs} jasperengine yaml-cpp ${arg_LINK})
        target_include_directories(${name}-objects PRIVATE ${CMAKE_SOURCE_DIR}/${EDITOR_DIR} ${GENERATED_DIR} ${arg_INCLUDES})
        target_include_directories(${name}-objects SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/${IMGUI_DIR} ${CMAKE_SOURCE_DIR}/${IMGUI_DIR}/backends)
        target_compile_definitions(${name}-objects PRIVATE ${testDefinitions})
        # sources are UTF-8 whatever the code page
        target_compile_options(${name}-objects PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /bigobj>)
        set_target_properties(${name}-objects PROPERTIES
            CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
            CXX_STANDARD_REQUIRED ON
            OBJCXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
            OBJCXX_STANDARD_REQUIRED ON
        )
        add_dependencies(${name}-objects generate_api_suggestions)
        jasperengine_target_warnings(${name}-objects)
        jasperengine_target_unity_build(${name}-objects)
        jasperengine_target_pch(${name}-objects)

        # the editor's objects, compiled once for the editor and every editor test target
        # (CMakeLists.txt, jasperengine-editor-objects), then the tests'
        add_executable(${name} $<TARGET_OBJECTS:jasperengine-editor-objects> $<TARGET_OBJECTS:${name}-objects>)
        # (with the editor's link libraries and ASan runtime)
        configure_jasperengine_editor_target(${name})
        # the tests include editor headers, which read the editor's own definitions (the C++
        # standard list, the compiler floors, ...)
        get_target_property(editorDefinitions jasperengine-editor-objects COMPILE_DEFINITIONS)
        if(editorDefinitions)
            target_compile_definitions(${name}-objects PRIVATE ${editorDefinitions})
        endif()
        # googletest and the support library by file, not by target: their include directories
        # and definitions would change the editor sources' command lines
        set(fileLibs)
        foreach(lib IN LISTS testLibs)
            list(APPEND fileLibs $<TARGET_FILE:${lib}>)
        endforeach()
        add_dependencies(${name} ${testLibs})
        target_link_libraries(${name} PRIVATE ${fileLibs} ${arg_LINK})
        # next to the editor and its shared engine ($ORIGIN rpath)
        set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    else()
        add_executable(${name} ${testSources})
        target_link_libraries(${name} PRIVATE ${testLibs} jasperengine ${arg_LINK})
        target_include_directories(${name} PRIVATE ${testSourceDir} ${arg_INCLUDES})
        target_compile_definitions(${name} PRIVATE ${testDefinitions})
        set_target_properties(${name} PROPERTIES
            CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
            CXX_STANDARD_REQUIRED ON
            OBJCXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
            OBJCXX_STANDARD_REQUIRED ON
        )
        # sources are UTF-8 whatever the code page
        target_compile_options(${name} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /bigobj>)
        jasperengine_target_warnings(${name})
        jasperengine_target_unity_build(${name})
        jasperengine_target_pch(${name})
        jasperengine_stage_asan_runtime(${name})
    endif()
    set_target_properties(${name} PROPERTIES FOLDER "tests")
    if(WIN32)
        jasperengine_add_windows_manifest(${name})
    endif()

    # CTest: discovered when ctest runs (PRE_TEST), so a build needs no display
    file(MAKE_DIRECTORY ${testBinaryDir})
    set(discovery DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120 WORKING_DIRECTORY ${testBinaryDir})
    if(arg_LABELS)
        gtest_discover_tests(${name} ${discovery} PROPERTIES LABELS "${arg_LABELS}")
    else()
        gtest_discover_tests(${name} ${discovery} TEST_FILTER "-*Gpu.*:*GpuSlow.*:*Slow.*" PROPERTIES LABELS unit)
        gtest_discover_tests(${name} ${discovery} TEST_FILTER "*Gpu.*" PROPERTIES LABELS gpu)
        gtest_discover_tests(${name} ${discovery} TEST_FILTER "*Slow.*-*GpuSlow.*" PROPERTIES LABELS slow)
        gtest_discover_tests(${name} ${discovery} TEST_FILTER "*GpuSlow.*" PROPERTIES LABELS "gpu;slow")
    endif()
endfunction()

# the shared test support library of this tree (engine/tests/support)
function(jasperengine_add_test_support)
    if(TARGET jasperengine-test-support)
        return()
    endif()
    jasperengine_add_googletest()
    add_subdirectory(${JASPERENGINE_TEST_SUPPORT_DIR} ${CMAKE_BINARY_DIR}/tests/support)
endfunction()
