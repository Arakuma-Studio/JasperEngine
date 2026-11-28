# jasperengine-htmlui-tests: unit and golden-image tests of the HTML/CSS UI, built only by
# the standalone engine build with -DJASPERENGINE_HTMLUI_TESTS=ON (tools/build.sh htmlui-tests).
# Tests are Test*.cpp files next to this one; harness/ holds what is HTML UI specific (the
# recording RmlUi render interface, the scene fixture). The headless GPU environment, readback
# and golden-image comparison are engine/tests/support (docs/testing.md).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "jasperengine-htmlui-tests runs on Linux only (Xvfb, Mesa llvmpipe and lavapipe)")
endif()

file(GLOB HTMLUI_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/harness/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anime/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/compat/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/effects/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/property/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/widgets/*.cpp
)

# editor logic without ImGui or editor state that the HTML UI owns (EditorLogicTests.cpp)
set(HTMLUI_TEST_EDITOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../editor")
set(HTMLUI_TEST_EDITOR_SOURCES
    ${HTMLUI_TEST_EDITOR_DIR}/window/widget/HtmlCssLexer.cpp
    ${HTMLUI_TEST_EDITOR_DIR}/util/HtmlUITemplates.cpp
)

# the core tests' allocation counter, for the zero-allocation checks of the gamepad cursor and text
# entry (it counts at malloc through GNU ld's --wrap, so on Linux; elsewhere those checks skip)
set(HTMLUI_TEST_ALLOCATION_COUNTER ${CMAKE_CURRENT_SOURCE_DIR}/../core/support/AllocationCounter.cpp)

jasperengine_add_test_target(jasperengine-htmlui-tests
    SOURCES ${HTMLUI_TEST_SOURCES} ${HTMLUI_TEST_EDITOR_SOURCES} ${HTMLUI_TEST_ALLOCATION_COUNTER}
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/harness
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/support
        ${JASPERENGINE_ROOT}/platform/linux
        ${JASPERENGINE_ROOT}/platform/common
        ${HTMLUI_TEST_EDITOR_DIR}
    LINK rmlui
    DEFINITIONS
        JASPER_HTMLUI_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        JASPER_HTMLUI_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        # shaders and pipelines created at once (spec S32), see engine/tests/support/TestMain.cpp
        JASPER_TEST_SYNC_GPU_CREATION=1
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/htmlui-test-output
    LABELS htmlui gpu
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(jasperengine-htmlui-tests PRIVATE JE_TEST_COUNTS_ALLOCATIONS)
    target_link_options(jasperengine-htmlui-tests PRIVATE "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=realloc" "-rdynamic")
endif()
