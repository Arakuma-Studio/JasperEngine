# jasperengine-test-support: what every googletest target of a tree shares (docs/testing.md).
# TestSystem (the engine's System for tests, with a log copy), TestPaths, TempDir, LogCapture,
# ImageCompare (golden images) and GpuEnvironment (the engine initialised without a GPU, and the
# headless device GPU suites ask for). TestMain.cpp is not in it: jasperengine_add_test_target
# compiles it into each target, with that target's directories.
#
# The headless device (HeadlessContext, Readback) exists in the standalone engine tree: on Linux
# Xvfb + GLX or lavapipe, on macOS the default Metal device; elsewhere GpuEnvironment::ensure()
# fails with a reason.

set(JASPER_TEST_SUPPORT_SOURCES
    GpuEnvironment.cpp
    ImageCompare.cpp
    LogCapture.cpp
    TempDir.cpp
    TestPaths.cpp
    TestSystem.cpp
)

set(JASPER_TEST_SUPPORT_GPU OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT JASPERENGINE_SHARED)
    set(JASPER_TEST_SUPPORT_GPU ON)
    list(APPEND JASPER_TEST_SUPPORT_SOURCES
        HeadlessContext.cpp
        Readback.cpp
        ${JASPERENGINE_ROOT}/platform/linux/WindowLinux.cpp
        # the hidden-window state and the pads of exported games (core-tests X11Visibility.*,
        # GamepadLinux.*)
        ${JASPERENGINE_ROOT}/platform/linux/X11Visibility.cpp
        ${JASPERENGINE_ROOT}/platform/linux/GamepadLinux.cpp
        ${JASPERENGINE_ROOT}/platform/linux/GamepadDB.cpp
    )
    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND JASPER_TEST_SUPPORT_SOURCES ${JASPERENGINE_ROOT}/platform/common/VulkanContext.cpp)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND GRAPHIC_BACKEND STREQUAL "metal" AND NOT JASPERENGINE_SHARED)
    set(JASPER_TEST_SUPPORT_GPU ON)
    list(APPEND JASPER_TEST_SUPPORT_SOURCES
        HeadlessContextMetal.mm
        ReadbackMetal.mm
    )
endif()

add_library(jasperengine-test-support STATIC ${JASPER_TEST_SUPPORT_SOURCES})

target_include_directories(jasperengine-test-support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# gtest privately: the editor tests link this library by file (JasperTesting.cmake)
target_link_libraries(jasperengine-test-support PRIVATE jasperengine gtest)
target_compile_definitions(jasperengine-test-support PUBLIC $<$<BOOL:${JASPER_TEST_SUPPORT_GPU}>:JASPER_TEST_HAS_GPU>)

if(JASPER_TEST_SUPPORT_GPU AND APPLE)
    target_link_libraries(jasperengine-test-support PUBLIC "-framework Metal" "-framework Foundation")
elseif(JASPER_TEST_SUPPORT_GPU)
    find_package(X11 REQUIRED)
    # public: the core tests include X11Visibility.h and GamepadLinux.h
    target_include_directories(jasperengine-test-support PUBLIC
        ${JASPERENGINE_ROOT}/platform/linux
        ${JASPERENGINE_ROOT}/platform/common
    )
    target_link_libraries(jasperengine-test-support PUBLIC X11::X11)
    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        target_link_libraries(jasperengine-test-support PUBLIC Vulkan::Vulkan)
    else()
        target_link_libraries(jasperengine-test-support PUBLIC GL)
    endif()
endif()

set_target_properties(jasperengine-test-support PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    FOLDER "tests"
)
target_compile_options(jasperengine-test-support PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
jasperengine_target_warnings(jasperengine-test-support)
jasperengine_skip_unity_build(
    REASON "Xlib.h defines None, Bool, Status, Always... as macros, which break every later source of the batch"
    SOURCES ${JASPERENGINE_ROOT}/platform/linux/WindowLinux.cpp ${JASPERENGINE_ROOT}/platform/linux/X11Visibility.cpp)
jasperengine_target_unity_build(jasperengine-test-support)
jasperengine_target_pch(jasperengine-test-support)
