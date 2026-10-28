# The C++ standard of the engine, the editor and the exported game, and the source of
# cxxStandards in the editor (JASPERENGINE_CXX_STANDARD_LIST); Play plugins read the project
# setting (editor/Generator.cpp). The first entry is the default.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/JasperEngineToolchain.cmake)
set(JASPERENGINE_CXX_STANDARDS 20 23)
if(NOT DEFINED JASPERENGINE_CXX_STANDARD)
    set(JASPERENGINE_CXX_STANDARD 20 CACHE STRING "C++ standard for the engine, editor and project")
    set_property(CACHE JASPERENGINE_CXX_STANDARD PROPERTY STRINGS ${JASPERENGINE_CXX_STANDARDS})
elseif("$CACHE{JASPERENGINE_CXX_STANDARD}" STREQUAL "17")
    # a build tree configured before C++17 was dropped
    message(WARNING "JASPERENGINE_CXX_STANDARD 17 is no longer supported; using 20")
    set(JASPERENGINE_CXX_STANDARD 20 CACHE STRING "C++ standard for the engine, editor and project" FORCE)
    set_property(CACHE JASPERENGINE_CXX_STANDARD PROPERTY STRINGS ${JASPERENGINE_CXX_STANDARDS})
endif()

option(JASPERENGINE_PHYSICS_2D "Enable Box2D-based 2D physics" ON)
option(JASPERENGINE_PHYSICS_3D "Enable Jolt-based 3D physics" ON)
# Fallback for one release: Jolt's own JobSystemThreadPool instead of the engine's job system
option(JE_JOLT_OWN_POOL "Run Jolt physics jobs on Jolt's own thread pool" OFF)
# Compiles at runtime the shader variants a build does not ship precompiled (scripted
# meshes and materials, properties changed while playing) instead of leaving out what
# draws with them. Adds the shader compiler and the engine shader library to the build.
option(JASPERENGINE_RUNTIME_SHADERS "Compile missing shader variants at runtime" ON)
# The editor export turns it off unless the project enables AdMob for iOS (Android's
# Gradle build gets the same switch from the export)
option(JASPERENGINE_ADMOB "Link Google Mobile Ads into iOS builds" ON)
# googletest suite of the HTML/CSS UI (engine/tests/htmlui), standalone engine builds only:
# the editor's shared engine hides the RmlUi symbols the tests need (tools/build.sh htmlui-tests)
option(JASPERENGINE_HTMLUI_TESTS "Build jasperengine-htmlui-tests" OFF)
# The unit tests (docs/testing.md): jasperengine-core-tests in a standalone engine build
# (engine/tests/core, against the static engine, with the job system and allocator benchmarks
# je-bench-jobs and je-bench-memory), jasperengine-editor-tests in the editor build
option(JASPERENGINE_TESTS "Build the unit-test target of this build tree" OFF)
# the S07 ECS benchmark (tools/performance/run-ecs-bench.py), standalone engine builds only
option(JASPERENGINE_ECS_BENCH "Build jasperengine-ecs-bench" OFF)
# Navigation meshes, queries and crowds (Recast/Detour, engine/libs/recastnavigation, spec S19);
# OFF leaves the library out and the navmesh API fails (grids keep working). The export sets it
# from Project Settings > Navigation.
option(JASPERENGINE_NAVIGATION "Compile Recast/Detour navigation meshes and crowds" ON)

