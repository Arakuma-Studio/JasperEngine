# The C++ standard of the engine, the editor and the exported game, and the source of
# cxxStandards in the editor (JASPERENGINE_CXX_STANDARD_LIST); Play plugins read the project
# setting (editor/Generator.cpp). The first entry is the default.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/JasperEngineToolchain.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/JasperEnginePaths.cmake)
# the engine's own folder in globs (an export's copy of it may sit below any path)
jasperengine_glob_escape(JASPERENGINE_SOURCE_GLOB "${CMAKE_CURRENT_LIST_DIR}")
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
# Plain http:// to hosts other than localhost (the export sets it from player.allow_insecure_http):
# App Transport Security of bare macOS executables; Http itself enforces the policy everywhere
option(JASPERENGINE_ALLOW_INSECURE_HTTP "Allow plain http:// in Apple's App Transport Security" OFF)
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
# The sound layer (S34 module soundlayer, specs S36 and S37): Sound Events, triggers, free voices and
# what a clip's import settings do at run time. A Compile build leaves it out when its project does
# not use it.
option(JASPERENGINE_SOUND_LAYER "Compile Sound Events, triggers, free voices and clip import settings" ON)
# The HTML/CSS UI (UI Document components, their Lua API, anime, widgets, effects, the on-screen
# controls: RmlUi, lexbor, ThorVG, msdfgen, the Lucide icons) and its <video> (dav1d, Opus,
# libwebm). A player variant or a Compile build leaves out what its project cannot reach
# (docs/specs/S34-player-size.md); the editor, the tests and the full player have both.
option(JASPERENGINE_HTMLUI "Compile the HTML UI" ON)
option(JASPERENGINE_VIDEO "Compile the HTML UI's <video> (needs JASPERENGINE_HTMLUI)" ON)

