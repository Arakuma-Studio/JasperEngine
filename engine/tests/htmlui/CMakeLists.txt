# jasperengine-htmlui-tests: unit and golden-image tests of the HTML/CSS UI, built only by
# the standalone engine build with -DJASPERENGINE_HTMLUI_TESTS=ON (tools/build.sh htmlui-tests).
# Tests are Test*.cpp files next to this one; harness/ holds the headless GPU environment,
# readback and golden-image comparison.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "jasperengine-htmlui-tests runs on Linux only (Xvfb, Mesa llvmpipe and lavapipe)")
endif()

# googletest from the repository's yaml-cpp test tree (not shipped with the engine)
set(JASPER_GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/yaml-cpp/test/gtest-1.11.0")
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)         # gtest 1.11 declares 2.8.12: CMake 4 rejects it, 3.x warns
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(${JASPER_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest EXCLUDE_FROM_ALL)
target_compile_options(gtest PRIVATE -w)

file(GLOB HTMLUI_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/harness/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anime/*.cpp
)

# editor logic without ImGui or editor state, tested here because the editor has no
# googletest target (EditorLogicTests.cpp)
set(HTMLUI_TEST_EDITOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../editor")
set(HTMLUI_TEST_EDITOR_SOURCES
    ${HTMLUI_TEST_EDITOR_DIR}/window/widget/HtmlCssLexer.cpp
    ${HTMLUI_TEST_EDITOR_DIR}/util/HtmlUITemplates.cpp
)

add_executable(jasperengine-htmlui-tests
    ${HTMLUI_TEST_SOURCES}
    ${HTMLUI_TEST_EDITOR_SOURCES}
    ${JASPERENGINE_ROOT}/platform/linux/WindowLinux.cpp
)

target_include_directories(jasperengine-htmlui-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/harness
    ${JASPERENGINE_ROOT}/platform/linux
    ${JASPERENGINE_ROOT}/platform/common
    ${HTMLUI_TEST_EDITOR_DIR}
)

find_package(X11 REQUIRED)
target_link_libraries(jasperengine-htmlui-tests PRIVATE jasperengine rmlui gtest X11::X11)

if(GRAPHIC_BACKEND STREQUAL "vulkan")
    target_sources(jasperengine-htmlui-tests PRIVATE ${JASPERENGINE_ROOT}/platform/common/VulkanContext.cpp)
    target_link_libraries(jasperengine-htmlui-tests PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(jasperengine-htmlui-tests PRIVATE GL)
endif()

target_compile_definitions(jasperengine-htmlui-tests PRIVATE
    JASPER_HTMLUI_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    JASPER_HTMLUI_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

set_target_properties(jasperengine-htmlui-tests PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
)