# =============================================================
# STANDALONE PROJECT SETUP
# Only runs when this file is used as the top-level build
# (i.e. building jasperengine-project outside the editor).
# =============================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)

    if(NOT DEFINED APP_NAME)
        set(APP_NAME jasperengine-project)
    endif()

    project(${APP_NAME})

    # before any target: sanitizers apply to the whole tree (empty by default, as in every export)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/JasperSanitizers.cmake)

    set(JASPERENGINE_SHARED OFF)

    if(NOT JASPERENGINE_ROOT)
        set(JASPERENGINE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    file(TO_CMAKE_PATH ${JASPERENGINE_ROOT} JASPERENGINE_ROOT)

    if(NOT PROJECT_ROOT)
        set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/project)
    endif()
    if(NOT EXISTS "${PROJECT_ROOT}")
        message(FATAL_ERROR "Can't find project root directory: ${PROJECT_ROOT}")
    endif()
    file(TO_CMAKE_PATH ${PROJECT_ROOT} PROJECT_ROOT)

    # Project-wide runtime settings are injected here by the editor Exporter.
    # Standalone engine builds keep the defaults declared by each platform.
    # @JASPERENGINE_PROJECT_SETTINGS@
    if(NOT DEFINED JASPERENGINE_VSYNC_ENABLED)
        set(JASPERENGINE_VSYNC_ENABLED ON)
    endif()
    if(JASPERENGINE_VSYNC_ENABLED)
        set(JASPERENGINE_VSYNC_ENABLED_VALUE 1)
    else()
        set(JASPERENGINE_VSYNC_ENABLED_VALUE 0)
    endif()
    add_definitions("-DJASPERENGINE_VSYNC_ENABLED=${JASPERENGINE_VSYNC_ENABLED_VALUE}")

    # Player > Logging (the Exporter sets them): the game's log level, player.log, backtraces
    if(DEFINED JASPERENGINE_LOG_LEVEL)
        add_definitions("-DJASPERENGINE_LOG_LEVEL=\"${JASPERENGINE_LOG_LEVEL}\"")
    endif()
    if(DEFINED JASPERENGINE_LOG_FILE)
        if(JASPERENGINE_LOG_FILE)
            add_definitions("-DJASPERENGINE_LOG_FILE=1")
        else()
            add_definitions("-DJASPERENGINE_LOG_FILE=0")
        endif()
    endif()
    if(DEFINED JASPERENGINE_LOG_BACKTRACE)
        if(JASPERENGINE_LOG_BACKTRACE)
            add_definitions("-DJASPERENGINE_LOG_BACKTRACE=1")
        else()
            add_definitions("-DJASPERENGINE_LOG_BACKTRACE=0")
        endif()
    endif()

    # Player > Crash Handling (the Exporter sets them; core/Engine.cpp has the defaults)
    foreach(crashSetting JASPERENGINE_CRASH_HANDLER JASPERENGINE_CRASH_LOG_TAIL JASPERENGINE_CRASH_DUMP_WITH_HEAP
            JASPERENGINE_CRASH_HANG_WATCHDOG JASPERENGINE_CRASH_WEB_OVERLAY)
        if(DEFINED ${crashSetting})
            if(${crashSetting})
                add_definitions("-D${crashSetting}=1")
            else()
                add_definitions("-D${crashSetting}=0")
            endif()
        endif()
    endforeach()
    if(DEFINED JASPERENGINE_CRASH_HANG_MS)
        add_definitions("-DJASPERENGINE_CRASH_HANG_MS=${JASPERENGINE_CRASH_HANG_MS}")
    endif()

    if(NOT DEFINED JASPERENGINE_WINDOW_WIDTH)
        set(JASPERENGINE_WINDOW_WIDTH 960)
    endif()
    if(NOT DEFINED JASPERENGINE_WINDOW_HEIGHT)
        set(JASPERENGINE_WINDOW_HEIGHT 540)
    endif()
    # 0 = windowed, 1 = maximized, 2 = fullscreen
    if(NOT DEFINED JASPERENGINE_WINDOW_MODE)
        set(JASPERENGINE_WINDOW_MODE 0)
    endif()
    if(NOT DEFINED JASPERENGINE_WINDOW_RESIZABLE)
        set(JASPERENGINE_WINDOW_RESIZABLE ON)
    endif()
    if(NOT DEFINED JASPERENGINE_WINDOW_TITLE)
        set(JASPERENGINE_WINDOW_TITLE "JasperEngine")
    endif()
    # Prefix of the web build's saved keys (the export writes one made safe for a define)
    if(NOT DEFINED JASPERENGINE_STORAGE_ID)
        set(JASPERENGINE_STORAGE_ID "${APP_NAME}")
    endif()
    # ON only when the editor export generated project/app_icon.h (+ .ico/.rc)
    if(NOT DEFINED JASPERENGINE_WINDOW_ICON)
        set(JASPERENGINE_WINDOW_ICON OFF)
    endif()
    if(JASPERENGINE_WINDOW_RESIZABLE)
        set(JASPERENGINE_WINDOW_RESIZABLE_VALUE 1)
    else()
        set(JASPERENGINE_WINDOW_RESIZABLE_VALUE 0)
    endif()
    add_definitions("-DDEFAULT_WINDOW_WIDTH=${JASPERENGINE_WINDOW_WIDTH}")
    add_definitions("-DDEFAULT_WINDOW_HEIGHT=${JASPERENGINE_WINDOW_HEIGHT}")
    add_definitions("-DJASPERENGINE_WINDOW_MODE=${JASPERENGINE_WINDOW_MODE}")
    add_definitions("-DJASPERENGINE_WINDOW_RESIZABLE=${JASPERENGINE_WINDOW_RESIZABLE_VALUE}")
    add_definitions("-DJASPERENGINE_WINDOW_TITLE=\"${JASPERENGINE_WINDOW_TITLE}\"")
    add_definitions("-DJASPERENGINE_STORAGE_ID=\"${JASPERENGINE_STORAGE_ID}\"")
    # X11 class / Wayland app_id (APP_NAME is a plain identifier, safe to quote).
    # Desktop environments match it against <APP_NAME>.desktop for the app icon.
    add_definitions("-DJASPERENGINE_APP_ID=\"${APP_NAME}\"")
    if(JASPERENGINE_WINDOW_ICON AND EXISTS "${PROJECT_ROOT}/app_icon.h")
        add_definitions("-DJASPERENGINE_WINDOW_ICON")
    endif()

    # Per-project HybridArray capacities (SceneMaxValues) are injected here by the
    # editor Exporter, replacing the marker below. When building manually (without
    # the editor), the marker is left untouched and the engine defaults declared in
    # core/Engine.h apply.
    # @JASPERENGINE_SCENE_MAX_VALUES@

    if(NOT GRAPHIC_BACKEND)
        if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
            set(GRAPHIC_BACKEND "gles3")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
               CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
            set(GRAPHIC_BACKEND "glcore")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(GRAPHIC_BACKEND "metal")
        else()
            message(FATAL_ERROR "GRAPHIC_BACKEND is not set")
        endif()
    endif()
    message(STATUS "Graphic backend is set to ${GRAPHIC_BACKEND}")

    if(NOT APP_BACKEND)
        if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
            set(APP_BACKEND "emscripten")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
            set(APP_BACKEND "android")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            set(APP_BACKEND "win")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(APP_BACKEND "linux")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
            set(APP_BACKEND "linux")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(APP_BACKEND "apple")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(APP_BACKEND "apple")
        else()
            message(FATAL_ERROR "APP_BACKEND is not set")
        endif()
    endif()
    if((NOT JASPERENGINE_VSYNC_ENABLED) AND (CMAKE_SYSTEM_NAME STREQUAL "Darwin") AND
       (APP_BACKEND STREQUAL "apple"))
        message(WARNING "VSync off is not supported by the macOS Metal application backend; this build will remain synchronized")
    endif()
    message(STATUS "Application backend is set to ${APP_BACKEND}")

    set(PLATFORM_EXEC_FLAGS)
    set(PLATFORM_ROOT)
    set(PLATFORM_SOURCE)
    set(PLATFORM_LIBS)
    set(PLATFORM_RESOURCES)
    set(PLATFORM_PROPERTIES)
    set(PLATFORM_OPTIONS)

    # FindVulkan reads the VULKAN_SDK environment variable the SDK installer
    # sets. A macro, not a function, so Vulkan_FOUND lands in the caller.
    macro(jasperengine_find_vulkan backendOption)
        # sokol_gfx binds every resource through VK_EXT_descriptor_buffer, which
        # MoltenVK does not implement, so the build only fails at device selection
        if(APPLE)
            message(WARNING "${backendOption}=vulkan runs on MoltenVK here, which does not implement VK_EXT_descriptor_buffer: this build will fail at startup unless the driver provides it. ${backendOption}=metal is the working macOS backend.")
        endif()

        # An app started from the Dock inherits no shell environment, so on macOS
        # the SDK is also looked for where its installer unpacks it
        if(APPLE AND NOT DEFINED ENV{VULKAN_SDK})
            file(GLOB jasperengineVulkanSdkDirs "$ENV{HOME}/VulkanSDK/*/macOS")
            if(jasperengineVulkanSdkDirs)
                list(SORT jasperengineVulkanSdkDirs)
                list(POP_BACK jasperengineVulkanSdkDirs jasperengineVulkanSdkNewest)
                set(ENV{VULKAN_SDK} "${jasperengineVulkanSdkNewest}")
                message(STATUS "Using the Vulkan SDK at ${jasperengineVulkanSdkNewest}")
            endif()
        endif()
        find_package(Vulkan QUIET)
        if(NOT Vulkan_FOUND)
            message(FATAL_ERROR "${backendOption}=vulkan needs the Vulkan SDK (https://vulkan.lunarg.com/sdk/home), and VULKAN_SDK is not set here. If it is installed outside the default location, restart the shell or editor this build was started from with VULKAN_SDK in its environment: Windows only gives the variable to processes started after the installer ran. Otherwise use ${backendOption}=glcore, or metal on macOS.")
        endif()
    endmacro()

    if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        add_definitions("-DJASPERENGINE_WEB")

        if(GRAPHIC_BACKEND STREQUAL "gles3")
            add_definitions("-DSOKOL_GLES3")
            set(USE_WEBGL2 "-s USE_WEBGL2=1")
        endif()

        add_definitions("-DWITH_MINIAUDIO")

        set(CMAKE_EXECUTABLE_SUFFIX ".html")

        set(PLATFORM_ROOT ${JASPERENGINE_ROOT}/platform/emscripten)

        set(EM_PRELOAD_FILES)
        if(EXISTS "${PROJECT_ROOT}/assets")
            set(EM_PRELOAD_FILES "${EM_PRELOAD_FILES} --preload-file ${PROJECT_ROOT}/assets@/")
            # the web loads the web family of the cooked textures only (S14 R9)
            foreach(family desktop mobile android_etc2)
                set(EM_PRELOAD_FILES "${EM_PRELOAD_FILES} --exclude-file */je-cooked/${family}")
            endforeach()
        endif()
        if(EXISTS "${PROJECT_ROOT}/lua")
            set(EM_PRELOAD_FILES "${EM_PRELOAD_FILES} --preload-file ${PROJECT_ROOT}/lua@/")
        endif()

        list(APPEND PLATFORM_SOURCE
            ${PLATFORM_ROOT}/JasperEngineWeb.cpp
            ${PLATFORM_ROOT}/main.cpp
        )

        list(APPEND PLATFORM_LIBS
            idbfs.js
        )

        option(EMSCRIPTEN_THREAD_SUPPORT "Enable pthreads for Emscripten builds" OFF)

        if(EMSCRIPTEN_THREAD_SUPPORT)
            # The pool is created at startup and never grows, and the engine sizes its threads
            # to it (JE_PTHREAD_POOL_SIZE, thread::webThreadPoolSize): the job system's workers
            # (at most 4, and pool - 3), one blocking thread, a video decoder, and a spare an
            # asset reader may take. STRICT=2 aborts loudly instead of deadlocking when a thread
            # is asked beyond it.
            set(JASPERENGINE_WEB_PTHREAD_POOL_SIZE 8 CACHE STRING "Emscripten pthread pool of the threaded web build")
            set(USE_PTHREADS "-s PTHREAD_POOL_SIZE=${JASPERENGINE_WEB_PTHREAD_POOL_SIZE} -s PTHREAD_POOL_SIZE_STRICT=2")
            add_compile_definitions(JE_PTHREAD_POOL_SIZE=${JASPERENGINE_WEB_PTHREAD_POOL_SIZE})
            list(APPEND PLATFORM_OPTIONS -pthread)
            # shared memory links only objects built with atomics: every library of the tree,
            # not only the engine (wasm-ld refuses glslang's PoolAlloc.cpp otherwise)
            add_compile_options(-pthread)
            # and the link: without it Emscripten links its pthread stubs, and the first thread
            # the engine starts fails with EOPNOTSUPP
            add_link_options(-pthread)
        endif()

        # GROWABLE_ARRAYBUFFERS only exists on recent Emscripten, and emcc errors
        # out on unknown -s names, so probe settings.js instead of passing it
        # blindly. SDKs without the setting predate the feature entirely — their
        # behaviour already matches =0, so omitting the flag there is equivalent.
        set(ALLOW_MEMORY_GROWTH "-s ALLOW_MEMORY_GROWTH=1")
        if(EXISTS "${EMSCRIPTEN_ROOT_PATH}/src/settings.js")
            file(STRINGS "${EMSCRIPTEN_ROOT_PATH}/src/settings.js" HAS_GROWABLE_ARRAYBUFFERS
                REGEX "^var GROWABLE_ARRAYBUFFERS" LIMIT_COUNT 1)
            if(HAS_GROWABLE_ARRAYBUFFERS)
                set(ALLOW_MEMORY_GROWTH "${ALLOW_MEMORY_GROWTH} -s GROWABLE_ARRAYBUFFERS=0")
            endif()
        endif()

        # DWARF limits the wasm optimizer, so release builds keep only the function names
        # (still readable stack traces) and the wasm is about a third smaller
        set(EM_DEBUG_INFO "--profiling-funcs")
        if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
            set(EM_DEBUG_INFO "-g")
        endif()

        list(APPEND PLATFORM_PROPERTIES
            LINK_FLAGS
                "${EM_DEBUG_INFO} \
                -s INITIAL_MEMORY=256MB \
                -s STACK_SIZE=4MB \
                -s EXPORTED_FUNCTIONS=\"['_getScreenWidth','_getScreenHeight','_changeCanvasSize','_main']\" \
                -s EXPORTED_RUNTIME_METHODS=\"['ccall', 'cwrap']\" \
                --pre-js ${PLATFORM_ROOT}/crash_pre.js \
                ${EM_PRELOAD_FILES} \
                ${EM_ADDITIONAL_LINK_FLAGS} \
                ${USE_PTHREADS} \
                ${ALLOW_MEMORY_GROWTH} \
                ${USE_WEBGL2}"
            # the pre-js is read at link time: a change of it relinks
            LINK_DEPENDS ${PLATFORM_ROOT}/crash_pre.js
        )
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Android")
        add_definitions("-DJASPERENGINE_ANDROID")

        if(GRAPHIC_BACKEND STREQUAL "gles3")
            add_definitions("-DSOKOL_GLES3")
            set(OPENGL_LIB GLESv3)
        endif()

        add_definitions("-D\"lua_getlocaledecpoint()='.'\"")
        add_definitions("-DLUA_ANSI")
        add_definitions("-DLUA_USE_C89")
        add_definitions("-DWITH_MINIAUDIO")

        set(APP_NAME jasperengine-android)

        if(ANDROID_ABI MATCHES "^arm(eabi)?(-v7a)?$")
            add_compile_options("-mfpu=fp-armv8")
        endif()

        set(PLATFORM_ROOT ${JASPERENGINE_ROOT}/platform/android)

        list(APPEND PLATFORM_SOURCE
            ${PLATFORM_ROOT}/JasperEngineAndroid.cpp
            ${PLATFORM_ROOT}/AndroidMain.cpp
            ${PLATFORM_ROOT}/NativeEngine.cpp
        )

        set(CMAKE_SHARED_LINKER_FLAGS
            "${CMAKE_SHARED_LINKER_FLAGS} -u Java_com_google_androidgamesdk_GameActivity_initializeNativeCode")

        find_package(game-activity REQUIRED CONFIG)
        find_package(games-frame-pacing REQUIRED CONFIG)

        list(APPEND PLATFORM_LIBS
            ${OPENGL_LIB}
            log
            android
            EGL
            OpenSLES
            game-activity::game-activity_static
            games-frame-pacing::swappy_static
        )
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        add_definitions("-DJASPERENGINE_WIN")

        # The native Win32 backend creates a WGL context, a Vulkan surface and
        # swapchain, or a D3D11 device and swapchain.
        if(GRAPHIC_BACKEND STREQUAL "glcore")
            add_definitions("-DSOKOL_GLCORE")
        elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
            add_definitions("-DSOKOL_VULKAN")
            add_definitions("-DVK_USE_PLATFORM_WIN32_KHR")
        elseif(GRAPHIC_BACKEND STREQUAL "d3d11")
            add_definitions("-DSOKOL_D3D11")
        else()
            message(FATAL_ERROR "APP_BACKEND=win supports GRAPHIC_BACKEND glcore, vulkan or d3d11 (got '${GRAPHIC_BACKEND}')")
        endif()

        add_definitions("-DWITH_MINIAUDIO")

        if(EXISTS "${PROJECT_ROOT}/assets")
            set(ASSETS_DEST_DIR ${CMAKE_BINARY_DIR}/$<CONFIG>/assets)
        endif()
        if(EXISTS "${PROJECT_ROOT}/lua")
            set(LUA_DEST_DIR ${CMAKE_BINARY_DIR}/$<CONFIG>/lua)
        endif()

        set(PLATFORM_EXEC_FLAGS WIN32)

        if(MSVC)
            # The windowed subsystem above enters at WinMain, but the backend's
            # entry point is main() (platform/win/main.cpp). MinGW handles this
            # on its own with -mwindows.
            list(APPEND PLATFORM_PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
        endif()

        set(PLATFORM_ROOT ${JASPERENGINE_ROOT}/platform/win)

        list(APPEND PLATFORM_SOURCE
            ${PLATFORM_ROOT}/JasperEngineWin.cpp
            ${PLATFORM_ROOT}/WindowWin.cpp
            ${PLATFORM_ROOT}/SystemWin.cpp
            ${PLATFORM_ROOT}/WinInputRouter.cpp
            ${PLATFORM_ROOT}/GamepadWin.cpp
            ${PLATFORM_ROOT}/main.cpp
        )

        list(APPEND PLATFORM_LIBS
            user32
            gdi32
            shell32
            imm32 # IME composition for HTML UI text fields (WinInputRouter)
        )

        if(GRAPHIC_BACKEND STREQUAL "vulkan")
            # Device and swapchain are shared with the other desktops
            include_directories(${JASPERENGINE_ROOT}/platform/common)
            list(APPEND PLATFORM_SOURCE
                ${JASPERENGINE_ROOT}/platform/common/VulkanContext.cpp
            )
            jasperengine_find_vulkan("GRAPHIC_BACKEND")
            list(APPEND PLATFORM_LIBS Vulkan::Vulkan)
        elseif(GRAPHIC_BACKEND STREQUAL "d3d11")
            # d3d11 and dxgi ship with the Windows SDK, nothing to find
            list(APPEND PLATFORM_SOURCE
                ${PLATFORM_ROOT}/D3D11Context.cpp
            )
            list(APPEND PLATFORM_LIBS d3d11 dxgi)
        else()
            list(APPEND PLATFORM_LIBS opengl32)
        endif()

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            list(APPEND PLATFORM_LIBS
                -static -static-libgcc -static-libstdc++
            )
        endif()
    endif()

    if((CMAKE_SYSTEM_NAME STREQUAL "Linux") OR (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD"))
        # The native X11 backend creates a GLX context or a Vulkan surface and
        # swapchain.
        # gles3 runs the mobile/web backend in an OpenGL ES context (GLX), for testing
        # (Mesa provides it; needs the GLES headers and libGLESv2)
        if(GRAPHIC_BACKEND STREQUAL "glcore")
            add_definitions("-DSOKOL_GLCORE")
        elseif(GRAPHIC_BACKEND STREQUAL "gles3")
            add_definitions("-DSOKOL_GLES3")
        elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
            add_definitions("-DSOKOL_VULKAN")
            add_definitions("-DVK_USE_PLATFORM_XLIB_KHR")
        else()
            message(FATAL_ERROR "APP_BACKEND=linux supports GRAPHIC_BACKEND glcore, vulkan or gles3 (got '${GRAPHIC_BACKEND}')")
        endif()

        add_definitions("-DWITH_MINIAUDIO")

        if(EXISTS "${PROJECT_ROOT}/assets")
            set(ASSETS_DEST_DIR ${CMAKE_BINARY_DIR}/assets)
        endif()
        if(EXISTS "${PROJECT_ROOT}/lua")
            set(LUA_DEST_DIR ${CMAKE_BINARY_DIR}/lua)
        endif()

        add_definitions("-DJASPERENGINE_LINUX")

        find_package(X11 REQUIRED)

        set(PLATFORM_ROOT ${JASPERENGINE_ROOT}/platform/linux)

        list(APPEND PLATFORM_SOURCE
            ${PLATFORM_ROOT}/JasperEngineLinux.cpp
            ${PLATFORM_ROOT}/WindowLinux.cpp
            ${PLATFORM_ROOT}/SystemLinux.cpp
            ${PLATFORM_ROOT}/LinuxInputRouter.cpp
            ${PLATFORM_ROOT}/GamepadLinux.cpp
            ${PLATFORM_ROOT}/GamepadDB.cpp
            ${PLATFORM_ROOT}/main.cpp
        )

        # GamepadDB parses the community mapping database in common/
        include_directories(${JASPERENGINE_ROOT}/platform/common)

        list(APPEND PLATFORM_LIBS
            X11::X11 dl m
        )

        if(GRAPHIC_BACKEND STREQUAL "vulkan")
            # The device and swapchain are the same on every desktop; only the
            # surface differs, so the backend passes that in.
            list(APPEND PLATFORM_SOURCE
                ${JASPERENGINE_ROOT}/platform/common/VulkanContext.cpp
            )
            jasperengine_find_vulkan("GRAPHIC_BACKEND")
            list(APPEND PLATFORM_LIBS Vulkan::Vulkan)
        elseif(GRAPHIC_BACKEND STREQUAL "gles3")
            # the GLES entry points, and GLX for the context
            list(APPEND PLATFORM_LIBS GLESv2 GL)
        else()
            list(APPEND PLATFORM_LIBS GL)
        endif()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        # The native Cocoa backend creates a GL context, a Metal device, or a
        # Vulkan surface and swapchain over a CAMetalLayer.
        if(GRAPHIC_BACKEND STREQUAL "glcore")
            add_definitions("-DSOKOL_GLCORE")
        elseif(GRAPHIC_BACKEND STREQUAL "metal")
            add_definitions("-DSOKOL_METAL")
        elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
            add_definitions("-DSOKOL_VULKAN")
            add_definitions("-DVK_USE_PLATFORM_METAL_EXT")
        else()
            message(FATAL_ERROR "APP_BACKEND=apple supports GRAPHIC_BACKEND glcore, metal or vulkan (got '${GRAPHIC_BACKEND}')")
        endif()

        add_definitions("-DWITH_MINIAUDIO")

        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fobjc-arc")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fobjc-arc")
        # Xcode 27 starts at macOS 12
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")

        # Only the Xcode generator assembles the real .app that the storyboard
        # entry point in platform/apple needs, shared with iOS. Every other
        # generator builds a bare executable, on the backend in platform/mac.
        if(GRAPHIC_BACKEND STREQUAL "metal" AND CMAKE_GENERATOR STREQUAL "Xcode")
            add_definitions("-DJASPERENGINE_APPLE")

            set(PLATFORM_ROOT ${JASPERENGINE_ROOT}/platform/apple)

            set(APP_BUNDLE_IDENTIFIER "com.jaspergameengine.jasperengine")
            set(PLATFORM_EXEC_FLAGS MACOSX_BUNDLE)

            include_directories(/System/Library/Frameworks)

            # The drawable and the input translation come from platform/mac
            include_directories(${JASPERENGINE_ROOT}/platform/mac)

            list(APPEND PLATFORM_SOURCE
                ${JASPERENGINE_ROOT}/platform/mac/MacInputRouter.mm
                ${JASPERENGINE_ROOT}/platform/mac/MacTextInputClient.mm
                ${JASPERENGINE_ROOT}/platform/mac/MacViewMetal.mm
                ${PLATFORM_ROOT}/macos/EngineView.h
                ${PLATFORM_ROOT}/macos/EngineView.mm
                ${PLATFORM_ROOT}/macos/main.m
                ${PLATFORM_ROOT}/macos/AppDelegate.h
                ${PLATFORM_ROOT}/macos/AppDelegate.m
                ${PLATFORM_ROOT}/macos/ViewController.h
                ${PLATFORM_ROOT}/macos/ViewController.m
                ${PLATFORM_ROOT}/Renderer.h
                ${PLATFORM_ROOT}/Renderer.mm
                ${PLATFORM_ROOT}/JasperEngineApple.h
                ${PLATFORM_ROOT}/JasperEngineApple.mm
                ${PLATFORM_ROOT}/JasperEngineGameController.h
                ${PLATFORM_ROOT}/JasperEngineGameController.mm
            )

            list(APPEND PLATFORM_RESOURCES
                ${PLATFORM_ROOT}/macos/Main.storyboard
            )
            set(BUNDLE_RESOURCES_DIR $<TARGET_BUNDLE_CONTENT_DIR:${APP_NAME}>/Resources)

            list(APPEND PLATFORM_PROPERTIES
                MACOSX_BUNDLE_INFO_PLIST "${JASPERENGINE_ROOT}/workspaces/xcode/macos/Info.plist"
                XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
            )

            list(APPEND PLATFORM_LIBS
                "-framework Cocoa"
                "-framework Metal"
                "-framework MetalKit"
                "-framework QuartzCore"
                "-framework GameController"
            )
        else()
            set(PLATFORM_ROOT ${JASPERENGINE_ROOT}/platform/mac)

            if(EXISTS "${PROJECT_ROOT}/assets")
                set(ASSETS_DEST_DIR ${CMAKE_BINARY_DIR}/assets)
            endif()
            if(EXISTS "${PROJECT_ROOT}/lua")
                set(LUA_DEST_DIR ${CMAKE_BINARY_DIR}/lua)
            endif()

            # Gamepads come from the same GameController code iOS and the editor use
            include_directories(${JASPERENGINE_ROOT}/platform/apple)

            list(APPEND PLATFORM_SOURCE
                ${PLATFORM_ROOT}/JasperEngineMac.h
                ${PLATFORM_ROOT}/JasperEngineMac.mm
                ${PLATFORM_ROOT}/WindowMac.mm
                ${PLATFORM_ROOT}/SystemMac.mm
                ${PLATFORM_ROOT}/MacInputRouter.mm
                ${PLATFORM_ROOT}/MacTextInputClient.mm
                ${PLATFORM_ROOT}/main.mm
                ${JASPERENGINE_ROOT}/platform/apple/JasperEngineGameController.mm
            )

            list(APPEND PLATFORM_LIBS
                "-framework Cocoa"
                "-framework QuartzCore"
                "-framework GameController"
            )

            # MTKView has no GL or Vulkan drawable, so those get views of their own
            if(GRAPHIC_BACKEND STREQUAL "glcore")
                list(APPEND PLATFORM_SOURCE ${PLATFORM_ROOT}/MacViewGL.mm)
                list(APPEND PLATFORM_LIBS "-framework OpenGL")
            elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
                # Device and swapchain are shared with the other desktops
                include_directories(${JASPERENGINE_ROOT}/platform/common)
                list(APPEND PLATFORM_SOURCE
                    ${PLATFORM_ROOT}/MacViewVulkan.mm
                    ${JASPERENGINE_ROOT}/platform/common/VulkanContext.cpp
                )
                jasperengine_find_vulkan("GRAPHIC_BACKEND")
                list(APPEND PLATFORM_LIBS Vulkan::Vulkan)
            else()
                list(APPEND PLATFORM_SOURCE ${PLATFORM_ROOT}/MacViewMetal.mm)
                list(APPEND PLATFORM_LIBS "-framework Metal" "-framework MetalKit")
            endif()

            list(APPEND PLATFORM_PROPERTIES
                XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
            )
        endif()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        add_definitions("-DJASPERENGINE_APPLE")

        if(GRAPHIC_BACKEND STREQUAL "metal")
            add_definitions("-DSOKOL_METAL")
        endif()

        add_definitions("-DWITH_MINIAUDIO")

        set(PLATFORM_ROOT ${JASPERENGINE_ROOT}/platform/apple)

        set(APP_BUNDLE_IDENTIFIER "com.jaspergameengine.jasperengine")

        include_directories(/System/Library/Frameworks)

        list(APPEND PLATFORM_SOURCE
            ${PLATFORM_ROOT}/ios/main.m
            ${PLATFORM_ROOT}/ios/AppDelegate.h
            ${PLATFORM_ROOT}/ios/AppDelegate.m
            ${PLATFORM_ROOT}/ios/SceneDelegate.h
            ${PLATFORM_ROOT}/ios/SceneDelegate.m
            ${PLATFORM_ROOT}/ios/EngineView.h
            ${PLATFORM_ROOT}/ios/EngineView.mm
            ${PLATFORM_ROOT}/ios/KeyCodesIOS.h
            ${PLATFORM_ROOT}/ios/ViewController.h
            ${PLATFORM_ROOT}/ios/ViewController.m
            ${PLATFORM_ROOT}/Renderer.h
            ${PLATFORM_ROOT}/Renderer.mm
            ${PLATFORM_ROOT}/JasperEngineApple.h
            ${PLATFORM_ROOT}/JasperEngineApple.mm
            ${PLATFORM_ROOT}/JasperEngineGameController.h
            ${PLATFORM_ROOT}/JasperEngineGameController.mm
        )

        list(APPEND PLATFORM_RESOURCES
            ${PLATFORM_ROOT}/ios/LaunchScreen.storyboard
            ${PLATFORM_ROOT}/ios/Main.storyboard
        )
        set(BUNDLE_RESOURCES_DIR $<TARGET_BUNDLE_DIR:${APP_NAME}>)

        list(APPEND PLATFORM_LIBS
            -ObjC

            # GameController framework for gamepad input (JasperEngineGameController.mm)
            "-framework GameController"
        )

        if(JASPERENGINE_ADMOB)
            add_definitions("-DJASPERENGINE_ADMOB")

            list(APPEND PLATFORM_SOURCE
                ${PLATFORM_ROOT}/ios/AdMobAdapter.h
                ${PLATFORM_ROOT}/ios/AdMobAdapter.m
            )

            # To run on iOS simulator
            list(APPEND PLATFORM_LIBS
                ${PLATFORM_ROOT}/GoogleMobileAdsSdkiOS-10.14.0/FBLPromises.xcframework/ios-arm64_x86_64-simulator/FBLPromises.framework
                ${PLATFORM_ROOT}/GoogleMobileAdsSdkiOS-10.14.0/GoogleAppMeasurement.xcframework/ios-arm64_x86_64-simulator/GoogleAppMeasurement.framework
                ${PLATFORM_ROOT}/GoogleMobileAdsSdkiOS-10.14.0/GoogleAppMeasurementIdentitySupport.xcframework/ios-arm64_x86_64-simulator/GoogleAppMeasurementIdentitySupport.framework
                ${PLATFORM_ROOT}/GoogleMobileAdsSdkiOS-10.14.0/GoogleMobileAds.xcframework/ios-arm64_x86_64-simulator/GoogleMobileAds.framework
                ${PLATFORM_ROOT}/GoogleMobileAdsSdkiOS-10.14.0/GoogleUtilities.xcframework/ios-arm64_x86_64-simulator/GoogleUtilities.framework
                ${PLATFORM_ROOT}/GoogleMobileAdsSdkiOS-10.14.0/UserMessagingPlatform.xcframework/ios-arm64_x86_64-simulator/UserMessagingPlatform.framework
                ${PLATFORM_ROOT}/GoogleMobileAdsSdkiOS-10.14.0/nanopb.xcframework/ios-arm64_x86_64-simulator/nanopb.framework
            )
        endif()

        add_compile_options(-fmodules)

        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fobjc-arc")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fobjc-arc")
        # Xcode 27 starts at iOS 15
        set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")

        list(APPEND PLATFORM_PROPERTIES
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${APP_BUNDLE_IDENTIFIER}
            MACOSX_BUNDLE_INFO_PLIST "${JASPERENGINE_ROOT}/workspaces/xcode/ios/Info.plist"
            XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
        )
    endif()

    include_directories(${PLATFORM_ROOT})
    include_directories(${PROJECT_ROOT})
    include_directories(${PROJECT_ROOT}/scripts)

endif() # End standalone project setup

# =============================================================
# JASPERENGINE LIBRARY BUILD
# Always runs (both standalone and when included by the editor).
# =============================================================

jasperengine_check_cxx_toolchain()
# Checked after the project settings marker, so the value the Exporter injects is validated too
if(NOT JASPERENGINE_CXX_STANDARD IN_LIST JASPERENGINE_CXX_STANDARDS)
    string(REPLACE ";" ", " JASPERENGINE_CXX_STANDARDS_TEXT "${JASPERENGINE_CXX_STANDARDS}")
    message(FATAL_ERROR "JASPERENGINE_CXX_STANDARD must be one of: ${JASPERENGINE_CXX_STANDARDS_TEXT} (got '${JASPERENGINE_CXX_STANDARD}')")
endif()
message(STATUS "C++ standard is set to ${JASPERENGINE_CXX_STANDARD}")
# The default of every target created from here on, the vendored libraries included;
# jasperengine_apply_cxx_standard below overrides the ones that set their own
set(CMAKE_CXX_STANDARD ${JASPERENGINE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD ${JASPERENGINE_CXX_STANDARD})
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
# MSVC reports __cplusplus as 199711L without it, and header-only libraries (nlohmann json,
# LuaBridge3) would pick their pre-C++20 code paths
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
endif()

option(JASPERENGINE_SHARED "Build jasperengine as a shared library" OFF)

# Warnings (off here by default, so exports stay quiet) and unity builds of our own targets
include(${CMAKE_CURRENT_LIST_DIR}/cmake/JasperWarnings.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/JasperUnityBuild.cmake)

if(JASPERENGINE_SHARED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(JASPERENGINE_LIB_TYPE SHARED)
else()
    set(JASPERENGINE_LIB_TYPE STATIC)
endif()

if(WIN32)
    if(MSVC)
        add_compile_options(/bigobj)
        # (MSVC's AddressSanitizer needs the DLL runtime too, JasperSanitizers.cmake)
        if(JASPERENGINE_SHARED OR JASPERENGINE_SANITIZE_ADDRESS)
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        else()
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    else()
        add_compile_options(-Wa,-mbig-obj)
    endif()
endif()

include_directories(shaders)

include_directories(core)
include_directories(core/action)
include_directories(core/asset)
include_directories(core/buffer)
include_directories(core/component)
include_directories(core/ecs)
include_directories(core/io)
include_directories(core/io/watch)
include_directories(core/manager)
include_directories(core/math)
include_directories(core/object)
include_directories(core/object/sound)
include_directories(core/object/ui)
include_directories(core/object/environment)
include_directories(core/object/physics)
include_directories(core/pool)
include_directories(core/registry)
include_directories(core/render)
include_directories(core/script)
include_directories(core/shader)
include_directories(core/subsystem)
include_directories(core/texture)
# Keep util/ namespaced: Signal.h shadows <signal.h> on case-insensitive hosts.
include_directories(core/htmlui)

include_directories(renders)

include_directories(SYSTEM libs)
include_directories(SYSTEM libs/luabridge3)
include_directories(SYSTEM libs/json)

include_directories(SYSTEM libs/sokol)
include_directories(SYSTEM libs/lua)
include_directories(SYSTEM libs/stb)
include_directories(SYSTEM libs/soloud/include)
include_directories(SYSTEM libs/tinyobjloader)
include_directories(SYSTEM libs/tinygltf)
include_directories(SYSTEM libs/tinyxml2)

# Built after the standalone block, so it sees the physics switches the Exporter injects there
set(JASPERENGINE_PHYSICS_LIBS "")
if(JASPERENGINE_PHYSICS_2D)
    list(APPEND JASPERENGINE_PHYSICS_LIBS box2d)
    include_directories(SYSTEM libs/box2d/include)
endif()
if(JASPERENGINE_PHYSICS_3D)
    list(APPEND JASPERENGINE_PHYSICS_LIBS joltphysics)
    include_directories(SYSTEM libs/joltphysics)
    # Otherwise Jolt defines _HAS_EXCEPTIONS=0 PUBLIC, which breaks try/catch on MSVC
    set(CPP_EXCEPTIONS_ENABLED ON)
endif()

# Convex decomposition of Physics Placement (V-HACD, engine/libs/vhacd): the editor's, on desktop.
# Exported games (this file as the top-level build) and Web, Android and iOS builds keep the hull
# cache (ConvexDecompositionCache) but cannot decompose. After the standalone block: the platform
# is only known once project() has read the toolchain.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(JASPERENGINE_CONVEX_DECOMPOSITION_DEFAULT OFF)
else()
    set(JASPERENGINE_CONVEX_DECOMPOSITION_DEFAULT ON)
endif()
option(JASPERENGINE_CONVEX_DECOMPOSITION "Compile V-HACD convex decomposition (Physics Placement)" ${JASPERENGINE_CONVEX_DECOMPOSITION_DEFAULT})

include_directories(SYSTEM libs/harfbuzz)
include_directories(SYSTEM libs/sheenbidi/Headers)
include_directories(SYSTEM libs/meshoptimizer)
include_directories(SYSTEM libs/bcdec)

add_subdirectory(libs/sokol)
add_subdirectory(libs/lua)
add_subdirectory(libs/stb)
add_subdirectory(libs/soloud)
add_subdirectory(libs/tinyobjloader)
add_subdirectory(libs/tinygltf)
add_subdirectory(libs/tinyxml2)
foreach(lib IN LISTS JASPERENGINE_PHYSICS_LIBS)
    add_subdirectory(libs/${lib})
endforeach()
add_subdirectory(libs/harfbuzz)
add_subdirectory(libs/sheenbidi)
add_subdirectory(libs/meshoptimizer)
if(JASPERENGINE_NAVIGATION)
    add_subdirectory(libs/recastnavigation)
endif()
# Cooked textures (S14, docs/textures.md): Zstandard (zstd_decompress in every game, zstd for the
# cook), the Basis Universal transcoder of web textures (the web runtime, the editor's previews
# and the tests; native games never load Basis files) and, in the editor tree only
# (JASPERENGINE_TEXTURE_ENCODERS, set by the root CMakeLists), the encoders of the cook
add_subdirectory(libs/zstd)
if(EMSCRIPTEN OR JASPERENGINE_SHARED OR JASPERENGINE_TESTS)
    set(JASPERENGINE_BASIS_TRANSCODER_DEFAULT ON)
else()
    set(JASPERENGINE_BASIS_TRANSCODER_DEFAULT OFF)
endif()
option(JASPERENGINE_BASIS_TRANSCODER "Transcode Basis Universal KTX2 textures at load" ${JASPERENGINE_BASIS_TRANSCODER_DEFAULT})
add_subdirectory(libs/basisu)
if(JASPERENGINE_TEXTURE_ENCODERS)
    add_subdirectory(libs/bc7enc_rdo)
    add_subdirectory(libs/astcenc)
endif()
# The RmlUi debugger (Source/Debugger, UIDocumentSystem::setDebuggerVisible): the editor, Debug
# exports and the HTML UI tests have it; release games leave it out
if(JASPERENGINE_SHARED OR JASPERENGINE_HTMLUI_TESTS OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(JASPERENGINE_HTMLUI_DEBUGGER_DEFAULT ON)
else()
    set(JASPERENGINE_HTMLUI_DEBUGGER_DEFAULT OFF)
endif()
option(JASPERENGINE_HTMLUI_DEBUGGER "Compile the RmlUi debugger into the HTML UI" ${JASPERENGINE_HTMLUI_DEBUGGER_DEFAULT})
if(JASPERENGINE_HTMLUI_DEBUGGER)
    add_definitions(-DJASPERENGINE_HTMLUI_DEBUGGER)
endif()
# The profiler (core/profiler, renders/sokol pass recorder and VRAM tracker, spec S15): the
# editor, Debug builds, the unit tests and Development Build exports have it; release games
# leave it out, and its macros compile to nothing there (docs/profiling.md)
option(JASPERENGINE_DEVELOPMENT_BUILD "A Development Build: a Release game with the profiler and its --profile options" OFF)
if(JASPERENGINE_SHARED OR JASPERENGINE_TESTS OR JASPERENGINE_DEVELOPMENT_BUILD OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(JASPERENGINE_PROFILER_DEFAULT ON)
else()
    set(JASPERENGINE_PROFILER_DEFAULT OFF)
endif()
option(JASPERENGINE_PROFILER "Compile the CPU/GPU profiler, the frame pass recorder and the VRAM tracker" ${JASPERENGINE_PROFILER_DEFAULT})
# Tracy remote profiling of a running development game (desktop and Android, libs/tracy)
option(JASPERENGINE_TRACY "Send the profiler's zones, frames and plots to Tracy" OFF)
if(JASPERENGINE_TRACY)
    if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "JASPERENGINE_TRACY is for desktop and Android builds; web and iOS profile with capture files (docs/profiling.md)")
    endif()
    if(NOT JASPERENGINE_PROFILER)
        message(FATAL_ERROR "JASPERENGINE_TRACY needs JASPERENGINE_PROFILER")
    endif()
    add_subdirectory(libs/tracy)
endif()
add_subdirectory(libs/lunasvg)
add_subdirectory(libs/thorvg)
add_subdirectory(libs/rmlui)
# the HTML5 parser of the HTML UI compatibility layer (engine/core/htmlui/compat/html)
add_subdirectory(libs/lexbor)
# the HTML UI's <video>: AV1 (dav1d), Opus (libopus) and the WebM parser (libwebm)
add_subdirectory(libs/dav1d)
add_subdirectory(libs/opus)
add_subdirectory(libs/libwebm)

if(JASPERENGINE_RUNTIME_SHADERS)
    # A game only needs the back end of its own graphics API (GLSL is always in);
    # the editor exports for every API, and so do the HTML UI tests (HtmlUIShaderTest)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        if(GRAPHIC_BACKEND STREQUAL "d3d11" OR JASPERENGINE_HTMLUI_TESTS)
            set(SHADERCOMPILER_HLSL ON CACHE BOOL "" FORCE)
        else()
            set(SHADERCOMPILER_HLSL OFF CACHE BOOL "" FORCE)
        endif()
        if(GRAPHIC_BACKEND STREQUAL "metal" OR JASPERENGINE_HTMLUI_TESTS)
            set(SHADERCOMPILER_MSL ON CACHE BOOL "" FORCE)
        else()
            set(SHADERCOMPILER_MSL OFF CACHE BOOL "" FORCE)
        endif()
        # a game compiles few variants; the editor compiles every shader of an export
        set(SHADERCOMPILER_OPTIMIZE_SIZE ON CACHE BOOL "" FORCE)
    else()
        set(SHADERCOMPILER_HLSL ON CACHE BOOL "" FORCE)
        set(SHADERCOMPILER_MSL ON CACHE BOOL "" FORCE)
        set(SHADERCOMPILER_OPTIMIZE_SIZE OFF CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(libs/shadercompiler)
endif()

# Set again here because standalone builds skip the root CMakeLists
if(MSVC)
    set(EXTERNAL_WARNING_FLAG /w)
else()
    set(EXTERNAL_WARNING_FLAG -w)
endif()

set(JASPERENGINE_TEXTURE_LIBS zstd_decompress zstd basisu_transcoder)
if(JASPERENGINE_TEXTURE_ENCODERS)
    list(APPEND JASPERENGINE_TEXTURE_LIBS basisu_encoder bc7enc_rdo astcenc)
endif()

foreach(lib IN ITEMS
    sokol lua stb soloud tinyobjloader tinygltf tinyxml2
    harfbuzz sheenbidi meshoptimizer lunasvg thorvg rmlui lexbor
    dav1d dav1d_bitdepth_8 dav1d_bitdepth_16 opus webm ${JASPERENGINE_PHYSICS_LIBS} ${JASPERENGINE_TEXTURE_LIBS})
    target_compile_options(${lib} PRIVATE ${EXTERNAL_WARNING_FLAG})
endforeach()
if(JASPERENGINE_NAVIGATION)
    target_compile_options(recastnavigation PRIVATE ${EXTERNAL_WARNING_FLAG})
    target_compile_options(recastnavigation_debugdraw PRIVATE ${EXTERNAL_WARNING_FLAG})
    jasperengine_apply_cxx_standard(recastnavigation recastnavigation_debugdraw)
endif()

# One C++ standard for every object of the binary: libc++ template definitions instantiated
# at two standards across a library boundary (tinygltf and RmlUi pass std containers into
# engine code) broke Wasm signatures. Most of these set a standard of their own, or none.
jasperengine_apply_cxx_standard(
    sokol soloud tinyobjloader tinygltf tinyxml2 harfbuzz meshoptimizer
    lunasvg thorvg rmlui webm ${JASPERENGINE_PHYSICS_LIBS} basisu_transcoder)
if(JASPERENGINE_TEXTURE_ENCODERS)
    jasperengine_apply_cxx_standard(basisu_encoder bc7enc_rdo astcenc)
endif()
if(JASPERENGINE_RUNTIME_SHADERS)
    # shadercompiler and every glslang, SPIRV and spirv-cross target it creates
    jasperengine_apply_cxx_standard_in_directories(${CMAKE_CURRENT_SOURCE_DIR}/libs/shadercompiler)
endif()

if(JASPERENGINE_HTMLUI_TESTS)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        message(FATAL_ERROR "JASPERENGINE_HTMLUI_TESTS needs the standalone engine build (cmake -S engine): the editor's shared engine hides the RmlUi symbols the tests use")
    endif()
endif()
if((JASPERENGINE_HTMLUI_TESTS OR JASPERENGINE_TESTS) AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # the tests read render targets back (engine/tests/support/Readback.cpp)
    add_definitions(-DJASPERENGINE_TEXTURE_READBACK)
endif()

if(DEFINED NO_LUA_INIT)
    add_definitions("-DNO_LUA_INIT=${NO_LUA_INIT}")
    unset(NO_LUA_INIT CACHE)
endif()
if(DEFINED NO_CPP_INIT)
    add_definitions("-DNO_CPP_INIT=${NO_CPP_INIT}")
    unset(NO_CPP_INIT CACHE)
endif()

set(JASPERENGINE_SRCS
    core/Engine.cpp
    core/Input.cpp
    core/input/InputControlPath.cpp
    core/input/InputActionAsset.cpp
    core/input/InputActions.cpp
    core/input/InputRebind.cpp
    core/input/InputVirtualControls.cpp
    core/Log.cpp
    core/diag/CrashHandler.cpp
    core/diag/CrashHandlerPosix.cpp
    core/diag/CrashHandlerWin.cpp
    core/diag/HangStackCapture.cpp
    core/diag/CrashHandlerWeb.cpp
    core/diag/HangWatchdog.cpp
    core/log/Logger.cpp
    core/log/LogFormat.cpp
    core/log/Backtrace.cpp
    core/log/sinks/ConsoleSink.cpp
    core/log/sinks/RotatingFileSink.cpp
    core/log/sinks/MemorySink.cpp
    core/log/sinks/CallbackSink.cpp
    core/log/sinks/PlatformSink.cpp
    core/log/sinks/PlatformSinks.cpp
    core/log/sinks/AndroidLogSink.cpp
    core/log/sinks/EmscriptenConsoleSink.cpp
    core/log/sinks/DebuggerSink.cpp
    core/log/sinks/AppleLogSink.cpp
    core/Scene.cpp
    core/SceneSettings.cpp
    core/System.cpp
    core/action/Action.cpp
    core/action/AlphaTween.cpp
    core/action/Animation.cpp
    core/action/ColorTween.cpp
    core/action/PositionTween.cpp
    core/action/RotationTween.cpp
    core/action/ScaleTween.cpp
    core/action/SpriteAnimation.cpp
    core/action/Tween.cpp
    core/action/keyframe/KeyframeTracks.cpp
    core/action/keyframe/MorphTracks.cpp
    core/action/keyframe/LatticeTracks.cpp
    core/action/keyframe/RotateTracks.cpp
    core/action/keyframe/ScaleTracks.cpp
    core/action/keyframe/TranslateTracks.cpp
    core/buffer/Attribute.cpp
    core/buffer/Buffer.cpp
    core/buffer/ExternalBuffer.cpp
    core/buffer/IndexBuffer.cpp
    core/buffer/InterleavedBuffer.cpp
    core/ecs/ComponentManager.cpp
    core/ecs/ChangeTracking.cpp
    core/ecs/CommandBuffer.cpp
    core/ecs/SystemScheduler.cpp
    core/htmlui/HtmlUI.cpp
    core/htmlui/HtmlUIClock.cpp
    core/htmlui/HtmlUIContext.cpp
    core/htmlui/HtmlUIContextRender.cpp
    core/htmlui/HtmlUIDependencies.cpp
    core/htmlui/HtmlUITexture.cpp
    core/htmlui/RmlFile.cpp
    core/htmlui/RmlFontEffects.cpp
    core/htmlui/RmlFontEngine.cpp
    core/htmlui/RmlRenderer.cpp
    core/htmlui/RmlSystem.cpp
    core/htmlui/HtmlUIContextInput.cpp
    core/htmlui/RmlKeyMap.cpp
    core/htmlui/RmlTextInputHandler.cpp
    core/htmlui/anim/AnimAnimation.cpp
    core/htmlui/anim/AnimCommon.cpp
    core/htmlui/anim/AnimEasing.cpp
    core/htmlui/anim/AnimEngine.cpp
    core/htmlui/anim/AnimParams.cpp
    core/htmlui/anim/AnimProperty.cpp
    core/htmlui/anim/AnimSpring.cpp
    core/htmlui/anim/AnimTargets.cpp
    core/htmlui/anim/AnimTimeline.cpp
    core/htmlui/anim/AnimTimer.cpp
    core/htmlui/anim/AnimUtils.cpp
    core/htmlui/anim/AnimValue.cpp
    core/htmlui/anim/Anime.cpp
    core/htmlui/UIWorldRaycast.cpp
    core/htmlui/vector/DotLottie.cpp
    core/htmlui/vector/LottiePlayback.cpp
    core/htmlui/vector/UIElementLottie.cpp
    core/htmlui/vector/UILottieElement.cpp
    core/htmlui/media/Av1Decoder.cpp
    core/htmlui/media/UIElementVideo.cpp
    core/htmlui/media/OpusAudioDecoder.cpp
    core/htmlui/media/UIVideoControls.cpp
    core/htmlui/media/UIVideoElement.cpp
    core/htmlui/media/VideoAudio.cpp
    core/htmlui/media/VideoColorConvert.cpp
    core/htmlui/media/VideoDemuxer.cpp
    core/htmlui/media/VideoFormats.cpp
    core/htmlui/media/VideoPlayback.cpp
    core/htmlui/media/VideoStream.cpp
    core/htmlui/HtmlUIInspector.cpp
    core/htmlui/HtmlUIAssertions.cpp
    core/htmlui/effects/BuiltinEffectManifests.gen.cpp
    core/htmlui/effects/EffectCompat.cpp
    core/htmlui/effects/EffectInstancers.cpp
    core/htmlui/effects/EffectManifest.cpp
    core/htmlui/effects/EffectRegistry.cpp
    core/htmlui/effects/EffectRenderer.cpp
    core/htmlui/effects/EffectRuntime.cpp
    core/htmlui/effects/EffectSimulator.cpp
    core/htmlui/effects/EffectSyntax.cpp
    core/htmlui/effects/EffectValue.cpp
    core/asset/AssetMap.cpp
    core/asset/Guid.cpp
    core/graph/Graph.cpp
    core/graph/GraphRegistry.cpp
    core/io/AssetIOService.cpp
    core/io/Data.cpp
    core/io/File.cpp
    core/io/FileData.cpp
    core/io/ResourcePack.cpp
    core/io/watch/ExpectedWriteLedger.cpp
    core/io/watch/FileChangeCoalescer.cpp
    core/io/watch/FileIdentity.cpp
    core/io/watch/FileWatchService.cpp
    core/io/watch/FileWatcher.cpp
    core/io/watch/ProjectFileIndex.cpp
    core/io/watch/SharedFileReader.cpp
    core/io/watch/backends/FsEventsBackend.cpp
    core/io/watch/backends/InotifyBackend.cpp
    core/io/watch/backends/NullBackend.cpp
    core/io/watch/backends/PollingBackend.cpp
    core/io/watch/backends/RdcwBackend.cpp
    core/manager/ObjectManager.cpp
    core/manager/PrefabAsset.cpp
    core/manager/PrefabManager.cpp
    core/manager/SceneManager.cpp
    core/io/UserSettings.cpp
    core/io/AtomicFile.cpp
    core/io/ScopedFileLock.cpp
    core/math/AABB.cpp
    core/math/Curve.cpp
    core/math/Matrix3.cpp
    core/math/Matrix4.cpp
    core/math/OBB.cpp
    core/math/Plane.cpp
    core/math/Quaternion.cpp
    core/math/Ray.cpp
    core/math/Rect.cpp
    core/math/Sphere.cpp
    core/math/Vector2.cpp
    core/math/Vector3.cpp
    core/math/Vector4.cpp
    core/object/Bone.cpp
    core/object/Camera.cpp
    core/object/EntityHandle.cpp
    core/object/Light.cpp
    core/object/Light2D.cpp
    core/object/LineRenderer.cpp
    core/object/Mesh.cpp
    core/object/MeshPolygon.cpp
    core/object/Mirror.cpp
    core/object/FluidSimulation.cpp
    core/object/Lattice.cpp
    core/object/Model.cpp
    core/object/GameObject.cpp
    core/object/ShadowCaster2D.cpp
    core/object/Points.cpp
    core/object/Primitive.cpp
    core/object/Sprite.cpp
    core/object/Terrain.cpp
    core/object/Tilemap.cpp
    core/object/sound/AudioSource.cpp
    core/object/environment/Fog.cpp
    core/object/environment/LocalVolumetricFog.cpp
    core/object/environment/ReflectionProbe.cpp
    core/object/environment/Skybox.cpp
    core/object/environment/Water.cpp
    core/object/environment/WaterVolume.cpp
    core/object/physics/Buoyancy.cpp
    core/object/environment/VolumetricClouds.cpp
    core/object/ui/Button.cpp
    core/object/ui/LayoutGroup.cpp
    core/object/ui/Image.cpp
    core/object/ui/Panel.cpp
    core/object/ui/Polygon.cpp
    core/object/ui/Scrollbar.cpp
    core/object/ui/Progressbar.cpp
    core/object/ui/Text.cpp
    core/object/ui/InputField.cpp
    core/object/ui/RectTransform.cpp
    core/object/ui/UIDocument.cpp
    core/object/ui/UIElement.cpp
    core/object/ui/UIEvent.cpp
    core/object/ui/UIDataModel.cpp
    core/object/ui/UIValue.cpp
    core/object/ui/HtmlUIEventListener.cpp
    core/object/ui/HtmlUIValueDefinition.cpp
    core/pool/FontPool.cpp
    core/pool/ShaderPool.cpp
    core/pool/SoundPool.cpp
    core/pool/TextureDataPool.cpp
    core/pool/TexturePool.cpp
    core/pool/ModelPool.cpp
    core/pool/MeshLodPool.cpp
    core/registry/EntityCloner.cpp
    core/registry/EntityRegistry.cpp
    core/registry/TagManager.cpp
    core/registry/PhysicsLayers.cpp
    core/physics/ConvexDecomposition.cpp
    core/physics/JoltShapeBuilder.cpp
    core/physics/PlacementWorld.cpp
    core/render/AtmosphereMath.cpp
    core/render/AtmosphereNoise.cpp
    core/render/AtmospherePresets.cpp
    core/render/AtmosphereQuality.cpp
    core/render/BufferRender.cpp
    core/render/CameraRender.cpp
    core/render/DynamicResolutionController.cpp
    core/render/FramebufferRender.cpp
    core/render/ObjectRender.cpp
    core/render/ShaderRender.cpp
    core/render/SystemRender.cpp
    core/render/TextureRender.cpp
    core/render/shadow/CascadeFit.cpp
    core/render/shadow/ShadowCache.cpp
    core/render/shadow/ShadowCasterRegistry.cpp
    core/render/shadow/ShadowSlotPlanner.cpp
    core/render/shadow/ShadowTiers.cpp
    core/render/shadow/ShadowTypes.cpp
    core/script/LuaBinding.cpp
    core/script/LuaFunctionBase.cpp
    core/script/LuaWatchdog.cpp
    core/script/LuaReload.cpp
    core/script/LuaScript.cpp
    core/script/ScriptBase.cpp
    core/script/ScriptProperty.cpp
    core/script/ToolchainFingerprint.cpp
    core/script/binding/ActionClassesLua.cpp
    core/script/binding/CoreClassesLua.cpp
    core/script/binding/CrashClassesLua.cpp
    core/script/binding/ECSClassesLua.cpp
    core/script/binding/IOClassesLua.cpp
    core/script/binding/MathClassesLua.cpp
    core/script/binding/GameObjectClassesLua.cpp
    core/script/binding/ProfilerClassesLua.cpp
    core/script/binding/GameplayClassesLua.cpp
    core/script/binding/ThreadClassesLua.cpp
    core/script/binding/HtmlUIVectorLua.cpp
    core/script/binding/HtmlUIMediaLua.cpp
    core/script/binding/HtmlUIWidgetsLua.cpp
    core/script/binding/UtilClassesLua.cpp
    core/script/binding/HtmlUIClassesLua.cpp
    core/script/binding/HtmlUIAnimeLua.cpp
    core/shader/RuntimeShaderCompiler.cpp
    core/shader/ShaderData.cpp
    core/shader/ShaderDataSerializer.cpp
    core/shader/ShaderManifest.cpp
    core/shader/ShaderUniforms.cpp
    core/shader/ShaderVariantCollection.cpp
    core/shader/ShaderVariantCompiler.cpp
    core/subsystem/ActionSystem.cpp
    core/subsystem/FluidSystem.cpp
    core/fluid/FluidSimulator.cpp
    core/fluid/FluidPresets.cpp
    core/subsystem/AudioSystem.cpp
    core/subsystem/MeshSystem.cpp
    core/subsystem/LatticeSystem.cpp
    core/lattice/LatticeSolver.cpp
    core/lattice/LatticePresets.cpp
    core/lattice/LatticeRender.cpp
    core/subsystem/MeshSystemGltfPbr.cpp
    core/subsystem/PhysicsSystem.cpp
    core/subsystem/WaterSystem.cpp
    core/subsystem/AtmosphereRenderer.cpp
    core/subsystem/RenderSystem.cpp
    core/subsystem/RenderSystemShadows.cpp
    core/subsystem/UISystem.cpp
    core/subsystem/UIDocumentSystem.cpp
    core/subsystem/UIDocumentSystemRender.cpp
    core/subsystem/UIDocumentSystemInput.cpp
    core/subsystem/UIDocumentSystemScript.cpp
    core/subsystem/UIDocumentSystemPatch.cpp
    core/subsystem/UIDocumentSystemReload.cpp
    core/subsystem/UIDocumentSystemCompat.cpp
    core/subsystem/UIDocumentSystemTexture.cpp
    core/texture/BasisTranscode.cpp
    core/texture/CookedTextures.cpp
    core/texture/Framebuffer.cpp
    core/texture/IBLEnvironment.cpp
    core/texture/Ktx2.cpp
    core/texture/Ktx2Write.cpp
    core/texture/MipChain.cpp
    core/texture/MaterialPbr.cpp
    core/texture/TexturePack.cpp
    core/texture/Texture.cpp
    core/texture/TextureData.cpp
    core/texture/TextureDecode.cpp
    core/texture/TextureFallback.cpp
    core/texture/TextureFormat.cpp
    core/texture/TexturePrepare.cpp
    core/jobs/BackgroundGate.cpp
    core/jobs/JobCounter.cpp
    core/jobs/JobSystem.cpp
    core/jobs/ParallelFor.cpp
    core/jobs/Primitives.cpp
    core/jobs/SerialQueue.cpp
    core/jobs/SlabPool.cpp
    core/profiler/EngineProfiling.cpp
    core/profiler/FrameTimes.cpp
    core/profiler/GpuMemory.cpp
    core/profiler/Profiler.cpp
    core/memory/Arena.cpp
    core/memory/MemoryStats.cpp
    core/memory/ObjectPool.cpp
    core/memory/RangeAllocator.cpp
    core/memory/ScratchScope.cpp
    core/thread/ResourceProgress.cpp
    core/thread/ThreadUtil.cpp
    core/util/Angle.cpp
    core/util/Base64.cpp
    core/util/Color.cpp
    core/util/CrashHook.cpp
    core/util/CubeLut.cpp
    core/util/FontFace.cpp
    core/util/FontRegistry.cpp
    core/util/GlyphAtlas.cpp
    core/util/JoltJobAdapter.cpp
    core/util/LunaSVGRuntime.cpp
    core/util/PostColorLut.cpp
    core/util/Scheduler.cpp
    core/util/STBText.cpp
    core/util/StringUtils.cpp
    core/util/SubscriptionOwnerIndex.cpp
    core/util/TextShaper.cpp
    core/util/ThorVGRuntime.cpp
    core/util/UniqueToken.cpp
    core/util/XMLUtils.cpp
)

# HTML/CSS compatibility layer (phase 2): globbed per directory
include(core/htmlui/compat/CompatSources.cmake)
list(APPEND JASPERENGINE_SRCS ${HTMLUI_COMPAT_SRCS})
# HTML UI widgets (S17): virtual views, tooltips, drag and drop, Markdown
include(core/htmlui/widgets/WidgetsSources.cmake)
list(APPEND JASPERENGINE_SRCS ${HTMLUI_WIDGETS_SRCS})
# Particles (S20): the SIMD kernels' flags, the subsystem and its drawing
include(core/particles/ParticleSources.cmake)
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_PARTICLE_SRCS} ${JASPERENGINE_PARTICLE_SYSTEM_SRCS})
# Navigation (S19): navmeshes, crowds, grids, the NavigationSystem and its Lua objects
include(core/navigation/NavigationSources.cmake)
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_NAVIGATION_SRCS})
include(libs/lucide/lucide.cmake)

if(JASPERENGINE_PHYSICS_2D)
    list(APPEND JASPERENGINE_SRCS
        core/object/physics/Rigidbody2D.cpp
        core/object/physics/Contact2D.cpp
        core/object/physics/Joint2D.cpp
        core/object/physics/Manifold2D.cpp
        core/object/physics/CharacterController2D.cpp
        core/subsystem/CharacterController2DSystem.cpp
    )
endif()

if(JASPERENGINE_PHYSICS_3D)
    list(APPEND JASPERENGINE_SRCS
        core/object/physics/Rigidbody.cpp
        core/object/physics/CollideShapeResult3D.cpp
        core/object/physics/Contact3D.cpp
        core/object/physics/Joint3D.cpp
        core/object/physics/CharacterController.cpp
        core/subsystem/CharacterControllerSystem.cpp
    )
endif()

set(RENDER_SOKOL_SRC
    renders/sokol/SokolBuffer.cpp
    renders/sokol/SokolTexture.cpp
    renders/sokol/SokolSystem.cpp
    renders/sokol/SokolCamera.cpp
    renders/sokol/SokolObject.cpp
    renders/sokol/SokolPipelineCache.cpp
    renders/sokol/SokolShader.cpp
    renders/sokol/SokolCmdQueue.cpp
    renders/sokol/SokolFramebuffer.cpp
    renders/sokol/SokolPassPreview.cpp
    renders/sokol/SokolPassRecorder.cpp
    renders/sokol/SokolTraceHub.cpp
    renders/sokol/SokolVramTracker.cpp
)

# The engine build a C++ script library must be made for (ToolchainFingerprint.h): the
# editor's version string, on this one file only so a new commit does not rebuild the engine
if(DEFINED JASPERENGINE_EDITOR_VERSION)
    set_source_files_properties(core/script/ToolchainFingerprint.cpp PROPERTIES
        COMPILE_DEFINITIONS "JASPERENGINE_ABI_VERSION=\"${JASPERENGINE_EDITOR_VERSION}\""
        SKIP_UNITY_BUILD_INCLUSION ON)
endif()

add_library(
    jasperengine

    ${JASPERENGINE_LIB_TYPE}

    ${RENDER_SOKOL_SRC}
    ${JASPERENGINE_SRCS}
)

# V-HACD is compiled in this one file (ENABLE_VHACD_IMPLEMENTATION): the define and the include
# path stay on it, so no other object's command line changes with the switch
if(JASPERENGINE_CONVEX_DECOMPOSITION)
    set_source_files_properties(core/physics/ConvexDecomposition.cpp PROPERTIES
        COMPILE_DEFINITIONS JASPERENGINE_CONVEX_DECOMPOSITION
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/libs/vhacd
    )
endif()

jasperengine_htmlui_compat_embed(jasperengine)
jasperengine_htmlui_lucide_embed(jasperengine)
jasperengine_target_warnings(jasperengine)
# Unity build: the HTML UI sources keep generic file-local helpers (lower, trim, startsWith, PI,
# Registry, Cache, ...) in anonymous namespaces at global scope, one helper set per file
get_target_property(JASPERENGINE_UNITY_SKIP jasperengine SOURCES)
list(FILTER JASPERENGINE_UNITY_SKIP INCLUDE REGEX "core/htmlui/")
jasperengine_skip_unity_build(
    REASON "the HTML UI subsystem: per-file helper sets with the same names"
    SOURCES ${JASPERENGINE_UNITY_SKIP})
jasperengine_skip_unity_build(
    REASON "the particle kernels build with their own floating-point flags"
    SOURCES ${JASPERENGINE_PARTICLE_SRCS})
jasperengine_skip_unity_build(
    REASON "the navigation sources: file-local Detour helpers and constants with the same names"
    SOURCES ${JASPERENGINE_NAVIGATION_SRCS})
jasperengine_target_unity_build(jasperengine)

target_compile_definitions(jasperengine
    PUBLIC
        $<$<BOOL:${JASPERENGINE_PHYSICS_2D}>:JASPERENGINE_PHYSICS_2D>
        $<$<BOOL:${JASPERENGINE_PHYSICS_3D}>:JASPERENGINE_PHYSICS_3D>
        JASPERENGINE_PROFILER=$<BOOL:${JASPERENGINE_PROFILER}>
        JASPERENGINE_DEVELOPMENT_BUILD=$<OR:$<BOOL:${JASPERENGINE_DEVELOPMENT_BUILD}>,$<CONFIG:Debug>>
        JASPERENGINE_TRACY=$<BOOL:${JASPERENGINE_TRACY}>
        JASPERENGINE_NAVIGATION=$<BOOL:${JASPERENGINE_NAVIGATION}>
    PRIVATE
        $<$<BOOL:${JE_JOLT_OWN_POOL}>:JE_JOLT_OWN_POOL>
)
if(JASPERENGINE_TRACY)
    target_link_libraries(jasperengine PUBLIC TracyClient)
endif()

find_package(Threads REQUIRED)
target_link_libraries(jasperengine PRIVATE Threads::Threads)

# the file watcher's FSEvents backend (core/io/watch/backends/FsEventsBackend.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(jasperengine PUBLIC "-framework CoreServices")
endif()

# the logger's backtraces (core/log/Backtrace.cpp): DbgHelp symbols on Windows, dladdr elsewhere
if(WIN32)
    target_link_libraries(jasperengine PRIVATE dbghelp)
elseif(ANDROID)
    target_link_libraries(jasperengine PRIVATE log)
elseif(NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(jasperengine PRIVATE ${CMAKE_DL_LIBS})
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(jasperengine PRIVATE -pthread)
endif()

target_link_libraries(
    jasperengine

    PUBLIC

    sokol lua stb tinyobjloader tinygltf tinyxml2 soloud harfbuzz sheenbidi meshoptimizer lunasvg thorvg rmlui lexbor
    dav1d opus webm zstd_decompress
    ${JASPERENGINE_PHYSICS_LIBS}
)
if(JASPERENGINE_NAVIGATION)
    target_link_libraries(jasperengine PRIVATE recastnavigation)
endif()
if(JASPERENGINE_BASIS_TRANSCODER)
    target_link_libraries(jasperengine PUBLIC basisu_transcoder)
    target_compile_definitions(jasperengine PUBLIC JASPERENGINE_BASIS_TRANSCODER)
endif()
# the mip chain and the import processing: no fused multiply-adds, so the levels (and the cook
# cache keys built on them) are the same bytes on every CPU, like stb_image_resize2's
if(NOT MSVC)
    set_source_files_properties(core/texture/MipChain.cpp core/texture/TexturePrepare.cpp
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
# the cooked-texture runtime the web downloads, optimised for size there like the Basis
# transcoder (spec S14 R21): its hot loops are the transcoder's and Zstandard's
if(EMSCRIPTEN)
    set_source_files_properties(core/texture/Ktx2.cpp core/texture/TextureDecode.cpp core/texture/TextureFallback.cpp
        core/texture/TextureFormat.cpp core/texture/CookedTextures.cpp core/texture/BasisTranscode.cpp
        PROPERTIES COMPILE_OPTIONS "-Os")
endif()

# The manifests of the library shaders (shaderlib/library/), embedded in every build: a
# game reads their properties, textures and render state even without the runtime
# compiler. Ahead of shaders/, whose stand-in the Xcode workspace uses.
set(JASPERENGINE_SHADERLIB_MANIFESTS_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/shaderlib_manifests.h)
file(GLOB_RECURSE JASPERENGINE_SHADERLIB_LIBRARY_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/shaderlib/library/*.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaderlib/library/*.frag
)
add_custom_command(
    OUTPUT ${JASPERENGINE_SHADERLIB_MANIFESTS_HEADER}
    COMMAND ${CMAKE_COMMAND}
            -DSHADERLIB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/shaderlib
            -DOUTPUT=${JASPERENGINE_SHADERLIB_MANIFESTS_HEADER}
            -DMANIFESTS=ON
            -P ${CMAKE_CURRENT_SOURCE_DIR}/libs/shadercompiler/embed_shaderlib.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libs/shadercompiler/embed_shaderlib.cmake ${JASPERENGINE_SHADERLIB_LIBRARY_FILES}
    COMMENT "Embedding the library shader manifests"
)
target_sources(jasperengine PRIVATE ${JASPERENGINE_SHADERLIB_MANIFESTS_HEADER})
target_include_directories(jasperengine BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

if(JASPERENGINE_RUNTIME_SHADERS)
    # The engine shader library (shaderlib/), embedded as the sources the runtime
    # compiler builds variants from. Ahead of shaders/, which holds a stand-in for
    # builds that cannot generate it (the Xcode workspace).
    set(JASPERENGINE_SHADERLIB_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/shaderlib.h)
    set(JASPERENGINE_EMBED_SHADERLIB ${CMAKE_CURRENT_SOURCE_DIR}/libs/shadercompiler/embed_shaderlib.cmake)
    file(GLOB_RECURSE JASPERENGINE_SHADERLIB_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/shaderlib/*.glsl
        ${CMAKE_CURRENT_SOURCE_DIR}/shaderlib/*.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shaderlib/*.frag
    )
    add_custom_command(
        OUTPUT ${JASPERENGINE_SHADERLIB_HEADER}
        COMMAND ${CMAKE_COMMAND}
                -DSHADERLIB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/shaderlib
                -DOUTPUT=${JASPERENGINE_SHADERLIB_HEADER}
                -P ${JASPERENGINE_EMBED_SHADERLIB}
        DEPENDS ${JASPERENGINE_EMBED_SHADERLIB} ${JASPERENGINE_SHADERLIB_FILES}
        COMMENT "Embedding the engine shader library"
    )
    target_sources(jasperengine PRIVATE ${JASPERENGINE_SHADERLIB_HEADER})
    target_include_directories(jasperengine BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(jasperengine PRIVATE JASPERENGINE_RUNTIME_SHADERS)
    target_link_libraries(jasperengine PRIVATE shadercompiler)
endif()

set_target_properties(
    jasperengine

    PROPERTIES

    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    OBJCXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    OBJCXX_STANDARD_REQUIRED ON
)

if(JASPERENGINE_SHARED)
    target_compile_definitions(jasperengine 
        PRIVATE 
        JASPERENGINE_EXPORTS
        PUBLIC
        JASPERENGINE_SHARED
    )
    # RmlUi is built with hidden visibility, but the std templates it instantiates over its
    # own enums keep default visibility; nothing of librmlui.a belongs in the dynamic symbol
    # table. GNU ld and lld only: MSVC exports dllexport symbols alone, and ld64 has no
    # equivalent flag (those weak std instantiations stay visible in the macOS dylib)
    if(NOT APPLE AND NOT MSVC)
        target_link_options(jasperengine PRIVATE "LINKER:--exclude-libs,librmlui.a")
        # the vector libraries (hidden visibility too): LunaSVG/plutovg for SVG, ThorVG for Lottie
        target_link_options(jasperengine PRIVATE "LINKER:--exclude-libs,liblunasvg.a:libthorvg.a")
        # the HTML parser of the compat layer (hidden visibility too; C, so lxb_*/lexbor_* symbols)
        target_link_options(jasperengine PRIVATE "LINKER:--exclude-libs,liblexbor.a")
        # the <video> decoders (hidden visibility too: dav1d_*, opus_*, mkvparser::)
        target_link_options(jasperengine PRIVATE "LINKER:--exclude-libs,libdav1d.a:libopus.a:libwebm.a")
        # the texture decoders (hidden visibility too: ZSTD_*, basist::); the editor links its own
        # zstd and basisu encoder
        target_link_options(jasperengine PRIVATE "LINKER:--exclude-libs,libzstd_decompress.a:libbasisu_transcoder.a")
        # and what engine code instantiates over RmlUi types (see the script; ELF only)
        if(NOT WIN32)
            target_link_options(jasperengine PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libs/rmlui/hidden-symbols.map")
            set_property(TARGET jasperengine APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/libs/rmlui/hidden-symbols.map")
        endif()
    endif()
endif()

# =============================================================
# STANDALONE EXECUTABLE BUILD
# Only runs when this file is used as the top-level build.
# =============================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)

    file(GLOB_RECURSE PROJECT_SOURCE ${PROJECT_ROOT}/*.cpp)

    # Embed the exported icon into the Windows executable (Explorer, taskbar
    # and window icon). Other platforms set the window icon at runtime instead.
    if(WIN32 AND EXISTS "${PROJECT_ROOT}/app_icon.rc")
        list(APPEND PLATFORM_SOURCE ${PROJECT_ROOT}/app_icon.rc)
    endif()

    set(all_code_files
        ${PROJECT_SOURCE}
        ${PLATFORM_SOURCE}
        ${PLATFORM_RESOURCES}
    )

    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
        add_executable(
            ${APP_NAME}
            ${PLATFORM_EXEC_FLAGS}
            ${all_code_files}
        )
    else()
        add_library(
            ${APP_NAME}
            SHARED
            ${all_code_files}
        )
    endif()

    if(WIN32)
        include(${JASPERENGINE_ROOT}/platform/win/WindowsManifest.cmake)
        jasperengine_add_windows_manifest(${APP_NAME})
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        jasperengine_skip_unity_build(
            REASON "Xlib.h defines None, Bool, Status, Always... as macros, which break every later source of the batch"
            SOURCES ${JASPERENGINE_ROOT}/platform/linux/JasperEngineLinux.cpp ${JASPERENGINE_ROOT}/platform/linux/WindowLinux.cpp
                    ${JASPERENGINE_ROOT}/platform/linux/LinuxInputRouter.cpp ${JASPERENGINE_ROOT}/platform/linux/SystemLinux.cpp)
    endif()
    jasperengine_target_warnings(${APP_NAME})
    jasperengine_target_unity_build(${APP_NAME})
    jasperengine_stage_asan_runtime(${APP_NAME})

    # Project script roots are injected here by the editor Exporter. They are
    # scoped to this target: include_directories() would reach the jasperengine
    # library too, ahead of core/, letting a project header shadow an engine one.
    # @JASPERENGINE_SCRIPT_DIRS@

    if(DEFINED ASSETS_DEST_DIR OR DEFINED LUA_DEST_DIR)
        install(TARGETS ${APP_NAME} RUNTIME DESTINATION bin)
    endif()

    if(DEFINED ASSETS_DEST_DIR)
        # a native build keeps the cooked texture family its platform loads (S14 R9)
        set(JE_COOKED_DROP)
        foreach(family mobile android_etc2 web)
            list(APPEND JE_COOKED_DROP COMMAND "${CMAKE_COMMAND}" -E remove_directory ${ASSETS_DEST_DIR}/je-cooked/${family})
        endforeach()
        add_custom_command(
            TARGET ${APP_NAME} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E remove_directory ${ASSETS_DEST_DIR}
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_ROOT}/assets ${ASSETS_DEST_DIR}
            ${JE_COOKED_DROP}
        )
        install(DIRECTORY ${ASSETS_DEST_DIR} DESTINATION bin)
    endif()

    if(DEFINED LUA_DEST_DIR)
        add_custom_command(
            TARGET ${APP_NAME} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E remove_directory ${LUA_DEST_DIR}
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_ROOT}/lua ${LUA_DEST_DIR}
        )
        install(DIRECTORY ${LUA_DEST_DIR} DESTINATION bin)
    endif()

    # Apple apps read the assets and scripts from inside their bundle
    if(DEFINED BUNDLE_RESOURCES_DIR)
        foreach(PROJECT_CONTENT assets lua)
            if(EXISTS "${PROJECT_ROOT}/${PROJECT_CONTENT}")
                add_custom_command(
                    TARGET ${APP_NAME} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E remove_directory ${BUNDLE_RESOURCES_DIR}/${PROJECT_CONTENT}
                    COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_ROOT}/${PROJECT_CONTENT} ${BUNDLE_RESOURCES_DIR}/${PROJECT_CONTENT}
                )
            endif()
        endforeach()
    endif()

    set_target_properties(
        ${APP_NAME}

        PROPERTIES
        ${PLATFORM_PROPERTIES}
        RESOURCE "${PLATFORM_RESOURCES}"
        CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        OBJCXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
        OBJCXX_STANDARD_REQUIRED ON
    )

    # emcc packs project/assets and project/lua into the .data file at link time
    # (--preload-file), which ninja does not see: list them as link inputs so an
    # edited script or asset relinks instead of shipping the previous bundle (appended after
    # PLATFORM_PROPERTIES, whose LINK_DEPENDS of the crash pre-js would replace them)
    if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        set(EM_PRELOAD_DEPENDS)
        foreach(preloadDir assets lua)
            if(EXISTS "${PROJECT_ROOT}/${preloadDir}")
                file(GLOB_RECURSE preloadFiles CONFIGURE_DEPENDS "${PROJECT_ROOT}/${preloadDir}/*")
                list(APPEND EM_PRELOAD_DEPENDS ${preloadFiles})
            endif()
        endforeach()
        if(EM_PRELOAD_DEPENDS)
            set_property(TARGET ${APP_NAME} APPEND PROPERTY LINK_DEPENDS ${EM_PRELOAD_DEPENDS})
        endif()
    endif()

    # Generated sources embed UTF-8 literals and carry no BOM, so MSVC would
    # otherwise decode them in the active code page.
    target_compile_options(
        ${APP_NAME}

        PUBLIC

        ${PLATFORM_OPTIONS}
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )

    target_link_libraries(
        ${APP_NAME}

        PRIVATE

        jasperengine
        Threads::Threads
        ${PLATFORM_LIBS}
    )

    # Optional user build file; the editor writes the same hook, with the same
    # variable names, into the project CMakeLists.txt it generates.
    set(JASPERENGINE_TARGET ${APP_NAME})
    set(JASPERENGINE_SCRIPTS_DIR ${PROJECT_ROOT}/scripts)
    include(${PROJECT_ROOT}/ProjectBuild.cmake OPTIONAL)

endif() # End standalone executable build

# googletest targets of the standalone engine tree (engine/cmake/JasperTesting.cmake)
if((JASPERENGINE_HTMLUI_TESTS OR JASPERENGINE_TESTS) AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/JasperTesting.cmake)
endif()
if(JASPERENGINE_HTMLUI_TESTS)
    add_subdirectory(tests/htmlui)
endif()
if(JASPERENGINE_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(tests/core)
endif()

if(JASPERENGINE_ECS_BENCH)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../tools/performance/EcsBench.cmake)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    jasperengine_check_cxx_standards()
endif()