# =============================================================
# STANDALONE PROJECT SETUP
# Only runs when this file is used as the top-level build
# (i.e. building jasperengine-project outside the editor).
# =============================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)

    # The prebuilt player (docs/specs/S30-shipping.md R1): the engine with engine/player as its
    # project, which runs any game's cooked content (game.json, scenes/*.jscn) instead of
    # compiled-in scenes
    option(JASPERENGINE_PLAYER "Build jasperengine-player, the prebuilt player of shipped games" OFF)
    if(JASPERENGINE_PLAYER)
        set(APP_NAME jasperengine-player)
        set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/player)
    endif()

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

    # The settings below reach the game executable only (JASPERENGINE_APP_DEFINITIONS, its platform
    # main and the project's sources), which hands them to the engine at start through
    # PlatformConfig (platform/common/PlatformDefaults.h): the engine library and the vendored
    # libraries compile the same for every project, so a settings change recompiles a few units,
    # and a prebuilt engine package serves every project of one configuration
    # (docs/specs/S33-build-speed.md R5)
    set(JASPERENGINE_APP_DEFINITIONS "")
    if(NOT DEFINED JASPERENGINE_VSYNC_ENABLED)
        set(JASPERENGINE_VSYNC_ENABLED ON)
    endif()
    if(JASPERENGINE_VSYNC_ENABLED)
        set(JASPERENGINE_VSYNC_ENABLED_VALUE 1)
    else()
        set(JASPERENGINE_VSYNC_ENABLED_VALUE 0)
    endif()
    list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_VSYNC_ENABLED=${JASPERENGINE_VSYNC_ENABLED_VALUE}")

    # Player > Logging (the Exporter sets them): the game's log level, player.log, backtraces
    if(DEFINED JASPERENGINE_LOG_LEVEL)
        list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_LOG_LEVEL=\"${JASPERENGINE_LOG_LEVEL}\"")
    endif()
    if(DEFINED JASPERENGINE_LOG_FILE)
        if(JASPERENGINE_LOG_FILE)
            list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_LOG_FILE=1")
        else()
            list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_LOG_FILE=0")
        endif()
    endif()
    if(DEFINED JASPERENGINE_LOG_BACKTRACE)
        if(JASPERENGINE_LOG_BACKTRACE)
            list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_LOG_BACKTRACE=1")
        else()
            list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_LOG_BACKTRACE=0")
        endif()
    endif()

    # Player > Crash Handling (the Exporter sets them; PlatformConfig.h has the defaults). Whether
    # the crash handler and the hang watchdog are compiled in at all stays a definition of the
    # whole build (core/Engine.cpp leaves their code out when off).
    foreach(crashSetting JASPERENGINE_CRASH_HANDLER JASPERENGINE_CRASH_HANG_WATCHDOG)
        if(DEFINED ${crashSetting})
            if(${crashSetting})
                add_definitions("-D${crashSetting}=1")
            else()
                add_definitions("-D${crashSetting}=0")
            endif()
        endif()
    endforeach()
    foreach(crashSetting JASPERENGINE_CRASH_LOG_TAIL JASPERENGINE_CRASH_DUMP_WITH_HEAP JASPERENGINE_CRASH_WEB_OVERLAY)
        if(DEFINED ${crashSetting})
            if(${crashSetting})
                list(APPEND JASPERENGINE_APP_DEFINITIONS "${crashSetting}=1")
            else()
                list(APPEND JASPERENGINE_APP_DEFINITIONS "${crashSetting}=0")
            endif()
        endif()
    endforeach()
    if(DEFINED JASPERENGINE_CRASH_HANG_MS)
        list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_CRASH_HANG_MS=${JASPERENGINE_CRASH_HANG_MS}")
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
    list(APPEND JASPERENGINE_APP_DEFINITIONS "DEFAULT_WINDOW_WIDTH=${JASPERENGINE_WINDOW_WIDTH}")
    list(APPEND JASPERENGINE_APP_DEFINITIONS "DEFAULT_WINDOW_HEIGHT=${JASPERENGINE_WINDOW_HEIGHT}")
    list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_WINDOW_MODE=${JASPERENGINE_WINDOW_MODE}")
    list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_WINDOW_RESIZABLE=${JASPERENGINE_WINDOW_RESIZABLE_VALUE}")
    list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_WINDOW_TITLE=\"${JASPERENGINE_WINDOW_TITLE}\"")
    list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_STORAGE_ID=\"${JASPERENGINE_STORAGE_ID}\"")
    # Only an editor export sets it: desktop builds then save per user
    # (~/.local/share/<id>, %APPDATA%\<id>, ~/Library/Application Support/<id>) instead of in
    # the working directory
    if(DEFINED JASPERENGINE_APP_IDENTIFIER AND NOT JASPERENGINE_APP_IDENTIFIER STREQUAL "")
        list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_APP_IDENTIFIER=\"${JASPERENGINE_APP_IDENTIFIER}\"")
    endif()
    # X11 class / Wayland app_id (APP_NAME is a plain identifier, safe to quote).
    # Desktop environments match it against <APP_NAME>.desktop for the app icon.
    list(APPEND JASPERENGINE_APP_DEFINITIONS "JASPERENGINE_APP_ID=\"${APP_NAME}\"")
    if(JASPERENGINE_WINDOW_ICON AND EXISTS "${PROJECT_ROOT}/app_icon.h")
        list(APPEND JASPERENGINE_APP_DEFINITIONS JASPERENGINE_WINDOW_ICON)
    endif()

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

    if(JASPERENGINE_PLAYER)
        # <platform>-<arch>-<backend>-<flavour> (R8): content-manifest.json names the player it
        # was built for, and the player refuses other content (exit 5)
        if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            set(JE_PLAYER_PLATFORM windows)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(JE_PLAYER_PLATFORM macos)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(JE_PLAYER_PLATFORM ios)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
            set(JE_PLAYER_PLATFORM android)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
            set(JE_PLAYER_PLATFORM web)
        else()
            set(JE_PLAYER_PLATFORM linux)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
            set(JE_PLAYER_ARCH wasm32)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            # one runtime holds every ABI (Android) or slice (a universal macOS binary)
            set(JE_PLAYER_ARCH universal)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(JE_PLAYER_ARCH arm64)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            set(JE_PLAYER_ARCH arm64)
        else()
            set(JE_PLAYER_ARCH x64)
        endif()
        # (the web runtime holds a single-thread and a pthreads build of one id: the page picks one)
        set(JE_PLAYER_BACKEND ${GRAPHIC_BACKEND})
        if(JASPERENGINE_DEVELOPMENT_BUILD OR CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(JE_PLAYER_FLAVOUR development)
        else()
            set(JE_PLAYER_FLAVOUR release)
        endif()
        # A release player is built for one variant of engine/player/variants.json, a set of the
        # optional modules (docs/specs/S34-player-size.md R4): the variant sets them here, and its
        # name follows the flavour in the id (-lite); the full player keeps the plain id. The
        # development flavour is always full.
        set(JASPERENGINE_PLAYER_VARIANT "full" CACHE STRING "The release player's variant (engine/player/variants.json)")
        include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/JasperPlayerVariants.cmake)
        jasperengine_player_variant("${JASPERENGINE_PLAYER_VARIANT}" jePlayerVariantSuffix)
        if(jePlayerVariantSuffix AND JE_PLAYER_FLAVOUR STREQUAL "development")
            message(FATAL_ERROR "JASPERENGINE_PLAYER_VARIANT=${JASPERENGINE_PLAYER_VARIANT}: a development player is always the full one")
        endif()
        set(jePlayerId "${JE_PLAYER_PLATFORM}-${JE_PLAYER_ARCH}-${JE_PLAYER_BACKEND}-${JE_PLAYER_FLAVOUR}")
        if(jePlayerVariantSuffix)
            string(APPEND jePlayerId "-${jePlayerVariantSuffix}")
        endif()
        set(JASPERENGINE_PLAYER_ID "${jePlayerId}" CACHE STRING
            "The player's runtime id (set from the platform, backend, flavour and variant)" FORCE)
        add_definitions("-DJASPERENGINE_PLAYER" "-DJASPERENGINE_PLAYER_ID=\"${JASPERENGINE_PLAYER_ID}\"")
        message(STATUS "Player runtime ${JASPERENGINE_PLAYER_ID}")
    endif()

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
        # quoted (a path with spaces), and emcc splits src@dst at a single @: @@ is a literal one
        string(REPLACE "@" "@@" EM_PRELOAD_ROOT "${PROJECT_ROOT}")
        if(EXISTS "${PROJECT_ROOT}/assets")
            set(EM_PRELOAD_FILES "${EM_PRELOAD_FILES} --preload-file \"${EM_PRELOAD_ROOT}/assets@/\"")
            # the web loads the web family of the cooked textures only (S14 R9)
            foreach(family desktop mobile android_etc2)
                set(EM_PRELOAD_FILES "${EM_PRELOAD_FILES} --exclude-file */je-cooked/${family}")
            endforeach()
        endif()
        if(EXISTS "${PROJECT_ROOT}/lua")
            set(EM_PRELOAD_FILES "${EM_PRELOAD_FILES} --preload-file \"${EM_PRELOAD_ROOT}/lua@/\"")
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
                -s EXPORTED_FUNCTIONS=\"['_getScreenWidth','_getScreenHeight','_changeCanvasSize','_main','_malloc','_free']\" \
                -s EXPORTED_RUNTIME_METHODS=\"['ccall', 'cwrap']\" \
                --pre-js \"${PLATFORM_ROOT}/crash_pre.js\" \
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
        else()
            # platform/android makes an EGL context and nothing else
            message(FATAL_ERROR "Android supports GRAPHIC_BACKEND gles3 only (got '${GRAPHIC_BACKEND}')")
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
            ${PLATFORM_ROOT}/HostServicesWin.cpp
            ${PLATFORM_ROOT}/WinInputRouter.cpp
            ${PLATFORM_ROOT}/GamepadWin.cpp
            ${PLATFORM_ROOT}/main.cpp
        )

        list(APPEND PLATFORM_LIBS
            user32
            gdi32
            shell32
            imm32 # IME composition for HTML UI text fields (WinInputRouter)
            ole32 # INetworkListManager (System.getInternetReachability)
            uuid
            advapi32 # the BIOS product name (System.getDeviceModel)
        )

        # HostServices.h, shared with the editor
        include_directories(${JASPERENGINE_ROOT}/platform/common)

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
            ${PLATFORM_ROOT}/X11Visibility.cpp
            ${PLATFORM_ROOT}/SystemLinux.cpp
            ${PLATFORM_ROOT}/HostServicesLinux.cpp
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

            # the privacy manifest (Contents/Resources): the engine's, or the export's composed one
            list(APPEND PLATFORM_RESOURCES
                ${PLATFORM_ROOT}/macos/Main.storyboard
                ${JASPERENGINE_ROOT}/workspaces/xcode/macos/PrivacyInfo.xcprivacy
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
            # HostServices.h, shared with the editor
            include_directories(${JASPERENGINE_ROOT}/platform/common)

            list(APPEND PLATFORM_SOURCE
                ${PLATFORM_ROOT}/JasperEngineMac.h
                ${PLATFORM_ROOT}/JasperEngineMac.mm
                ${PLATFORM_ROOT}/WindowMac.mm
                ${PLATFORM_ROOT}/SystemMac.mm
                ${PLATFORM_ROOT}/HostServicesMac.mm
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

            # A bare executable has no bundle Info.plist, so App Transport Security would block
            # every http:// URL, localhost too: an embedded __info_plist section carries the
            # same ATS dictionary the Xcode bundle gets
            if(JASPERENGINE_ALLOW_INSECURE_HTTP)
                set(JASPERENGINE_ATS_KEY NSAllowsArbitraryLoads)
            else()
                set(JASPERENGINE_ATS_KEY NSAllowsLocalNetworking)
            endif()
            set(JASPERENGINE_ATS_PLIST ${CMAKE_BINARY_DIR}/jasperengine-ats-Info.plist)
            file(WRITE ${JASPERENGINE_ATS_PLIST}
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                "<plist version=\"1.0\">\n<dict>\n"
                "\t<key>NSAppTransportSecurity</key>\n\t<dict>\n\t\t<key>${JASPERENGINE_ATS_KEY}</key>\n\t\t<true/>\n\t</dict>\n"
                "</dict>\n</plist>\n")
            list(APPEND PLATFORM_LIBS "-Wl,-sectcreate,__TEXT,__info_plist,${JASPERENGINE_ATS_PLIST}")

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

            # The prebuilt player is a bundle, jasperengine-player.app: the macOS runtime is a
            # template bundle that an export renames, fills (Contents/Resources) and re-signs with
            # its own Info.plist (S30 R11, R12; tools/players/package_player.py). A bare
            # executable left the players-apple job and the packaging without one.
            if(JASPERENGINE_PLAYER)
                set(PLATFORM_EXEC_FLAGS MACOSX_BUNDLE)
                # the engine's privacy manifest; an export writes the game's over it
                list(APPEND PLATFORM_RESOURCES ${JASPERENGINE_ROOT}/workspaces/xcode/macos/PrivacyInfo.xcprivacy)
                list(APPEND PLATFORM_PROPERTIES
                    MACOSX_BUNDLE_BUNDLE_NAME "JasperEnginePlayer"
                    MACOSX_BUNDLE_GUI_IDENTIFIER "com.jaspergameengine.player"
                )
            endif()
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

        # the privacy manifest at the bundle root: the engine's, or the export's composed one
        list(APPEND PLATFORM_RESOURCES
            ${PLATFORM_ROOT}/ios/LaunchScreen.storyboard
            ${PLATFORM_ROOT}/ios/Main.storyboard
            ${JASPERENGINE_ROOT}/workspaces/xcode/ios/PrivacyInfo.xcprivacy
        )
        set(BUNDLE_RESOURCES_DIR $<TARGET_BUNDLE_DIR:${APP_NAME}>)

        # The system frameworks the engine's code calls into, linked explicitly: the C++ and
        # Objective-C++ files import no modules, so clang autolinks nothing for them (miniaudio's
        # AudioUnit calls need AudioToolbox). The same list as the Xcode export's
        # (Exporter::appleEngineFrameworks, checked by AppleTemplatesTest).
        list(APPEND PLATFORM_LIBS
            -ObjC
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreGraphics"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework Foundation"
            "-framework GameController"
            "-framework Metal"
            "-framework MetalKit"
            "-framework QuartzCore"
            "-framework UIKit"
        )

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

    # A Debug-type export is a development build (Engine.isDevelopmentBuild, the debug overlay's
    # "development" mode): the generated init reads it. Multi-config generators (Xcode, Visual
    # Studio) and Android's debug variant get it for their Debug configuration.
    add_compile_definitions($<$<CONFIG:Debug>:JASPERENGINE_DEVELOPMENT_BUILD>)

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
# precompiled headers (off here by default: exports build as before) and the linker of development
# and test builds (docs/testing.md "Build speed"; selected below, once a shipped game is known)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/JasperPch.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/JasperLinker.cmake)

if(JASPERENGINE_SHARED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(JASPERENGINE_LIB_TYPE SHARED)
else()
    set(JASPERENGINE_LIB_TYPE STATIC)
endif()

# (the editor's export and Play builds pass their job cap; empty or 0: every CPU)
set(JASPERENGINE_PARALLEL_BUILD_JOBS "" CACHE STRING "Parallel compile job limit of Visual Studio builds (/MP); empty or 0: every CPU")
jasperengine_msvc_parallel_compile()

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
include_directories(core/spline)
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
    # clipper2: the union of the tilemap collider builder, which only 2D physics needs
    list(APPEND JASPERENGINE_PHYSICS_LIBS box2d clipper2)
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

# A release game's link (a prebuilt player and a Compile build alike; docs/specs/S30-shipping.md,
# shipped size), set before the libraries so they are built the same way:
#   - every function and datum in its own section, and the linker drops what nothing reaches;
#   - hidden visibility: no symbol leaves the binary but what the platform looks up (Android:
#     platform/android/exports.map, the JNI and GameActivity entry points);
#   - Clang with lld (Android): ThinLTO, and identical code folding of the functions whose
#     address is not taken (--icf=safe).
# Static registrations survive: an initializer is reached from .init_array, a GC and LTO root.
# Not for the editor, the tests, sanitizer or Development builds (their backtraces name every
# function), nor the web: wasm-ld already drops unreferenced code and exports only
# EXPORTED_FUNCTIONS. A release build's own backtraces give module offsets, which its split
# symbols resolve.
set(jasperengineShippedGame OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT JASPERENGINE_TESTS AND NOT JASPERENGINE_SHARED
   AND "${JASPERENGINE_SANITIZE}" STREQUAL "" AND NOT EMSCRIPTEN AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    set(jasperengineShippedGame ON)
endif()
set(JASPERENGINE_RELEASE_LINK_DEFAULT OFF)
if(jasperengineShippedGame AND NOT JASPERENGINE_DEVELOPMENT_BUILD)
    set(JASPERENGINE_RELEASE_LINK_DEFAULT ON)
endif()
option(JASPERENGINE_RELEASE_LINK "A release game's size-optimised link (sections, hidden visibility, ThinLTO and ICF with lld)"
    ${JASPERENGINE_RELEASE_LINK_DEFAULT})
# GCC's link-time optimisation in that link (Linux release games; Android's Clang has ThinLTO).
# A first configure takes its default from the environment when JASPERENGINE_RELEASE_LTO is set there
# (OFF: a game built for a test, e.g. the MCP suite's compiled games but the one that checks LTO,
# docs/testing.md; the link then takes a minute instead of a quarter of an hour)
set(jasperengineReleaseLtoDefault ON)
if(DEFINED ENV{JASPERENGINE_RELEASE_LTO})
    set(jasperengineReleaseLtoDefault "$ENV{JASPERENGINE_RELEASE_LTO}")
endif()
option(JASPERENGINE_RELEASE_LTO "The release link of a GCC Linux game with link-time optimisation" ${jasperengineReleaseLtoDefault})
# A shipped game (a prebuilt player or a Compile build, release or Development) ships stripped:
# it is built with debug information, which the link's post-build step moves into the build
# tree's symbols/ (desktop: <exe>.debug with a debug link, <exe>.dSYM, the PDB). Publish prepare
# puts them in the release's symbols/. Android's Gradle strips the libraries it packages itself.
option(JASPERENGINE_SPLIT_SYMBOLS "A shipped game's debug information, split off into the build tree's symbols/" ${jasperengineShippedGame})
if(JASPERENGINE_SPLIT_SYMBOLS AND NOT ANDROID)
    if(MSVC)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/Zi>)
        add_link_options(/DEBUG:FULL)
    else()
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-g>)
    endif()
endif()
if(JASPERENGINE_RELEASE_LINK)
    if(MSVC)
        # function-level linking, then the unreferenced and identical COMDATs out (also with /DEBUG)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/Gy> $<$<COMPILE_LANGUAGE:C,CXX>:/Gw>)
        add_link_options(/OPT:REF /OPT:ICF)
        set(jasperengineLto "/OPT:REF,ICF")
    else()
        # (C and C++ only: the assemblers of dav1d and others take none of these)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-ffunction-sections>
            $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fdata-sections> $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fvisibility=hidden>
            $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fvisibility-inlines-hidden>)
        if(APPLE)
            add_link_options(LINKER:-dead_strip)
        else()
            add_link_options(LINKER:--gc-sections)
        endif()
        if(ANDROID)
            # what the Java side and GameActivity look up in libjasperengine-android.so
            add_link_options("LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/platform/android/exports.map")
        endif()
        if(ANDROID AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # the NDK links with lld: ThinLTO (its llvm-ar archives the bitcode objects), its cache
            # for the relinks, and safe identical code folding (Clang emits the address-
            # significance tables it needs by default). Set directly: CMake's check_ipo_supported
            # probes with a project that falls back to ld.gold, which cannot load LLVM's plugin.
            add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-flto=thin>)
            add_link_options(-flto=thin "LINKER:--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache" LINKER:--icf=safe)
            set(jasperengineLto "ThinLTO, --icf=safe")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND JASPERENGINE_RELEASE_LTO)
            # GCC's LTO (docs/specs/S34-player-size.md D7): the whole program optimised at the link,
            # identical functions folded (-fipa-icf across units), its partitions compiled by as
            # many jobs as the machine has (-flto=auto); the archives through gcc-ar, which CMake's
            # interprocedural optimisation selects
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
            add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-flto=auto>)
            add_link_options(-flto=auto)
            set(jasperengineLto "GCC LTO")
        else()
            set(jasperengineLto "no LTO")
        endif()
    endif()
    message(STATUS "Release link: dead sections out, hidden visibility, ${jasperengineLto}")
endif()

# Build speed of the trees that ship nothing (docs/specs/S33-build-speed.md R3, R4): lld for their
# Debug and RelWithDebInfo links, and Jolt unity-built. A shipped game keeps the toolchain's linker
# and compiles every vendored source on its own, as always. (The editor tree selects its linker in
# the root CMakeLists.txt, before this directory.)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    jasperengine_select_linker(${jasperengineShippedGame})
endif()
if(jasperengineShippedGame)
    set(jasperengineVendoredUnityDefault OFF)
else()
    set(jasperengineVendoredUnityDefault ON)
endif()
option(JASPERENGINE_VENDORED_UNITY "Unity-build the vendored libraries that gain from it and merge cleanly (Jolt)" ${jasperengineVendoredUnityDefault})

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
if(JASPERENGINE_PHYSICS_3D)
    jasperengine_vendored_unity_build(joltphysics)
endif()
add_subdirectory(libs/harfbuzz)
add_subdirectory(libs/sheenbidi)
add_subdirectory(libs/meshoptimizer)
# The optional modules (docs/specs/S34-player-size.md R1): the video of an HTML UI, and the
# editor's shared engine has every module (its windows and the Play of any project use them)
if(JASPERENGINE_VIDEO AND NOT JASPERENGINE_HTMLUI)
    message(STATUS "JASPERENGINE_VIDEO needs JASPERENGINE_HTMLUI: the build has no video")
    set(JASPERENGINE_VIDEO OFF)
endif()
if(JASPERENGINE_SHARED AND NOT (JASPERENGINE_HTMLUI AND JASPERENGINE_VIDEO AND JASPERENGINE_RUNTIME_SHADERS))
    message(FATAL_ERROR "The editor's engine (JASPERENGINE_SHARED) has every module: JASPERENGINE_HTMLUI, JASPERENGINE_VIDEO and JASPERENGINE_RUNTIME_SHADERS stay ON")
endif()
set(JASPERENGINE_HTMLUI_LIBS "")
set(JASPERENGINE_VIDEO_LIBS "")
if(JASPERENGINE_HTMLUI)
    # the HTML UI's MTSDF glyphs (S16)
    add_subdirectory(libs/msdfgen)
    list(APPEND JASPERENGINE_HTMLUI_LIBS msdfgen thorvg rmlui lexbor)
endif()
if(JASPERENGINE_VIDEO)
    list(APPEND JASPERENGINE_VIDEO_LIBS dav1d opus webm)
endif()
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
# Audio captures (core/audio/AudioCapture, AudioMarkers, spec S35): the editor, the unit tests and
# desktop development players; a release player and the mobile and web ones (no path a harness
# reads) leave them out, and their sites compile to nothing
if(JASPERENGINE_PROFILER_DEFAULT AND NOT (EMSCRIPTEN OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS"))
    set(JASPERENGINE_AUDIO_CAPTURE_DEFAULT ON)
else()
    set(JASPERENGINE_AUDIO_CAPTURE_DEFAULT OFF)
endif()
option(JASPERENGINE_AUDIO_CAPTURE "Compile audio captures and their gameplay markers" ${JASPERENGINE_AUDIO_CAPTURE_DEFAULT})
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
# SVG images (texture/TextureData, and the HTML UI's)
add_subdirectory(libs/lunasvg)
if(JASPERENGINE_HTMLUI)
    add_subdirectory(libs/thorvg)
    add_subdirectory(libs/rmlui)
    # the HTML5 parser of the HTML UI compatibility layer (engine/core/htmlui/compat/html)
    add_subdirectory(libs/lexbor)
endif()
if(JASPERENGINE_VIDEO)
    # the HTML UI's <video>: AV1 (dav1d), Opus (libopus) and the WebM parser (libwebm)
    add_subdirectory(libs/dav1d)
    add_subdirectory(libs/opus)
    add_subdirectory(libs/libwebm)
endif()

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

set(jasperengineQuietVideoLibs "")
if(JASPERENGINE_VIDEO)
    set(jasperengineQuietVideoLibs dav1d dav1d_bitdepth_8 dav1d_bitdepth_16 opus webm)
endif()
foreach(lib IN ITEMS
    sokol lua stb soloud tinyobjloader tinygltf tinyxml2
    harfbuzz sheenbidi meshoptimizer lunasvg ${JASPERENGINE_HTMLUI_LIBS}
    ${jasperengineQuietVideoLibs} ${JASPERENGINE_PHYSICS_LIBS} ${JASPERENGINE_TEXTURE_LIBS})
    target_compile_options(${lib} PRIVATE ${EXTERNAL_WARNING_FLAG})
endforeach()
if(JASPERENGINE_NAVIGATION)
    target_compile_options(recastnavigation PRIVATE ${EXTERNAL_WARNING_FLAG})
    target_compile_options(recastnavigation_debugdraw PRIVATE ${EXTERNAL_WARNING_FLAG})
    jasperengine_apply_cxx_standard(recastnavigation recastnavigation_debugdraw)
    # No fused multiply-adds in the voxeliser: clang contracts a * b + c into one rounding on
    # arm64 (Apple silicon, Android, iOS) and x86-64 GCC builds do not, and Recast clips
    # triangles at cell boundaries, so a mesh edge on a boundary lost its last column of cells
    # on arm64 (a plane walkable to 9.2 m instead of 9.5 m: NavQueryTest on macOS). Without
    # contraction both bake the same edges; the cost is in the bake only.
    if(NOT MSVC)
        target_compile_options(recastnavigation PRIVATE -ffp-contract=off)
    endif()
endif()
# The same for meshoptimizer's simplifier (placement contexts, LODs): with contraction an arm64
# build collapsed other edges than x86-64, and its absolute 1 cm target strayed 1.5 cm
# (PlacementWorldReview.SimplificationErrorIsAbsolute on macOS)
if(NOT MSVC)
    target_compile_options(meshoptimizer PRIVATE -ffp-contract=off)
endif()
# The glTF loader's meshopt decoder (docs/specs/S53-model-import-cook.md R27): WebAssembly has no
# run-time SIMD detection, so the codec and filters take simd128 at build time, with the particle
# kernels' switch (JASPERENGINE_WASM_SIMD, core/particles/ParticleSources.cmake; OFF stays scalar).
# Only these two files: the simplifier keeps the scalar maths the LOD and placement tests pin.
if(EMSCRIPTEN AND (NOT DEFINED JASPERENGINE_WASM_SIMD OR JASPERENGINE_WASM_SIMD))
    set_source_files_properties(libs/meshoptimizer/vertexcodec.cpp libs/meshoptimizer/vertexfilter.cpp
        DIRECTORY libs/meshoptimizer PROPERTIES COMPILE_OPTIONS -msimd128)
endif()

# One C++ standard for every object of the binary: libc++ template definitions instantiated
# at two standards across a library boundary (tinygltf and RmlUi pass std containers into
# engine code) broke Wasm signatures. Most of these set a standard of their own, or none.
set(jasperengineCxxHtmlUILibs "")
if(JASPERENGINE_HTMLUI)
    set(jasperengineCxxHtmlUILibs msdfgen thorvg rmlui)
endif()
if(JASPERENGINE_VIDEO)
    list(APPEND jasperengineCxxHtmlUILibs webm)
endif()
jasperengine_apply_cxx_standard(
    sokol soloud tinyobjloader tinygltf tinyxml2 harfbuzz meshoptimizer
    lunasvg ${jasperengineCxxHtmlUILibs} ${JASPERENGINE_PHYSICS_LIBS} basisu_transcoder)
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
if((JASPERENGINE_HTMLUI_TESTS OR JASPERENGINE_TESTS) AND NOT (JASPERENGINE_HTMLUI AND JASPERENGINE_VIDEO))
    message(FATAL_ERROR "The tests cover the HTML UI and its video: JASPERENGINE_HTMLUI and JASPERENGINE_VIDEO stay ON")
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
    core/EngineTime.cpp
    core/PlatformConfig.cpp
    core/player/ContentManifest.cpp
    core/player/Launch.cpp
    core/player/LaunchOptions.cpp
    core/player/PlayerConfig.cpp
    core/player/PlayerModules.cpp
    core/player/RunControl.cpp
    core/Input.cpp
    core/input/InputControlPath.cpp
    core/input/InputActionAsset.cpp
    core/input/InputActions.cpp
    core/input/InputRebind.cpp
    core/input/InputVirtualControls.cpp
    core/audio/AudioCapture.cpp
    core/audio/AudioClock.cpp
    core/audio/AudioEffects.cpp
    core/audio/AudioFileCheck.cpp
    core/audio/AudioMarkers.cpp
    core/audio/AudioMeter.cpp
    core/audio/AudioMixer.cpp
    core/audio/AudioMixerIO.cpp
    core/audio/AudioTap.cpp
    core/audio/SoloudEngineFile.cpp
    core/audio/TruePeak.cpp
    core/input/InputControlNames.cpp
    core/input/InputManagerFormat.cpp
    core/Log.cpp
    core/device/Device.cpp
    core/device/DeviceSettings.cpp
    core/device/GpuTierDatabase.cpp
    core/device/GpuTiersData.cpp
    core/device/QualityResolver.cpp
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
    core/debug/DebugOverlay.cpp
    core/debug/DebugOverlayResources.cpp
    core/debug/DebugStats.cpp
    core/debug/ProcessMemory.cpp
    core/Scene.cpp
    core/SceneSettings.cpp
    core/serialization/CookCodecs.cpp
    core/serialization/CookedScene.cpp
    core/serialization/CookStream.cpp
    core/serialization/codecs/ComponentHooks.cpp
    core/serialization/RuntimeFields.cpp
    core/serialization/SceneSettingsCodec.cpp
    core/Screen.cpp
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
    core/animation/AnimationClip.cpp
    core/animation/AnimationClipIO.cpp
    core/animation/AnimJson.cpp
    core/animation/AvatarMaskWeights.cpp
    core/animation/UpstreamAnimFormats.cpp
    core/camera/CameraShake.cpp
    core/camera/ShakeInstance.cpp
    core/camera/ShakePreset.cpp
    core/camera/ShakePresets.cpp
    core/camera/ShakerRuntime.cpp
    core/animation/AnimationEvent.cpp
    core/animation/ClipAudio.cpp
    core/animation/AnimationPlayerRuntime.cpp
    core/animation/AnimatorAssetIO.cpp
    core/animation/AnimatorIK.cpp
    core/animation/AnimatorInstance.cpp
    core/animation/AnimatorPose.cpp
    core/animation/AnimatorRetarget.cpp
    core/animation/AnimatorRig.cpp
    core/animation/BlendTree.cpp
    core/animation/VertexAnimatorRuntime.cpp
    core/animation/ClipBinding.cpp
    core/animation/CompiledController.cpp
    core/animation/PropertyBindings.cpp
    core/animation/HumanoidClip.cpp
    core/animation/KeyframeSampling.cpp
    core/animation/ModelClipSettings.cpp
    core/animation/ModelSubClips.cpp
    core/animation/VertexAnimation.cpp
    core/pose/HumanBones.cpp
    core/pose/HumanExtraBones.cpp
    core/pose/HumanoidMap.cpp
    core/pose/HumanoidMapIO.cpp
    core/pose/ModelRig.cpp
    core/pose/PoseAnimationIO.cpp
    core/pose/PoseAnimatorRuntime.cpp
    core/pose/PoseDofs.cpp
    core/pose/PoseFile.cpp
    core/pose/PoseIK.cpp
    core/pose/PoseMetrics.cpp
    core/pose/PoseRealisation.cpp
    core/pose/PoseRanges.cpp
    core/pose/PoseRig.cpp
    core/pose/PoseSampler.cpp
    core/pose/PoseSecondary.cpp
    core/pose/PoseSkin.cpp
    core/pose/PoseTween.cpp
    core/buffer/Attribute.cpp
    core/buffer/Buffer.cpp
    core/buffer/ExternalBuffer.cpp
    core/buffer/IndexBuffer.cpp
    core/buffer/InterleavedBuffer.cpp
    core/ecs/ComponentManager.cpp
    core/ecs/ChangeTracking.cpp
    core/ecs/CommandBuffer.cpp
    core/ecs/SystemScheduler.cpp
    # pixel art (S41: the pixel-snap rule, the integer layout, the rotation policy)
    core/pixelart/PixelPerfectLayout.cpp
    core/pixelart/PixelRotation.cpp
    core/pixelart/PixelsPerUnit.cpp
    core/pixelart/PixelSnap.cpp
    # pixel fonts (S51: bitmap fonts, their layout, the styled glyph cache, the built-in font)
    core/pixelart/BitmapFont.cpp
    core/pixelart/BitmapFontParsers.cpp
    core/pixelart/BitmapFontPool.cpp
    core/pixelart/BitmapText.cpp
    core/pixelart/BitmapTextCache.cpp
    core/pixelart/BitmapTextRaster.cpp
    core/pixelart/BuiltinPixelFont.cpp
    core/pixelart/FontPixelReport.cpp
    core/picking/BinnedSah.cpp
    core/picking/MeshBvh.cpp
    core/picking/MeshBvhSimd_avx2.cpp
    core/picking/MeshBvhSimd_neon.cpp
    core/picking/MeshPose.cpp
    core/picking/MeshRaycast.cpp
    core/picking/PickCache.cpp
    core/picking/PickGeometry.cpp
    core/picking/SceneBvh.cpp
    core/htmlui/HtmlUI.cpp
    core/htmlui/HtmlUIClock.cpp
    core/htmlui/HtmlTextEffects.cpp
    core/htmlui/UIParticleSystemElement.cpp
    core/htmlui/HtmlUIContext.cpp
    core/htmlui/HtmlUIContextRender.cpp
    core/htmlui/HtmlUIDependencies.cpp
    core/htmlui/HtmlUITexture.cpp
    core/htmlui/RmlFile.cpp
    core/htmlui/RmlFontEffects.cpp
    core/htmlui/RmlFontEngine.cpp
    core/htmlui/RmlFontEngineBitmap.cpp
    core/htmlui/RmlFontMsdf.cpp
    core/htmlui/RmlRenderer.cpp
    core/htmlui/RmlRendererBatch.cpp
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
    core/htmlui/controls/OnScreenControls.cpp
    core/media/Av1Decoder.cpp
    core/media/GrowingFileReader.cpp
    core/media/MediaByteSource.cpp
    core/media/MediaCache.cpp
    core/media/Mp4Probe.cpp
    core/media/OpusAudioDecoder.cpp
    core/media/PlatformVideoSource.cpp
    core/media/VideoAudio.cpp
    core/media/VideoColorConvert.cpp
    core/media/VideoDemuxer.cpp
    core/media/VideoFormats.cpp
    core/media/VideoPlayback.cpp
    core/media/VideoSourceFactory.cpp
    core/media/VideoStream.cpp
    core/media/VideoTextureConverter.cpp
    core/htmlui/media/UIElementVideo.cpp
    core/htmlui/media/UIVideoControls.cpp
    core/htmlui/media/UIVideoElement.cpp
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
    core/asset/ImportedAssets.cpp
    # sprite sheets (S44): the record, its IO, the pool, frame geometry
    core/sprite/SpriteSheetIO.cpp
    core/sprite/SpriteSheetPool.cpp
    core/sprite/SpriteFrameGeometry.cpp
    core/render/sprite/SpriteInstanceBatch.cpp
    core/sprite/AsepriteClips.cpp
    # S45: texture sheets, name@WxH, .spriteatlas and the atlas remap
    core/sprite/SpriteSheetSources.cpp
    core/graph/Graph.cpp
    core/graph/GraphRegistry.cpp
    core/io/AssetIOService.cpp
    core/io/Data.cpp
    core/io/File.cpp
    core/io/FileData.cpp
    core/io/JPak.cpp
    core/io/FileUtils.cpp
    core/io/ResourcePack.cpp
    core/io/watch/ExpectedWriteLedger.cpp
    core/io/watch/FileChangeCoalescer.cpp
    core/io/watch/FileIdentity.cpp
    core/io/watch/FileWatchService.cpp
    core/io/watch/FileWatcher.cpp
    core/io/watch/LinkedFolders.cpp
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
    core/native/NativeBridge.cpp
    core/net/Http.cpp
    core/net/HttpUtil.cpp
    core/io/UserSettings.cpp
    core/io/AtomicFile.cpp
    core/io/ScopedFileLock.cpp
    core/io/UserSettingsStore.cpp
    core/math/AABB.cpp
    core/math/Curve.cpp
    core/math/CurveJson.cpp
    # the Line and Trail Renderers' curves and gradients (upstream's, beside S27's Curve)
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
    core/object/CameraFollow.cpp
    core/object/CameraShaker.cpp
    core/object/EntityHandle.cpp
    core/object/Light.cpp
    core/object/PixelPerfectCamera.cpp
    core/object/SortingGroup.cpp
    core/object/tilemap/Grid.cpp
    core/object/tilemap/TilemapLayer.cpp
    core/object/SpriteBatch.cpp
    core/object/Light2D.cpp
    core/object/LineRenderer.cpp
    core/object/TrailRenderer.cpp
    core/object/Mesh.cpp
    core/object/VideoPlayer.cpp
    core/object/MeshPolygon.cpp
    core/object/Mirror.cpp
    core/object/FluidSimulation.cpp
    core/object/Lattice.cpp
    core/object/Model.cpp
    core/object/PoseAnimator.cpp
    core/object/AnimationPlayer.cpp
    core/object/VertexAnimator.cpp
    core/object/Animator.cpp
    core/object/GameObject.cpp
    core/object/ShadowCaster2D.cpp
    core/object/CapsuleShadowCaster.cpp
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
    core/object/environment/WaterExcluder.cpp
    core/object/environment/WaterWake.cpp
    core/object/physics/Buoyancy.cpp
    core/object/spline/Spline.cpp
    core/object/spline/SplineExtrude.cpp
    core/object/spline/SplineInstantiate.cpp
    core/object/spline/SplineAnimate.cpp
    core/object/environment/VolumetricClouds.cpp
    core/object/ui/Button.cpp
    core/object/ui/OnScreenControls.cpp
    core/object/ui/LayoutGroup.cpp
    core/object/ui/CanvasGroup.cpp
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
    core/pool/MeshBufferPool.cpp
    core/pool/AnimationClipPool.cpp
    core/pool/ShakePresetPool.cpp
    core/pool/PhysicsMaterialPool.cpp
    core/pool/VertexAnimationPool.cpp
    core/pool/AnimatorAssetPool.cpp
    core/object/physics/Collision.cpp
    core/object/physics/Collision2D.cpp
    core/registry/EntityCloner.cpp
    core/registry/EntityRegistry.cpp
    core/registry/TagManager.cpp
    core/registry/PhysicsLayers.cpp
    core/registry/SortingLayers.cpp
    core/physics/ContactTracker.cpp
    core/physics/ConvexDecomposition.cpp
    core/physics/JoltShapeBuilder.cpp
    core/physics/PhysicsDebugLines.cpp
    core/physics/PhysicsMaterial.cpp
    core/physics/RagdollBuilder.cpp
    core/physics/PolygonDecomposition2D.cpp
    core/physics/PlacementWorld.cpp
    core/render/AtmosphereMath.cpp
    core/render/FrameReadback.cpp
    core/render/AtmosphereNoise.cpp
    core/render/AtmospherePresets.cpp
    core/render/AtmosphereQuality.cpp
    core/render/BufferRender.cpp
    core/render/CameraRender.cpp
    core/render/DynamicResolutionController.cpp
    core/render/CpuSkin.cpp
    core/render/FramebufferRender.cpp
    core/render/FrustumCull.cpp
    core/render/DrawBatcher.cpp
    core/render/LineRibbonBuilder.cpp
    core/render/LodFade.cpp
    core/render/ObjectRender.cpp
    core/render/ShaderRender.cpp
    core/render/SSRProbeSlots.cpp
    core/render/SystemRender.cpp
    core/render/TextureRender.cpp
    core/render/VisibleList.cpp
    core/render/shadow/CascadeFit.cpp
    core/render/shadow/ShadowCache.cpp
    core/render/shadow/ShadowCasterRegistry.cpp
    core/render/shadow/ShadowSlotPlanner.cpp
    core/render/shadow/ShadowTiers.cpp
    core/render/shadow/ShadowTypes.cpp
    core/render/CapsuleShadows.cpp
    core/render/ContactShadows.cpp
    core/render/LightingTiers.cpp
    core/render/OrderIndependentTransparency.cpp
    core/render/TiledLights.cpp
    core/script/AsyncOwner.cpp
    core/script/LuaBinding.cpp
    core/script/LuaFunctionBase.cpp
    core/script/LuaJson.cpp
    core/script/LuaWatchdog.cpp
    core/script/LuaReload.cpp
    core/script/LuaScript.cpp
    core/script/ScriptBase.cpp
    core/script/ScriptScheduler.cpp
    core/script/ScriptEvent.cpp
    core/script/ScriptScheduler.cpp
    core/script/ScriptMessages.cpp
    core/script/ScriptProperty.cpp
    core/script/ToolchainFingerprint.cpp
    core/script/ScriptRuntime.cpp
    core/script/binding/ActionClassesLua.cpp
    core/script/binding/CoreClassesLua.cpp
    core/script/binding/CrashClassesLua.cpp
    core/script/binding/AudioClassesLua.cpp
    core/script/binding/ECSClassesLua.cpp
    core/script/binding/IOClassesLua.cpp
    core/script/binding/MathClassesLua.cpp
    core/script/binding/NativeClassesLua.cpp
    core/script/binding/NetClassesLua.cpp
    core/script/binding/ParticleClassesLua.cpp
    core/script/binding/GameObjectClassesLua.cpp
    core/script/binding/ProfilerClassesLua.cpp
    core/script/binding/GameplayClassesLua.cpp
    core/script/binding/SplineClassesLua.cpp
    core/script/binding/PixelArtClassesLua.cpp
    core/script/binding/SortingClassesLua.cpp
    core/script/binding/TilemapClassesLua.cpp
    core/script/binding/ThreadClassesLua.cpp
    core/script/binding/HtmlUIVectorLua.cpp
    core/script/binding/HtmlUIMediaLua.cpp
    core/script/binding/HtmlUIWidgetsLua.cpp
    core/script/binding/UtilClassesLua.cpp
    core/script/binding/HtmlUIClassesLua.cpp
    core/script/binding/HtmlUIAnimeLua.cpp
    core/script/binding/CoroutineClassesLua.cpp
    core/script/binding/ScriptClassesLua.cpp
    core/shader/RuntimeShaderCompiler.cpp
    core/shader/ShaderData.cpp
    core/shader/ShaderDataSerializer.cpp
    core/shader/ShaderManifest.cpp
    core/shader/ShaderUniforms.cpp
    core/shader/ShaderGlobals.cpp
    core/shader/ShaderVariantCollection.cpp
    core/shader/ShaderWarmup.cpp
    core/shader/ShaderVariantCompiler.cpp
    core/subsystem/ActionSystem.cpp
    core/subsystem/ActionSystemPlayers.cpp
    core/subsystem/ActionSystemLayers.cpp
    core/subsystem/PoseAnimatorSystem.cpp
    core/subsystem/VertexAnimatorSystem.cpp
    core/subsystem/AnimatorSystem.cpp
    core/subsystem/TrailSystem.cpp
    core/subsystem/WaterInteractionSystem.cpp
    core/subsystem/CameraControlSystem.cpp
    core/subsystem/FluidSystem.cpp
    core/subsystem/VideoPlayerSystem.cpp
    core/fluid/FluidSimulator.cpp
    core/fluid/FluidPresets.cpp
    core/subsystem/AudioSystem.cpp
    core/subsystem/MeshSystem.cpp
    core/subsystem/LatticeSystem.cpp
    core/subsystem/SplineSystem.cpp
    core/lattice/LatticeSolver.cpp
    core/lattice/LatticePresets.cpp
    core/lattice/LatticeRender.cpp
    core/subsystem/MeshSystemGltfPbr.cpp
    core/subsystem/GltfNormalize.cpp
    core/subsystem/PhysicsSystem.cpp
    core/subsystem/WaterSystem.cpp
    core/subsystem/AtmosphereRenderer.cpp
    core/subsystem/PhysicsSystemContacts.cpp
    core/subsystem/PhysicsSystemJoints3D.cpp
    core/subsystem/PhysicsSystemRagdoll.cpp
    core/subsystem/PhysicsSystemDebugDraw.cpp
    core/subsystem/RenderSystem.cpp
    core/subsystem/RenderSystemShadows.cpp
    core/subsystem/RenderSystemBatch.cpp
    core/subsystem/RenderSystemLighting.cpp
    core/subsystem/RenderSystemPixelArt.cpp
    core/subsystem/RenderSystemSorting.cpp
    core/subsystem/RenderSystemSpriteBatch.cpp
    core/subsystem/UISystem.cpp
    core/subsystem/UISystemPointer.cpp
    core/subsystem/UISystemNavigation.cpp
    core/subsystem/UISystemBitmapText.cpp
    core/subsystem/UIDocumentSystem.cpp
    core/subsystem/UIDocumentSystemRender.cpp
    core/subsystem/UIDocumentSystemInput.cpp
    core/subsystem/UIDocumentSystemScript.cpp
    core/subsystem/UIDocumentSystemPatch.cpp
    core/subsystem/UIDocumentSystemReload.cpp
    core/subsystem/UIDocumentSystemCompat.cpp
    core/subsystem/UIDocumentSystemTexture.cpp
    core/model/CookedModels.cpp
    core/model/ModelCpuData.cpp
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
    core/memory/HeapTrim.cpp
    core/memory/MemoryStats.cpp
    core/memory/ObjectPool.cpp
    core/memory/RangeAllocator.cpp
    core/memory/ScratchScope.cpp
    core/thread/MainThreadQueue.cpp
    core/thread/ResourceProgress.cpp
    core/thread/ThreadUtil.cpp
    core/util/Angle.cpp
    core/util/Base64.cpp
    core/util/Color.cpp
    core/util/CrashHook.cpp
    core/util/ContentHash.cpp
    core/util/CubeLut.cpp
    core/util/FontFace.cpp
    core/util/FontRegistry.cpp
    core/util/FrameLimiter.cpp
    core/util/CanvasScaler.cpp
    core/util/TextEffects.cpp
    core/util/GlyphAtlas.cpp
    core/util/JoltJobAdapter.cpp
    core/util/LunaSVGRuntime.cpp
    core/util/PlatformRules.cpp
    core/util/NumberText.cpp
    core/util/PostColorLut.cpp
    core/util/Scheduler.cpp
    core/util/Sha256.cpp
    core/util/STBText.cpp
    core/util/StringUtils.cpp
    core/util/SubscriptionOwnerIndex.cpp
    core/util/TextShaper.cpp
    core/util/ThorVGRuntime.cpp
    core/util/UniqueToken.cpp
    core/util/WaterExclusion.cpp
    core/util/WaterWakeMap.cpp
    core/util/XMLUtils.cpp
)

# The HTTP client's backend (core/net/HttpBackend.h): each platform's own stack. Outside the
# list above, which the Xcode workspace mirrors (tools/xcode_sync_engine.py): its engine targets
# compile platform/apple/HttpBackendApple.mm instead.
if(EMSCRIPTEN)
    set(JASPERENGINE_HTTP_BACKEND platform/emscripten/HttpBackendWeb.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(JASPERENGINE_HTTP_BACKEND platform/android/HttpBackendAndroid.cpp)
elseif(WIN32)
    set(JASPERENGINE_HTTP_BACKEND platform/win/HttpBackendWin.cpp)
elseif(APPLE)
    set(JASPERENGINE_HTTP_BACKEND platform/apple/HttpBackendApple.mm)
    set_source_files_properties(platform/apple/HttpBackendApple.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
    # Linux and FreeBSD: the system libcurl through dlopen
    set(JASPERENGINE_HTTP_BACKEND core/net/HttpBackendCurl.cpp)
endif()
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_HTTP_BACKEND})

# The video decoders of MP4 and other non-AV1 video (core/media/PlatformVideoBackend.h): each
# platform's own (docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.6). Outside the list
# above like the HTTP backend: the Xcode workspace compiles platform/apple/AVFoundationBackend.mm.
if(EMSCRIPTEN)
    # the browser's <video> (its JavaScript inline, EM_JS, as the engine's other Web code)
    set(JASPERENGINE_VIDEO_BACKEND platform/emscripten/WebVideoSource.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(JASPERENGINE_VIDEO_BACKEND platform/android/MediaCodecBackend.cpp)
elseif(WIN32)
    set(JASPERENGINE_VIDEO_BACKEND platform/win/MediaFoundationBackend.cpp)
elseif(APPLE)
    set(JASPERENGINE_VIDEO_BACKEND platform/apple/AVFoundationBackend.mm)
    set_source_files_properties(platform/apple/AVFoundationBackend.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
    # Linux and FreeBSD: the system FFmpeg through dlopen, one translation unit per ABI major,
    # each compiled against its vendored headers only (-isystem: ahead of a system FFmpeg's)
    set(JASPERENGINE_VIDEO_BACKEND platform/linux/FFmpegBackend.cpp)
    foreach(major 58 59 60 61 62)
        list(APPEND JASPERENGINE_VIDEO_BACKEND platform/linux/FFmpegBackend${major}.cpp)
        set_source_files_properties(platform/linux/FFmpegBackend${major}.cpp PROPERTIES
            COMPILE_OPTIONS "-isystem;${CMAKE_CURRENT_SOURCE_DIR}/libs/ffmpeg-headers/${major}")
    endforeach()
endif()
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_VIDEO_BACKEND})
# The native bridge's platform layer (core/native/NativePlatform.h): Java/Kotlin through JNI,
# Objective-C/Swift through the runtime, JavaScript through EM_JS; none elsewhere (C++ plugins
# and Lua mocks only). Outside JASPERENGINE_SRCS like the HTTP backend: the Xcode workspace
# compiles the Apple files in its engine targets (tools/xcode_sync_engine.py).
if(EMSCRIPTEN)
    set(JASPERENGINE_NATIVE_PLATFORM platform/emscripten/NativePlatformWeb.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(JASPERENGINE_NATIVE_PLATFORM platform/android/NativePlatformAndroid.cpp)
elseif(APPLE)
    # JasperNative.m: the helper API plugin code calls (JasperNative.h)
    set(JASPERENGINE_NATIVE_PLATFORM platform/apple/NativePlatformApple.mm platform/apple/JasperNative.m)
    set_source_files_properties(platform/apple/NativePlatformApple.mm platform/apple/JasperNative.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
    set(JASPERENGINE_NATIVE_PLATFORM core/native/NativePlatformNone.cpp)
endif()
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_NATIVE_PLATFORM})

# The video module (docs/specs/S34-player-size.md R1): <video>, its decoders and its Lua API
if(NOT JASPERENGINE_VIDEO)
    # what links dav1d, Opus and libwebm, and the platform decoders (FFmpeg's headers on Linux). The
    # rest of core/media stays: VideoSourceFactory makes a source that fails with the reason, and the
    # format rules stay for the compatibility layer, which drops a video used as a CSS image
    list(FILTER JASPERENGINE_SRCS EXCLUDE REGEX
        "^core/(htmlui/media/|script/binding/HtmlUIMediaLua|media/(Av1Decoder|OpusAudioDecoder|VideoDemuxer|VideoStream)[.])|^platform/.*/(WebVideoSource|MediaCodecBackend|MediaFoundationBackend|AVFoundationBackend|FFmpegBackend)")
endif()
# The HTML UI module: core/htmlui (but its clock, which the engine advances), the UI Document
# system, the documents' Lua objects and bindings, the on-screen controls and the Lottie runtime
if(JASPERENGINE_HTMLUI)
    # HTML/CSS compatibility layer (phase 2): globbed per directory
    include(core/htmlui/compat/CompatSources.cmake)
    list(APPEND JASPERENGINE_SRCS ${HTMLUI_COMPAT_SRCS})
    # HTML UI widgets (S17): virtual views, tooltips, drag and drop, Markdown
    include(core/htmlui/widgets/WidgetsSources.cmake)
    list(APPEND JASPERENGINE_SRCS ${HTMLUI_WIDGETS_SRCS})
else()
    list(FILTER JASPERENGINE_SRCS EXCLUDE REGEX
        "^core/(htmlui/|debug/DebugOverlayResources|subsystem/UIDocumentSystem|object/ui/(UIDocument|UIElement|UIEvent|UIDataModel|UIValue|HtmlUI)|script/binding/HtmlUI|input/InputVirtualControls|util/ThorVGRuntime)")
    list(APPEND JASPERENGINE_SRCS core/htmlui/HtmlUIClock.cpp)
endif()
# Particles (S20): the SIMD kernels' flags, the subsystem and its drawing
include(core/particles/ParticleSources.cmake)
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_PARTICLE_SRCS} ${JASPERENGINE_PARTICLE_SYSTEM_SRCS})
# Navigation (S19): navmeshes, crowds, grids, the NavigationSystem and its Lua objects
include(core/navigation/NavigationSources.cmake)
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_NAVIGATION_SRCS})

# The sound layer (S34 module soundlayer): its own list, which S36, S37, S39 and S40 add to
include(core/audio/SoundLayerSources.cmake)
if(JASPERENGINE_SOUND_LAYER)
    list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_SOUND_LAYER_SRCS})
endif()
# Splines (S18): the core, geometry and layouts (no contraction, bit-identical output)
include(core/spline/SplineSources.cmake)
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_SPLINE_SRCS})
# Pixel art (S43): colour maths and palettes (no contraction, bit-identical output)
include(core/pixelart/PixelArtSources.cmake)
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_PIXELART_SRCS})
# Grid tilemaps (S50): cells, layouts, tilesets, terrains
include(core/tilemap/TilemapSources.cmake)
list(APPEND JASPERENGINE_SRCS ${JASPERENGINE_TILEMAP_SRCS})
if(JASPERENGINE_HTMLUI)
    include(libs/lucide/lucide.cmake)
endif()

if(JASPERENGINE_PHYSICS_2D)
    list(APPEND JASPERENGINE_SRCS
        core/object/physics/Rigidbody2D.cpp
        core/object/physics/Contact2D.cpp
        core/object/physics/Joint2D.cpp
        core/object/physics/Manifold2D.cpp
        core/object/physics/CharacterController2D.cpp
        core/subsystem/CharacterController2DSystem.cpp
        core/tilemap/TileColliderBuilder.cpp
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
        core/object/physics/Ragdoll.cpp
    )
endif()

set(RENDER_SOKOL_SRC
    renders/sokol/SokolBuffer.cpp
    renders/sokol/SokolTexture.cpp
    renders/sokol/SokolSystem.cpp
    renders/sokol/SokolCamera.cpp
    renders/sokol/SokolObject.cpp
    renders/sokol/SokolPipelineCache.cpp
    renders/sokol/SokolAsyncCreate.cpp
    renders/sokol/SokolPipelineRecord.cpp
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

# The clip pool's test seams (pool/AnimationClipPoolTesting.h) exist only in builds with the unit
# tests (JASPERENGINE_TESTS): the define stays on the two sources, so a shipped engine has none of it
if(JASPERENGINE_TESTS)
    set_property(SOURCE core/pool/AnimationClipPool.cpp core/animation/ModelSubClips.cpp
        APPEND PROPERTY COMPILE_DEFINITIONS JASPERENGINE_ANIM_TEST_HOOKS)
endif()

if(JASPERENGINE_HTMLUI)
    jasperengine_htmlui_compat_embed(jasperengine)
    jasperengine_htmlui_lucide_embed(jasperengine)
endif()
jasperengine_target_warnings(jasperengine)
# Unity build: the HTML UI sources keep generic file-local helpers (lower, trim, startsWith, PI,
# Registry, Cache, ...) in anonymous namespaces at global scope, one helper set per file
get_target_property(JASPERENGINE_UNITY_SKIP jasperengine SOURCES)
list(FILTER JASPERENGINE_UNITY_SKIP INCLUDE REGEX "core/htmlui/")
jasperengine_skip_unity_build(
    REASON "the HTML UI subsystem: per-file helper sets with the same names"
    SOURCES ${JASPERENGINE_UNITY_SKIP})
# Triangle picking (core/picking/, S26): every SIMD path of the BVH slab test does the same
# arithmetic, which needs the compiler not to fuse a multiply and an add (arm64 GCC and Clang do
# by default); its own units, since a unity batch would apply that to its neighbours too
set(JASPERENGINE_PICKING_SRCS ${JASPERENGINE_SRCS})
list(FILTER JASPERENGINE_PICKING_SRCS INCLUDE REGEX "^core/picking/")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${JASPERENGINE_PICKING_SRCS} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
jasperengine_skip_unity_build(
    REASON "the picking sources: no floating-point contraction, per unit"
    SOURCES ${JASPERENGINE_PICKING_SRCS})
jasperengine_skip_unity_build(
    REASON "the particle kernels build with their own floating-point flags"
    SOURCES ${JASPERENGINE_PARTICLE_SRCS})
jasperengine_skip_unity_build(
    REASON "the navigation sources: file-local Detour helpers and constants with the same names"
    SOURCES ${JASPERENGINE_NAVIGATION_SRCS})
if(JASPERENGINE_SPLINE_SRCS)
    jasperengine_skip_unity_build(
        REASON "the spline sources build with their own floating-point flags"
        SOURCES ${JASPERENGINE_SPLINE_SRCS})
endif()
# Unity build: these sources keep file-local helpers or types (State, Category, TempDir, ...)
# whose names another source of the engine defines too (docs/specs/S33-build-speed.md M8)
jasperengine_skip_unity_build(
    REASON "file-local helpers or types named as in another engine source"
    SOURCES
        core/subsystem/ActionSystemPlayers.cpp
        core/animation/ClipBinding.cpp
        core/media/VideoStream.cpp
        core/pose/PoseDofs.cpp
        core/shader/ShaderGlobals.cpp
        core/animation/AnimationPlayerRuntime.cpp
        core/animation/HumanoidClip.cpp
        core/animation/UpstreamAnimFormats.cpp
        core/audio/AudioEffects.cpp
        core/audio/AudioMixer.cpp
        core/camera/ShakePreset.cpp
        core/diag/HangStackCapture.cpp
        core/media/MediaByteSource.cpp
        core/media/VideoFormats.cpp
        core/object/TrailRenderer.cpp
        core/pool/AnimatorAssetPool.cpp
        core/pool/PhysicsMaterialPool.cpp
        core/pool/TexturePool.cpp
        core/pool/VertexAnimationPool.cpp
        core/pose/HumanoidMap.cpp
        core/script/binding/GameObjectClassesLua.cpp
        core/script/binding/IOClassesLua.cpp
        core/script/binding/NativeClassesLua.cpp
        core/script/binding/NetClassesLua.cpp
        core/subsystem/ActionSystemLayers.cpp
        core/subsystem/CharacterController2DSystem.cpp
        core/subsystem/PhysicsSystemJoints3D.cpp
        core/subsystem/VertexAnimatorSystem.cpp
        core/Input.cpp
        core/animation/AnimatorAssetIO.cpp
        core/diag/CrashHandlerPosix.cpp
        core/graph/GraphRegistry.cpp
        core/lattice/LatticePresets.cpp
        core/lattice/LatticeSolver.cpp
        core/object/spline/SplineAnimate.cpp
        core/object/spline/SplineInstantiate.cpp
        core/pose/PoseAnimationIO.cpp
        core/pose/PoseFile.cpp
        core/profiler/GpuMemory.cpp
        core/profiler/Profiler.cpp
        core/script/binding/HtmlUIClassesLua.cpp
        core/shader/ShaderWarmup.cpp
        core/subsystem/PoseAnimatorSystem.cpp
        core/subsystem/SplineSystem.cpp
        core/util/PostColorLut.cpp
        core/util/SubscriptionOwnerIndex.cpp
        renders/sokol/SokolPassPreview.cpp
        renders/sokol/SokolPassRecorder.cpp)
jasperengine_target_unity_build(jasperengine)
jasperengine_target_pch(jasperengine)

# the engine's sources include the components they use (registry/EntityRegistry.h); a game's own
# code, built without it, keeps all of them through Scene.h
target_compile_definitions(jasperengine PRIVATE JASPERENGINE_EXPLICIT_COMPONENT_INCLUDES)

target_compile_definitions(jasperengine
    PUBLIC
        $<$<BOOL:${JASPERENGINE_PHYSICS_2D}>:JASPERENGINE_PHYSICS_2D>
        $<$<BOOL:${JASPERENGINE_PHYSICS_3D}>:JASPERENGINE_PHYSICS_3D>
        JASPERENGINE_PROFILER=$<BOOL:${JASPERENGINE_PROFILER}>
        # RenderBackendTime (the graphics backend's share of a frame): the perf tests' alone, an
        # empty type in every other build (a backend call pays nothing for it)
        JASPERENGINE_BACKEND_TIME=$<OR:$<BOOL:${JASPERENGINE_TESTS}>,$<BOOL:${JASPERENGINE_HTMLUI_TESTS}>>
        JASPERENGINE_AUDIO_CAPTURE=$<BOOL:${JASPERENGINE_AUDIO_CAPTURE}>
        JASPERENGINE_DEVELOPMENT_BUILD=$<OR:$<BOOL:${JASPERENGINE_DEVELOPMENT_BUILD}>,$<CONFIG:Debug>>
        JASPERENGINE_TRACY=$<BOOL:${JASPERENGINE_TRACY}>
        JASPERENGINE_NAVIGATION=$<BOOL:${JASPERENGINE_NAVIGATION}>
        JASPERENGINE_SOUND_LAYER=$<BOOL:${JASPERENGINE_SOUND_LAYER}>
        JASPERENGINE_HTMLUI=$<BOOL:${JASPERENGINE_HTMLUI}>
        JASPERENGINE_VIDEO=$<BOOL:${JASPERENGINE_VIDEO}>
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

# The HTTP and video backends' system libraries
if(WIN32)
    # iphlpapi: whether a network is up at all ("offline"). Media Foundation (the video decoders):
    # mfplat.dll and mfreadwrite.dll are loaded at run time (Windows N editions lack them without
    # the Media Feature Pack, platform/win/MediaFoundationBackend.cpp); mfuuid is static GUIDs
    target_link_libraries(jasperengine PRIVATE winhttp iphlpapi mfuuid ole32)
elseif(APPLE)
    target_link_libraries(jasperengine PRIVATE "-framework Foundation" "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    # NDK media (API 21; the API 28 custom data source is looked up with dlsym)
    target_link_libraries(jasperengine PRIVATE mediandk)
elseif(NOT EMSCRIPTEN)
    # dlopen of libcurl and of FFmpeg
    target_link_libraries(jasperengine PRIVATE ${CMAKE_DL_LIBS})
endif()

# The User-Agent's version ("JasperEngine/<version> (<platform>)"): on this one file only, so a
# new version recompiles nothing else
if(JASPERENGINE_EDITOR_VERSION)
    set_property(SOURCE core/net/Http.cpp core/audio/AudioCapture.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        "JASPERENGINE_VERSION_STRING=\"${JASPERENGINE_EDITOR_VERSION}\"")
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(jasperengine PRIVATE -pthread)
endif()

target_link_libraries(
    jasperengine

    PUBLIC

    sokol lua stb tinyobjloader tinygltf tinyxml2 soloud harfbuzz sheenbidi meshoptimizer lunasvg
    ${JASPERENGINE_HTMLUI_LIBS} ${JASPERENGINE_VIDEO_LIBS} zstd_decompress
    ${JASPERENGINE_PHYSICS_LIBS}
)
if(JASPERENGINE_NAVIGATION)
    target_link_libraries(jasperengine PRIVATE recastnavigation)
endif()
if(JASPERENGINE_BASIS_TRANSCODER)
    target_link_libraries(jasperengine PUBLIC basisu_transcoder)
    target_compile_definitions(jasperengine PUBLIC JASPERENGINE_BASIS_TRANSCODER)
endif()
# the Animator's pose kernels (docs/specs/S21-animation.md R17): SIMD and scalar give the same
# bits only without floating-point contraction; WebAssembly builds the kernels (AnimatorPose.cpp
# only: the others include Jolt, whose SSE detection simd128 would trip) with simd128 like the
# particle kernels (JASPERENGINE_WASM_SIMD)
set(JASPERENGINE_ANIMATOR_SRCS core/animation/AnimatorPose.cpp core/animation/AnimatorIK.cpp core/animation/AnimatorInstance.cpp)
if(MSVC)
    set(JASPERENGINE_ANIMATOR_FLAGS /fp:precise)
else()
    set(JASPERENGINE_ANIMATOR_FLAGS -ffp-contract=off)
endif()
set_source_files_properties(${JASPERENGINE_ANIMATOR_SRCS} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_ANIMATOR_FLAGS}")
# the editor's audio analysis (S36) reads clips through these decoders, FFT and meter: without
# contraction they give it the same samples and numbers on every CPU
set_source_files_properties(core/audio/AudioMeter.cpp PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_ANIMATOR_FLAGS}")
set_source_files_properties(libs/soloud/src/audiosource/wav/stb_vorbis.c libs/soloud/src/audiosource/wav/dr_impl.cpp
    libs/soloud/src/audiosource/wav/soloud_wav.cpp libs/soloud/src/core/soloud_fft.cpp DIRECTORY libs/soloud PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_ANIMATOR_FLAGS}")
if(EMSCRIPTEN)
    if(JASPERENGINE_WASM_SIMD)
        set_source_files_properties(core/animation/AnimatorPose.cpp PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_ANIMATOR_FLAGS};-msimd128")
    else()
        set_source_files_properties(core/animation/AnimatorPose.cpp PROPERTIES COMPILE_DEFINITIONS JASPERENGINE_SIMD_DISABLE)
    endif()
endif()
jasperengine_skip_unity_build(
    REASON "the Animator's pose kernels: no floating-point contraction, per unit"
    SOURCES ${JASPERENGINE_ANIMATOR_SRCS})

# the mip chain and the import processing: no fused multiply-adds, so the levels (and the cook
# cache keys built on them) are the same bytes on every CPU, like stb_image_resize2's
if(NOT MSVC)
    set_source_files_properties(core/texture/MipChain.cpp core/texture/TexturePrepare.cpp
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
# Every player links the limiter (S38 6.6), and most run it on one bus, with room to spare in its budget
# (R17): it is built for size. Its result does not depend on it: no fast-math, no contraction here.
if(NOT MSVC)
    set_source_files_properties(core/audio/AudioEffects.cpp PROPERTIES COMPILE_OPTIONS "-Os")
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
    ${JASPERENGINE_SOURCE_GLOB}/shaderlib/library/*.vert
    ${JASPERENGINE_SOURCE_GLOB}/shaderlib/library/*.frag
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
        ${JASPERENGINE_SOURCE_GLOB}/shaderlib/*.glsl
        ${JASPERENGINE_SOURCE_GLOB}/shaderlib/*.vert
        ${JASPERENGINE_SOURCE_GLOB}/shaderlib/*.frag
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

# C++ scripts link only the editor's shared engine, so it carries all of Box2D and Jolt and exports
# them: built with their export macros (the engine's own sources too, so every object agrees on
# Jolt's shared-library declarations), every archive member linked, and whoever links the engine
# (the editor, its tests; generated projects through Generator::physicsCompileDefinitions) compiles
# with the import macros (MSVC links data such as JPH::Allocate only through dllimport)
if(JASPERENGINE_SHARED AND JASPERENGINE_PHYSICS_2D)
    target_compile_definitions(box2d PRIVATE box2d_EXPORTS)
    target_compile_definitions(jasperengine PRIVATE box2d_EXPORTS INTERFACE BOX2D_DLL)
    if(MINGW)
        # base.h uses dllexport with MSVC only
        target_compile_options(box2d PRIVATE "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/platform/win/Box2DExport.h\"")
    endif()
    jasperengine_link_whole_archive(jasperengine box2d)
endif()
if(JASPERENGINE_SHARED AND JASPERENGINE_PHYSICS_3D)
    target_compile_definitions(joltphysics PRIVATE JPH_SHARED_LIBRARY JPH_BUILD_SHARED_LIBRARY)
    target_compile_definitions(jasperengine PRIVATE JPH_BUILD_SHARED_LIBRARY PUBLIC JPH_SHARED_LIBRARY)
    if(MINGW)
        # Core.h uses dllexport with MSVC only
        target_compile_options(joltphysics PRIVATE "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/platform/win/JoltExport.h\"")
    endif()
    jasperengine_link_whole_archive(jasperengine joltphysics)
endif()

# =============================================================
# STANDALONE EXECUTABLE BUILD
# Only runs when this file is used as the top-level build.
# =============================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)

    jasperengine_glob_escape(PROJECT_ROOT_GLOB "${PROJECT_ROOT}")
    file(GLOB_RECURSE PROJECT_SOURCE ${PROJECT_ROOT_GLOB}/*.cpp)
    # the native plugins' platform folders belong to Gradle / Xcode (and the macOS block the
    # export adds); plugins/native/cpp ships as a script root under scripts/
    foreach(native_source IN LISTS PROJECT_SOURCE)
        string(FIND "${native_source}" "${PROJECT_ROOT}/plugins/native/" native_at)
        if(native_at EQUAL 0)
            list(REMOVE_ITEM PROJECT_SOURCE "${native_source}")
        endif()
    endforeach()

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

    if(JASPERENGINE_PLAYER AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # the prebuilt iOS player is a static library, main() included, that the runtime's Xcode
        # template links (tools/players/ios_player.sh makes JasperEnginePlayer.xcframework of it)
        add_library(
            ${APP_NAME}
            STATIC
            ${all_code_files}
        )
    elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
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

    if(JASPERENGINE_PLAYER AND EMSCRIPTEN)
        # the web runtime ships both builds side by side (R15): their .js name their own .wasm
        if(EMSCRIPTEN_THREAD_SUPPORT)
            set_target_properties(${APP_NAME} PROPERTIES OUTPUT_NAME jasperengine-player-mt)
        else()
            set_target_properties(${APP_NAME} PROPERTIES OUTPUT_NAME jasperengine-player-st)
        endif()
    endif()

    if(WIN32)
        include(${JASPERENGINE_ROOT}/platform/win/WindowsManifest.cmake)
        jasperengine_add_windows_manifest(${APP_NAME})
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        jasperengine_skip_unity_build(
            REASON "Xlib.h defines None, Bool, Status, Always... as macros, which break every later source of the batch"
            SOURCES ${JASPERENGINE_ROOT}/platform/linux/JasperEngineLinux.cpp ${JASPERENGINE_ROOT}/platform/linux/WindowLinux.cpp
                    ${JASPERENGINE_ROOT}/platform/linux/X11Visibility.cpp
                    ${JASPERENGINE_ROOT}/platform/linux/LinuxInputRouter.cpp ${JASPERENGINE_ROOT}/platform/linux/SystemLinux.cpp)
    endif()
    # the shipped binary stripped, its debug information in symbols/ (JASPERENGINE_SPLIT_SYMBOLS)
    get_target_property(jasperengineAppType ${APP_NAME} TYPE)
    if(JASPERENGINE_SPLIT_SYMBOLS AND NOT ANDROID AND jasperengineAppType STREQUAL "EXECUTABLE")
        set(jasperengineSymbols "${CMAKE_BINARY_DIR}/symbols")
        if(MSVC)
            set_target_properties(${APP_NAME} PROPERTIES PDB_OUTPUT_DIRECTORY "${jasperengineSymbols}")
        elseif(APPLE)
            add_custom_command(TARGET ${APP_NAME} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${jasperengineSymbols}"
                COMMAND dsymutil "$<TARGET_FILE:${APP_NAME}>" -o "${jasperengineSymbols}/$<TARGET_FILE_NAME:${APP_NAME}>.dSYM"
                COMMAND strip -S -x "$<TARGET_FILE:${APP_NAME}>"
                COMMENT "Splitting the symbols of ${APP_NAME} into symbols/"
                VERBATIM)
        elseif(CMAKE_OBJCOPY AND CMAKE_STRIP)
            set(jasperengineDebug "${jasperengineSymbols}/$<TARGET_FILE_NAME:${APP_NAME}>.debug")
            add_custom_command(TARGET ${APP_NAME} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${jasperengineSymbols}"
                COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:${APP_NAME}>" "${jasperengineDebug}"
                COMMAND "${CMAKE_STRIP}" --strip-debug --strip-unneeded "$<TARGET_FILE:${APP_NAME}>"
                COMMAND "${CMAKE_OBJCOPY}" "--add-gnu-debuglink=${jasperengineDebug}" "$<TARGET_FILE:${APP_NAME}>"
                COMMENT "Splitting the symbols of ${APP_NAME} into symbols/"
                VERBATIM)
        else()
            message(WARNING "JASPERENGINE_SPLIT_SYMBOLS: no objcopy and strip, ${APP_NAME} keeps its symbols")
        endif()
    endif()

    jasperengine_target_warnings(${APP_NAME})
    jasperengine_target_unity_build(${APP_NAME})
    jasperengine_target_pch(${APP_NAME})
    # the project's runtime settings (above), for its platform main and its own sources
    target_compile_definitions(${APP_NAME} PRIVATE ${JASPERENGINE_APP_DEFINITIONS})
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
                jasperengine_glob_escape(preloadGlob "${PROJECT_ROOT}/${preloadDir}")
                file(GLOB_RECURSE preloadFiles CONFIGURE_DEPENDS "${preloadGlob}/*")
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

# je-bench-anim as WebAssembly, run under node by tools/perf/bench-anim-wasm.sh (docs/perf/animator.md,
# S77 R11); JASPERENGINE_WASM_SIMD says whether the pose kernels are built with -msimd128
option(JASPERENGINE_WASM_BENCH "Build je-bench-anim for Emscripten and node" OFF)
if(JASPERENGINE_WASM_BENCH AND EMSCRIPTEN AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(je-bench-anim ${CMAKE_CURRENT_SOURCE_DIR}/tests/core/bench/AnimBench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/core/support/AllocationCounter.cpp)
    target_link_libraries(je-bench-anim PRIVATE jasperengine)
    target_include_directories(je-bench-anim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/core/support)
    target_compile_definitions(je-bench-anim PRIVATE JASPER_TEST_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
    jasperengine_apply_cxx_standard(je-bench-anim)
    set_target_properties(je-bench-anim PROPERTIES
        SUFFIX ".js"
        LINK_FLAGS "--profiling-funcs -s ENVIRONMENT=node -s NODERAWFS=1 -s EXIT_RUNTIME=1 -s INITIAL_MEMORY=256MB -s STACK_SIZE=4MB ${ALLOW_MEMORY_GROWTH} ${USE_WEBGL2}")
endif()

if(JASPERENGINE_ECS_BENCH)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../tools/performance/EcsBench.cmake)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    jasperengine_check_cxx_standards()
endif()

