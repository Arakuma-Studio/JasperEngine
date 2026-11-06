// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Generator.h"
#include "object/FluidSimulation.h"
#include "component/PrefabComponent.h"
#include "util/NativePlugins.h"
#include "Factory.h"
#include "App.h"
#include "EditorVersion.h"
#include "editor/Out.h"
#include "AppSettings.h"
#include "util/CxxStandards.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "util/ShellEnv.h"
#include "util/Util.h"
#include "render/shadow/ShadowTiers.h"
#include "render/LightingTiers.h"
#include "engine_lua_stubs.h"
#include "util/ProjectChecks.h"
#include "util/EditorFingerprint.h"
#include "util/EditorScripts.h"
#include "util/LuaApiData.h"
#include "util/MsBuildProgress.h"
#include "util/HtmlUiExamples.h"
#include "json.hpp"
#include "lua.h"
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Collision/ObjectLayer.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <stdexcept>
#include <chrono>
#include <fstream>
#include <thread>
#include <cstring>
#include <cerrno>
#include <map>
#include <unordered_set>
#include <algorithm>
#include <functional>
#include <regex>
#include <set>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
#else
    #include <signal.h>
    #include <unistd.h>
    #include <sys/wait.h>
    #include <fcntl.h>

    extern char **environ;
#endif

using namespace jasperengine;

std::string editor::Generator::getGeneratorPlatform(const std::string& generator) {
#ifdef _WIN32
    if (generator.compare(0, 14, "Visual Studio ") != 0) return "";
#if defined(_M_ARM64) || defined(__aarch64__)
    return "ARM64";
#elif defined(_M_ARM) || defined(__arm__)
    return "ARM";
#else
    return sizeof(void*) == 8 ? "x64" : "Win32";
#endif
#else
    (void)generator;
    return "";
#endif
}

fs::path editor::Generator::getGeneratedPath(const fs::path& projectInternalPath) {
    return projectInternalPath / "generated";
}

editor::Generator::Generator() :
    lastBuildSucceeded(false)
{
}

editor::Generator::~Generator() {
    // the build job uses this generator: cancel it and wait for it to end
    cancelBuild();
    waitForBuildToComplete();
}

bool editor::Generator::cleanBuildDirectory(const fs::path& buildPath) {
    // the editor's build tree (.jasperengine/build), refused to agents by the deny-by-default guard
    safewrite::EditorInternal ownBuild(buildPath);
    std::error_code ec;

    // Best-effort full wipe first.
    FileUtils::removeAll(buildPath, ec);

    // CMakeCache.txt and CMakeFiles/ are what actually pin the previous
    // generator/compiler. If the full wipe above was blocked partway by a
    // locked file elsewhere in the tree (a loaded plugin DLL, an IDE's
    // CMake Tools extension, an antivirus scan), make sure at least these are
    // gone, retrying briefly in case the lock is transient.
    const fs::path cacheFile = buildPath / "CMakeCache.txt";
    const fs::path cacheDir  = buildPath / "CMakeFiles";
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!fs::exists(cacheFile) && !fs::exists(cacheDir)) {
            break;
        }
        FileUtils::removePath(cacheFile, ec);
        FileUtils::removeAll(cacheDir, ec);
        if (!fs::exists(cacheFile) && !fs::exists(cacheDir)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    FileUtils::createDirectories(buildPath, ec);

    if (fs::exists(cacheFile)) {
        Out::error("Could not remove the previous CMake cache at: %s", cacheFile.string().c_str());
        Out::error("The build directory is locked by another program. Close any editor or IDE using it (e.g. the VS Code \"CMake Tools\" extension), stop any running build, then delete the '.jasperengine/build' folder and try again.");
        return false;
    }
    return true;
}

bool editor::Generator::clearStaleCMakeCache(const fs::path& projectPath, const fs::path& buildPath) {
    safewrite::EditorInternal ownBuild(buildPath);   // the editor's build tree (util/SafeWrite.h)
    fs::path cacheFile = buildPath / "CMakeCache.txt";
    if (!fs::exists(cacheFile)) {
        return true;
    }

    std::ifstream cache(cacheFile);
    if (!cache.is_open()) {
        return true;
    }

    std::string line;
    std::string cachedHomeDir;
    while (std::getline(cache, line)) {
        // Look for CMAKE_HOME_DIRECTORY:INTERNAL=<path>
        const std::string prefix = "CMAKE_HOME_DIRECTORY:INTERNAL=";
        if (line.compare(0, prefix.size(), prefix) == 0) {
            cachedHomeDir = line.substr(prefix.size());
            break;
        }
    }
    cache.close();

    if (cachedHomeDir.empty()) {
        return true;
    }

    // Compare canonical paths to handle trailing slashes, symlinks, etc.
    std::error_code ec;
    fs::path cachedPath = fs::canonical(cachedHomeDir, ec);
    if (ec) {
        // Old path no longer exists — definitely stale
        cachedPath = fs::path(cachedHomeDir);
    }
    fs::path currentPath = fs::canonical(projectPath, ec);
    if (ec) {
        currentPath = projectPath;
    }

    if (cachedPath != currentPath) {
        // expected after a copy or a move of the project: the build just starts over
        Out::info("Project path changed (was %s, now %s). Cleaning build directory...", cachedHomeDir.c_str(),
                  projectPath.string().c_str());

        return cleanBuildDirectory(buildPath);
    }
    return true;
}

const editor::CMakeKit* editor::Generator::chooseDefaultKit(const std::vector<CMakeKit>& kits) {
    const CMakeKit* chosen = nullptr;
    for (const auto& k : kits) {
        if (!k.available) continue;
        // Prefer MSVC on an MSVC editor, including the explicit Ninja/cl
        // fallback. A MinGW editor has already filtered these kits out.
        if (k.cxxCompiler.empty() || fs::path(k.cxxCompiler).stem() == "cl") {
            return &k;
        }
        if (!chosen) chosen = &k;
    }
    return chosen;
}

bool editor::Generator::resolveDefaultKit(std::string& cCompiler, std::string& cxxCompiler, std::string& generator) {
    // Only a "Default" selection (everything empty) is resolved; an explicit kit
    // is left untouched.
    if (!cCompiler.empty() || !cxxCompiler.empty() || !generator.empty()) {
        return true;
    }

    // Windows-only: a bare cmake lets CMake auto-detect a toolchain that may not
    // match the editor's C++ ABI (e.g. a MinGW editor would get MSVC and fail to
    // link). Resolve Default to the best ABI-compatible detected kit so it obeys
    // the same ABI check as the Editor Settings dropdown. Other platforms have a
    // single C++ ABI, so CMake's auto-detection is always safe and left alone.
    bool resolveDefault = false;
#ifdef _WIN32
    resolveDefault = true;
#endif
    if (!resolveDefault) {
        return true;
    }

    std::vector<CMakeKit> kits = detectAvailableKits();
    const CMakeKit* chosen = chooseDefaultKit(kits);
    if (chosen) {
        cCompiler = chosen->cCompiler;
        cxxCompiler = chosen->cxxCompiler;
        generator = chosen->generator;
        if (!chosen->displayName.empty()) {
            Out::info("Default compiler resolved to: %s", chosen->displayName.c_str());
        }
        return true;
    }
    Out::error("No compatible C++ build toolchain was found for this editor. Select an available compiler in Preferences > External Tools.");
    return false;
}

bool editor::Generator::configureCMake(const fs::path& projectPath, const fs::path& buildPath, const std::string& configType, const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator, unsigned int parallelJobs) {
    // the build tree and its .jasperengine_kit marker (a refused marker would make the next compile
    // clean the whole build directory): the editor's own (util/SafeWrite.h, EditorInternal)
    safewrite::EditorInternal ownBuild(buildPath);
    if (!clearStaleCMakeCache(projectPath, buildPath)) {
        return false;
    }

    const fs::path exePath = FileUtils::getExecutableDir();
    const std::string platform = getGeneratorPlatform(generator);
    std::string currentKit = generator + "\n" + cCompiler + "\n" + cxxCompiler + "\n" + exePath.string();
    if (!platform.empty()) currentKit += "\n" + platform;

    // Detect kit change: if the compiler/generator selection changed since the
    // last configure, the build tree must be wiped (CMake does not support
    // switching generators or compilers in-place). The editor engine library
    // directory participates too: find_library() caches absolute paths, so a
    // moved/updated editor install must force a fresh library lookup instead
    // of retaining an old libjasperengine path.
    {
        fs::path kitMarker = buildPath / ".jasperengine_kit";
        fs::path cacheFile = buildPath / "CMakeCache.txt";
        if (fs::exists(kitMarker)) {
            std::ifstream f(kitMarker);
            std::string prevKit((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();
            if (prevKit != currentKit) {
                Out::warning("Compiler kit or engine library directory changed. Cleaning build directory...");
                if (!cleanBuildDirectory(buildPath)) {
                    return false;
                }
            }
        } else if (fs::exists(cacheFile)) {
            std::ifstream cache(cacheFile);
            std::string line;
            std::string cachedGenerator;
            std::string cachedCCompiler;
            std::string cachedCxxCompiler;

            while (std::getline(cache, line)) {
                const std::string genPrefix = "CMAKE_GENERATOR:INTERNAL=";
                const std::string cPrefix = "CMAKE_C_COMPILER:FILEPATH=";
                const std::string cxxPrefix = "CMAKE_CXX_COMPILER:FILEPATH=";

                if (line.compare(0, genPrefix.size(), genPrefix) == 0) {
                    cachedGenerator = line.substr(genPrefix.size());
                } else if (line.compare(0, cPrefix.size(), cPrefix) == 0) {
                    cachedCCompiler = line.substr(cPrefix.size());
                } else if (line.compare(0, cxxPrefix.size(), cxxPrefix) == 0) {
                    cachedCxxCompiler = line.substr(cxxPrefix.size());
                }
            }

            bool kitChanged = false;
            if (!generator.empty()) {
                kitChanged = cachedGenerator != generator;
            } else if (!cachedGenerator.empty() && !cCompiler.empty()) {
                kitChanged = true;
            }
            if (!kitChanged && !cCompiler.empty()) {
                kitChanged = cachedCCompiler != cCompiler;
            }
            if (!kitChanged && !cxxCompiler.empty()) {
                kitChanged = cachedCxxCompiler != cxxCompiler;
            }
            if (!kitChanged && cCompiler.empty() && cxxCompiler.empty()) {
                kitChanged = !cachedCCompiler.empty() || !cachedCxxCompiler.empty();
            }

            if (kitChanged) {
                Out::warning("Compiler kit or engine library directory changed. Cleaning build directory...");
                if (!cleanBuildDirectory(buildPath)) {
                    return false;
                }
            }
        }
    }

    // Safety net: even when the kit bookkeeping above is out of sync (e.g. a
    // previous configure left a cache the marker does not describe), a
    // generator recorded in the cache that differs from the one we are about to
    // request is fatal to CMake ("Does not match the generator used
    // previously"). Force a clean so the configure can actually proceed.
    if (!generator.empty()) {
        fs::path cacheFile = buildPath / "CMakeCache.txt";
        std::ifstream cache(cacheFile);
        if (cache.is_open()) {
            const std::string genPrefix = "CMAKE_GENERATOR:INTERNAL=";
            const std::string platformPrefix = "CMAKE_GENERATOR_PLATFORM:INTERNAL=";
            std::string line;
            std::string cachedGenerator;
            std::string cachedPlatform;
            while (std::getline(cache, line)) {
                if (line.compare(0, genPrefix.size(), genPrefix) == 0) {
                    cachedGenerator = line.substr(genPrefix.size());
                } else if (line.compare(0, platformPrefix.size(), platformPrefix) == 0) {
                    cachedPlatform = line.substr(platformPrefix.size());
                }
            }
            cache.close();
            if ((!cachedGenerator.empty() && cachedGenerator != generator) ||
                (!platform.empty() && cachedPlatform != platform)) {
                Out::warning("CMake generator or architecture changed. Cleaning build directory...");
                if (!cleanBuildDirectory(buildPath)) {
                    return false;
                }
            }
        }
    }

#ifdef _WIN32
    // A compiler override without an explicit generator makes CMake fall back
    // to the Visual Studio generator, which ignores CMAKE_C/CXX_COMPILER
    // (MSBuild always drives cl.exe) and fails at link time. Fail early with
    // an actionable message instead. Honor CMAKE_GENERATOR if the user set it.
    if (generator.empty() && (!cCompiler.empty() || !cxxCompiler.empty())) {
        const char* envGenerator = std::getenv("CMAKE_GENERATOR");
        if (!envGenerator || !*envGenerator) {
            Out::error("The selected compiler '%s' cannot be used with the default Visual Studio generator. Install Ninja (https://ninja-build.org), add it to PATH and re-select the compiler in Preferences > External Tools, or switch to the MSVC compiler.",
                (!cxxCompiler.empty() ? cxxCompiler : cCompiler).c_str());
            return false;
        }
    }
#endif

    // CMake stores path values (e.g. CMAKE_C_COMPILER) into generated .cmake
    // files and re-parses them, treating backslashes as escape sequences. On
    // Windows a path like "C:\clang...\bin\clang.exe" then fails with
    // "Invalid character escape '\c'". CMake accepts forward slashes on every
    // platform, so normalize backslashes before building the command.
    auto toCMakePath = [](const std::string& p) {
        std::string out = p;
        std::replace(out.begin(), out.end(), '\\', '/');
        return out;
    };

    // A CMake generator name an agent could have set must be one CMake actually knows: an arbitrary
    // string is refused (never pasted into a command line). "" is CMake's platform default.
    if (!generator.empty() && !isKnownGenerator(generator)) {
        Out::error("Unknown CMake generator \"%s\"; set it in Preferences > External Tools to one CMake supports (e.g. Ninja, Unix Makefiles).", generator.c_str());
        return false;
    }

    // Launched as an argv array (CommandRunner::run), never through a shell: a project path or a
    // setting that holds shell metacharacters ($(...), ;, ") is one literal argument, not a command.
    std::vector<std::string> cmakeArgv = { cmakeProgram() };
    if (!generator.empty()) { cmakeArgv.push_back("-G"); cmakeArgv.push_back(generator); }
    if (!platform.empty()) { cmakeArgv.push_back("-A"); cmakeArgv.push_back(platform); }
    if (!cCompiler.empty()) cmakeArgv.push_back("-DCMAKE_C_COMPILER=" + toCMakePath(cCompiler));
    if (!cxxCompiler.empty()) cmakeArgv.push_back("-DCMAKE_CXX_COMPILER=" + toCMakePath(cxxCompiler));
    cmakeArgv.push_back("-DCMAKE_BUILD_TYPE=" + configType);
    // When configuring from inside the editor, ensure the generated project builds
    // in "plugin" mode (no Factory main.cpp/scene sources added).
    cmakeArgv.push_back("-DJASPERENGINE_EDITOR_PLUGIN=ON");
    // The generated CMakeLists.txt stays machine-independent; the effective
    // local limit is supplied through the build tree's cache on every configure.
    cmakeArgv.push_back("-DJASPERENGINE_PARALLEL_BUILD_JOBS=" + std::to_string(parallelJobs));
    cmakeArgv.push_back(toCMakePath(projectPath.string()));
    cmakeArgv.push_back("-B");
    cmakeArgv.push_back(toCMakePath(buildPath.string()));
    cmakeArgv.push_back("-DJASPERENGINE_LIB_DIR=" + toCMakePath(exePath.string()));

    Out::info("Configuring CMake project (%s)...", cmakeProgram().c_str());
    // Windows + Ninja: the MSVC environment vcvars sets (loaded once, cached), cmake launched
    // directly with it: no shell sees the project path or a build setting
    CommandRunner::Environment msvcEnv;
    std::string msvcError;
    if (!CommandRunner::msvcEnvironment(generator, msvcEnv, msvcError)) {
        Out::error("%s", msvcError.c_str());
        return false;
    }
    bool result = commandRunner.run(cmakeArgv, projectPath, {}, msvcEnv.empty() ? nullptr : &msvcEnv);

    // Record which kit was used so we can detect changes next time.
    if (result) {
        std::error_code ec;
        FileUtils::createDirectories(buildPath, ec);
        // (in a write scope: beneath its root)
        FileUtils::writeFile(buildPath / ".jasperengine_kit", currentKit);
    }

    return result;
}

unsigned int editor::Generator::getAutomaticParallelBuildJobs() {
    return std::max(std::thread::hardware_concurrency(), 1u);
}

unsigned int editor::Generator::getMaxParallelBuildJobs() {
    constexpr unsigned int maxJobsPerLogicalCpu = 4;
    // Keeping the shared limit within MAX_SUPPORTED_PARALLEL_BUILD_JOBS lets
    // the same effective value drive every generator.
    const unsigned long long scaledJobs =
        static_cast<unsigned long long>(getAutomaticParallelBuildJobs()) * maxJobsPerLogicalCpu;
    return static_cast<unsigned int>(std::min(
        scaledJobs,
        static_cast<unsigned long long>(MAX_SUPPORTED_PARALLEL_BUILD_JOBS)
    ));
}

bool editor::Generator::buildProject(const fs::path& buildPath, const std::string& configType, const std::string& generator, unsigned int parallelJobs) {
    std::vector<std::string> buildArgv = { cmakeProgram(), "--build", buildPath.string(),
                                           "--config", configType, "--parallel", std::to_string(parallelJobs) };
    Out::info("Building project with %u parallel jobs...", parallelJobs);
    // The build step invokes the compiler/linker again, so it needs the same
    // MSVC environment as configure (see CommandRunner::msvcEnvironment). Launched as argv (no
    // shell). Its lines go to the Console as before, and their progress to the script status
    // (S23 R17).
    CommandRunner::Environment msvcEnv;
    std::string msvcError;
    if (!CommandRunner::msvcEnvironment(generator, msvcEnv, msvcError)) {
        Out::error("%s", msvcError.c_str());
        return false;
    }
    MsBuildProgress msbuild(buildPath, configType);
    const std::string row = statusRow;
    float lastFraction = -1.0f;
    return commandRunner.run(buildArgv, buildPath, [&](const std::string& line) {
        Out::build(line);
        float fraction = -1.0f;
        if (msbuild.consumeLine(line, fraction) || parseBuildProgress(line, fraction)) {
            if (fraction - lastFraction >= 0.01f || fraction >= 1.0f) {
                lastFraction = fraction;
                ScriptBuildEvent event;
                event.kind = ScriptBuildEvent::Kind::Progress;
                event.row = row;
                event.fraction = fraction;
                buildEvents().invoke(event);
            }
        }
    }, msvcEnv.empty() ? nullptr : &msvcEnv);
}

// The Jolt configuration the editor's engine library was built with, as compile definitions.
// It changes Jolt's class layouts (the engine's PhysicsSystem holds a JPH::PhysicsSystem by
// value), so scripts linking that library compile Jolt's headers the same way: the object
// layer size, and JPH_DEBUG, which Jolt otherwise derives from the script's own NDEBUG.
static std::string joltCompileDefinitions() {
    std::string definitions = "JPH_OBJECT_LAYER_BITS=" + std::to_string(JPH_OBJECT_LAYER_BITS);
#ifdef JPH_DEBUG
    // empty, as Jolt defines it, so its own definition is no redefinition
    definitions += " JPH_DEBUG=";
#else
    definitions += " JPH_NO_DEBUG";
#endif
#ifdef JPH_ENABLE_ASSERTS
    definitions += " JPH_ENABLE_ASSERTS";
#endif
#ifdef JPH_DEBUG_RENDERER
    definitions += " JPH_DEBUG_RENDERER";
#endif
#ifdef JPH_PROFILE_ENABLED
    definitions += " JPH_PROFILE_ENABLED";
#endif
#ifdef JPH_DOUBLE_PRECISION
    definitions += " JPH_DOUBLE_PRECISION";
#endif
#ifdef JPH_SHARED_LIBRARY
    definitions += " JPH_SHARED_LIBRARY";
#endif
    return definitions;
}

// Relative to ${PROJECT_ROOT} so the generated CMakeLists.txt reads the same on every
// checkout. A path configured outside the project has no such spelling.
static std::string toProjectRootCMakePath(const fs::path& projectPath, const fs::path& target) {
    std::error_code ec;
    const fs::path relative = fs::relative(target, projectPath, ec);
    if (ec || relative.empty() || *relative.begin() == "..") {
        return target.generic_string();
    }
    if (relative == ".") {
        return "${PROJECT_ROOT}";
    }
    return "${PROJECT_ROOT}/" + relative.generic_string();
}

std::string editor::Generator::getPlatformCMakeConfig(bool vsyncEnabled, const WindowSettings& windowSettings, const fs::path& projectPath, const fs::path& assetsPath, const fs::path& luaPath) {
    // The title crosses two quoting layers: the generated CMake string and the
    // C string literal passed to the platform backend. Escape both, matching the
    // exported-project configuration produced by Exporter.
    std::string title;
    for (char c : windowSettings.title) {
        if (c == '\\' || c == '"') title += '\\';
        title += c;
    }
    std::string cmakeTitle;
    for (char c : title) {
        if (c == '\\' || c == '"' || c == '$') cmakeTitle += '\\';
        cmakeTitle += c;
    }

    std::string content;
    content += "if (NOT JASPERENGINE_EDITOR_PLUGIN)\n";
    content += "    add_definitions(\"-DJASPERENGINE_VSYNC_ENABLED=" + std::string(vsyncEnabled ? "1" : "0") + "\")\n";
    content += "    add_definitions(\"-DDEFAULT_WINDOW_WIDTH=" + std::to_string(windowSettings.width) + "\")\n";
    content += "    add_definitions(\"-DDEFAULT_WINDOW_HEIGHT=" + std::to_string(windowSettings.height) + "\")\n";
    content += "    add_definitions(\"-DJASPERENGINE_WINDOW_MODE=" + std::to_string(static_cast<int>(windowSettings.mode)) + "\")\n";
    content += "    add_definitions(\"-DJASPERENGINE_WINDOW_RESIZABLE=" + std::string(windowSettings.resizable ? "1" : "0") + "\")\n";
    content += "    add_definitions(\"-DJASPERENGINE_WINDOW_TITLE=\\\"" + cmakeTitle + "\\\"\")\n";
    content += "\n";
    content += "\n";
    content += "    add_definitions(\"-DWITH_MINIAUDIO\") # For SoLoud\n";
    content += "\n";
    // Resolved from the project directory rather than written out absolute, so a
    // teammate who clones elsewhere still finds the assets
    content += "    add_definitions(\"-DJASPERENGINE_ASSET_PATH=\\\"" + toProjectRootCMakePath(projectPath, assetsPath) + "\\\"\")\n";
    content += "    add_definitions(\"-DJASPERENGINE_LUA_PATH=\\\"" + toProjectRootCMakePath(projectPath, luaPath) + "\\\"\")\n";
    // shader forks are named by their project-relative paths, inside the assets directory or not
    // (the generated main.cpp reads them from here first, as Project::findShaderFile does)
    content += "    add_definitions(\"-DJASPERENGINE_PROJECT_PATH=\\\"${PROJECT_ROOT}\\\"\")\n";
    content += "\n";
    content += "    # The compiled shader cache sits in this user's cache directory and is named\n";
    content += "    # after the engine version, so its path belongs to neither the project nor any\n";
    content += "    # other machine. The editor writes it to ${INTERNAL_DIR}/LocalPaths.cmake, which\n";
    content += "    # version control ignores; -DJASPERENGINE_SHADER_PATH=<dir> overrides it.\n";
    content += "    include(${INTERNAL_DIR}/LocalPaths.cmake OPTIONAL)\n";
    content += "    if(NOT JASPERENGINE_SHADER_PATH)\n";
    content += "        message(FATAL_ERROR \"JASPERENGINE_SHADER_PATH is not set and ${INTERNAL_DIR}/LocalPaths.cmake is missing. Open this project in JasperEngine Editor once to regenerate it, or configure with -DJASPERENGINE_SHADER_PATH=<shader cache directory>.\")\n";
    content += "    endif()\n";
    content += "    add_definitions(\"-DJASPERENGINE_SHADER_PATH=\\\"${JASPERENGINE_SHADER_PATH}\\\"\")\n";
    content += "\n";
    content += "    list(APPEND PLATFORM_SOURCE\n";
    content += "        ${INTERNAL_DIR}/generated/main.cpp\n";
    content += "    )\n";
    content += "\n";
    content += "    # Each desktop OS uses the same native application backend as the editor,\n";
    content += "    # compiled from the engine API snapshot in ${INTERNAL_DIR}/engine-api.\n";
    // This build links the editor's own engine library, whose sokol_gfx was
    // compiled for a single graphics backend, so the default has to be that one.
#if defined(SOKOL_VULKAN)
    const std::string editorGraphicBackend = "vulkan";
#elif defined(SOKOL_METAL)
    const std::string editorGraphicBackend = "metal";
#else
    const std::string editorGraphicBackend = "glcore";
#endif
    content += "    if(NOT JASPERENGINE_GRAPHIC_BACKEND)\n";
    content += "        set(JASPERENGINE_GRAPHIC_BACKEND \"" + editorGraphicBackend + "\")\n";
    content += "    endif()\n";
    content += "\n";
    content += "\n";
    content += "    # FindVulkan reads the VULKAN_SDK environment variable the SDK installer sets\n";
    content += "    macro(jasperengine_find_vulkan)\n";
    content += "        # sokol_gfx binds every resource through VK_EXT_descriptor_buffer, which\n";
    content += "        # MoltenVK does not implement, so the build only fails at device selection\n";
    content += "        if(APPLE)\n";
    content += "            message(WARNING \"JASPERENGINE_GRAPHIC_BACKEND=vulkan runs on MoltenVK here, which does not implement VK_EXT_descriptor_buffer: this build will fail at startup unless the driver provides it. JASPERENGINE_GRAPHIC_BACKEND=metal is the working macOS backend.\")\n";
    content += "        endif()\n";
    content += "\n";
    content += "        # An app started from the Dock inherits no shell environment, so on macOS\n";
    content += "        # the SDK is also looked for where its installer unpacks it\n";
    content += "        if(APPLE AND NOT DEFINED ENV{VULKAN_SDK})\n";
    content += "            file(GLOB jasperengineVulkanSdkDirs \"$ENV{HOME}/VulkanSDK/*/macOS\")\n";
    content += "            if(jasperengineVulkanSdkDirs)\n";
    content += "                list(SORT jasperengineVulkanSdkDirs)\n";
    content += "                list(POP_BACK jasperengineVulkanSdkDirs jasperengineVulkanSdkNewest)\n";
    content += "                set(ENV{VULKAN_SDK} \"${jasperengineVulkanSdkNewest}\")\n";
    content += "            endif()\n";
    content += "        endif()\n";
    content += "        find_package(Vulkan QUIET)\n";
    content += "        if(NOT Vulkan_FOUND)\n";
    content += "            message(FATAL_ERROR \"JASPERENGINE_GRAPHIC_BACKEND=vulkan needs the Vulkan SDK (https://vulkan.lunarg.com/sdk/home), and VULKAN_SDK is not set here. If it is installed outside the default location, restart the shell or editor this build was started from with VULKAN_SDK in its environment: Windows only gives the variable to processes started after the installer ran. Otherwise use JASPERENGINE_GRAPHIC_BACKEND=glcore, or metal on macOS.\")\n";
    content += "        endif()\n";
    content += "    endmacro()\n";
    content += "\n";
    content += "    set(JASPERENGINE_PLATFORM_DIR ${JASPERENGINE_API_DIR}/platform)\n";
    content += "    include_directories(${JASPERENGINE_PLATFORM_DIR}/win ${JASPERENGINE_PLATFORM_DIR}/linux ${JASPERENGINE_PLATFORM_DIR}/mac ${JASPERENGINE_PLATFORM_DIR}/apple ${JASPERENGINE_PLATFORM_DIR}/common)\n";
    content += "\n";
    content += "    if(WIN32)\n";
    content += "        list(APPEND PLATFORM_SOURCE\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/win/JasperEngineWin.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/win/WindowWin.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/win/SystemWin.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/win/HostServicesWin.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/win/WinInputRouter.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/win/GamepadWin.cpp\n";
    content += "        )\n";
    content += "        # winhttp, iphlpapi: the HTTP client's backend (platform/win/HttpBackendWin.cpp);\n";
    content += "        # ole32, uuid, advapi32: platform services (platform/win/HostServicesWin.cpp)\n";
    content += "        list(APPEND PLATFORM_LIBS gdi32 user32 shell32 imm32 winhttp iphlpapi ole32 uuid advapi32)\n";
    content += "        if(JASPERENGINE_GRAPHIC_BACKEND STREQUAL \"vulkan\")\n";
    content += "            add_definitions(\"-DSOKOL_VULKAN\")\n";
    content += "            add_definitions(\"-DVK_USE_PLATFORM_WIN32_KHR\")\n";
    content += "            list(APPEND PLATFORM_SOURCE ${JASPERENGINE_PLATFORM_DIR}/common/VulkanContext.cpp)\n";
    content += "            jasperengine_find_vulkan()\n";
    content += "            list(APPEND PLATFORM_LIBS Vulkan::Vulkan)\n";
    content += "        else()\n";
    content += "            add_definitions(\"-DSOKOL_GLCORE\")\n";
    content += "            list(APPEND PLATFORM_LIBS opengl32)\n";
    content += "        endif()\n";
    content += "    elseif(APPLE)\n";
    content += "        set(CMAKE_CXX_FLAGS \"${CMAKE_CXX_FLAGS} -fobjc-arc\")\n";
    content += "        set(CMAKE_C_FLAGS \"${CMAKE_C_FLAGS} -fobjc-arc\")\n";
    content += "        list(APPEND PLATFORM_SOURCE\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/apple/JasperEngineGameController.mm\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/mac/JasperEngineMac.mm\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/mac/WindowMac.mm\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/mac/SystemMac.mm\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/mac/HostServicesMac.mm\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/mac/MacInputRouter.mm\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/mac/MacTextInputClient.mm\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/mac/main.mm\n";
    content += "        )\n";
    content += "        list(APPEND PLATFORM_LIBS \"-framework Cocoa\" \"-framework QuartzCore\" \"-framework GameController\")\n";
    content += "        # MTKView has no GL or Vulkan drawable, so those get views of their own\n";
    content += "        if(JASPERENGINE_GRAPHIC_BACKEND STREQUAL \"glcore\")\n";
    content += "            add_definitions(\"-DSOKOL_GLCORE\")\n";
    content += "            list(APPEND PLATFORM_SOURCE ${JASPERENGINE_PLATFORM_DIR}/mac/MacViewGL.mm)\n";
    content += "            list(APPEND PLATFORM_LIBS \"-framework OpenGL\")\n";
    content += "        elseif(JASPERENGINE_GRAPHIC_BACKEND STREQUAL \"vulkan\")\n";
    content += "            add_definitions(\"-DSOKOL_VULKAN\")\n";
    content += "            add_definitions(\"-DVK_USE_PLATFORM_METAL_EXT\")\n";
    content += "            list(APPEND PLATFORM_SOURCE ${JASPERENGINE_PLATFORM_DIR}/mac/MacViewVulkan.mm ${JASPERENGINE_PLATFORM_DIR}/common/VulkanContext.cpp)\n";
    content += "            jasperengine_find_vulkan()\n";
    content += "            list(APPEND PLATFORM_LIBS Vulkan::Vulkan)\n";
    content += "        else()\n";
    content += "            add_definitions(\"-DSOKOL_METAL\")\n";
    content += "            list(APPEND PLATFORM_SOURCE ${JASPERENGINE_PLATFORM_DIR}/mac/MacViewMetal.mm)\n";
    content += "            list(APPEND PLATFORM_LIBS \"-framework Metal\" \"-framework MetalKit\")\n";
    content += "        endif()\n";
    content += "    else()\n";
    content += "        find_package(X11 REQUIRED)\n";
    content += "        list(APPEND PLATFORM_SOURCE\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/linux/JasperEngineLinux.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/linux/WindowLinux.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/linux/SystemLinux.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/linux/HostServicesLinux.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/linux/LinuxInputRouter.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/linux/GamepadLinux.cpp\n";
    content += "            ${JASPERENGINE_PLATFORM_DIR}/linux/GamepadDB.cpp\n";
    content += "        )\n";
    content += "        list(APPEND PLATFORM_LIBS X11::X11 dl m)\n";
    content += "        if(JASPERENGINE_GRAPHIC_BACKEND STREQUAL \"vulkan\")\n";
    content += "            add_definitions(\"-DSOKOL_VULKAN\")\n";
    content += "            add_definitions(\"-DVK_USE_PLATFORM_XLIB_KHR\")\n";
    content += "            list(APPEND PLATFORM_SOURCE ${JASPERENGINE_PLATFORM_DIR}/common/VulkanContext.cpp)\n";
    content += "            jasperengine_find_vulkan()\n";
    content += "            list(APPEND PLATFORM_LIBS Vulkan::Vulkan)\n";
    content += "        else()\n";
    content += "            add_definitions(\"-DSOKOL_GLCORE\")\n";
    content += "            list(APPEND PLATFORM_LIBS GL)\n";
    content += "        endif()\n";
    content += "    endif()\n";
    content += "endif() \n";
    return content;
}

std::string editor::Generator::buildInitSceneScriptsSource(const std::vector<SceneScriptSource>& scriptFiles) {
    std::string sourceContent;

    sourceContent += "\n";
    sourceContent += "using namespace jasperengine;\n";

    sourceContent += "\n";
    sourceContent += "#if defined(_MSC_VER)\n";
    sourceContent += "    #define PROJECT_API __declspec(dllexport)\n";
    sourceContent += "#else\n";
    sourceContent += "    #define PROJECT_API\n";
    sourceContent += "#endif\n\n";

    if (!scriptFiles.empty()) {
        // The C++ scripts of one GameObject. A constructor can instantiate (which moves the
        // components), so the component is looked up again after each one
        sourceContent += "static void createCppScripts(jasperengine::Scene* scene, jasperengine::Entity entity) {\n";
        sourceContent += "    jasperengine::ScriptComponent* scriptComp = scene->findComponent<jasperengine::ScriptComponent>(entity);\n";
        sourceContent += "    for (size_t s = 0; scriptComp && s < scriptComp->scripts.size(); s++) {\n";
        sourceContent += "        const jasperengine::ScriptEntry& scriptEntry = scriptComp->scripts[s];\n";
        sourceContent += "        if (scriptEntry.type == ScriptType::LUA || scriptEntry.instance)\n";
        sourceContent += "            continue;\n";
        sourceContent += "\n";
        sourceContent += "        const std::string className = scriptEntry.className;\n";
        sourceContent += "        void* instance = nullptr;\n";
        // a behaviour (ScriptBase) gets the lifecycle and other script messages it overrides
        sourceContent += "        jasperengine::ScriptBase* cppScript = nullptr;\n";
        sourceContent += "        jasperengine::ScriptMessageMask cppMessageMask = 0;\n";
        for (const auto& s : scriptFiles) {
            sourceContent += "        if (className == \"" + s.className + "\") {\n";
            sourceContent += "            " + s.className + "* typed = new " + s.className + "(scene, entity);\n";
            sourceContent += "            instance = static_cast<void*>(typed);\n";
            sourceContent += "            cppScript = jasperengine::ScriptMessages::cppScript<" + s.className + ">(typed);\n";
            sourceContent += "            cppMessageMask = jasperengine::ScriptMessages::cppMask<" + s.className + ">();\n";
            sourceContent += "        }\n";
        }
        sourceContent += "        scriptComp = scene->isEntityCreated(entity) ? scene->findComponent<jasperengine::ScriptComponent>(entity) : nullptr;\n";
        sourceContent += "        if (scriptComp && s < scriptComp->scripts.size()) {\n";
        sourceContent += "            scriptComp->scripts[s].instance = instance;\n";
        sourceContent += "            scriptComp->scripts[s].cppScript = cppScript;\n";
        sourceContent += "            scriptComp->scripts[s].cppMessageMask = cppMessageMask;\n";
        sourceContent += "            jasperengine::ScriptMessages::notifyReceiversChanged(scene, entity);\n";
        sourceContent += "        }\n";
        sourceContent += "    }\n";
        sourceContent += "}\n\n";

        // Their properties: values and the GameObjects they point at
        sourceContent += "static void bindCppScriptProperties(jasperengine::Scene* scene, jasperengine::Entity entity) {\n";
        sourceContent += "    jasperengine::ScriptComponent* scriptCompPtr = scene->findComponent<jasperengine::ScriptComponent>(entity);\n";
        sourceContent += "    if (!scriptCompPtr)\n";
        sourceContent += "        return;\n";
        sourceContent += "    {\n";
        sourceContent += "        for (auto& scriptEntry : scriptCompPtr->scripts) {\n";
        sourceContent += "            if (scriptEntry.type == ScriptType::LUA || !scriptEntry.instance) \n";
        sourceContent += "                continue; \n";
        // bound already (a GameObject a script instantiated while the scene started)
        sourceContent += "            bool bound = false;\n";
        sourceContent += "            for (const auto& prop : scriptEntry.properties) {\n";
        sourceContent += "                if (prop.memberPtr) { bound = true; break; }\n";
        sourceContent += "            }\n";
        sourceContent += "            if (bound)\n";
        sourceContent += "                continue;\n";
        sourceContent += "\n";

        for (const auto& s : scriptFiles) {
            if (s.properties.empty()) {
                continue;
            }
            sourceContent += "            if (scriptEntry.className == \"" + s.className + "\") {\n";

            sourceContent += "                " + s.className + "* typedScript = static_cast<" + s.className + "*>(scriptEntry.instance);\n";
            sourceContent += "\n";
            sourceContent += "                for (auto& prop : scriptEntry.properties) {\n";

            for (const auto& prop : s.properties) {
                sourceContent += "\n";
                sourceContent += "                    if (prop.name == \"" + prop.name + "\") {\n";

                if (prop.isPtr && !prop.ptrTypeName.empty()) {
                    sourceContent += "                        jasperengine::EntityReference entRef;\n";
                    sourceContent += "                        if (std::holds_alternative<jasperengine::EntityReference>(prop.value)) {\n";
                    sourceContent += "                            entRef = std::get<jasperengine::EntityReference>(prop.value);\n";
                    sourceContent += "                        }\n";
                    sourceContent += "                        jasperengine::Entity targetEntity = entRef.entity;\n";
                    sourceContent += "                        void* instancePtr = nullptr;\n";
                    sourceContent += "\n";
                    sourceContent += "                        if (targetEntity != NULL_ENTITY) {\n";
                    sourceContent += "                            jasperengine::Scene* targetScene = scene;\n";
                    sourceContent += "                            if (entRef.sceneId != 0) {\n";
                    sourceContent += "                                targetScene = SceneManager::getScenePtr(entRef.sceneId);\n";
                    sourceContent += "                            }\n";
                    sourceContent += "                            if (!targetScene || !targetScene->isEntityCreated(targetEntity)) {\n";
                    sourceContent += "                                Log::error(\"Script property " + s.className + "::" + prop.name + ": entity %u not found\", targetEntity);\n";
                    sourceContent += "                            } else {\n";
                    sourceContent += "                                jasperengine::ScriptComponent* targetScriptComp = targetScene->findComponent<jasperengine::ScriptComponent>(targetEntity);\n";
                    sourceContent += "                                if (targetScriptComp) {\n";
                    sourceContent += "                                    for (auto& targetScript : targetScriptComp->scripts) {\n";
                    sourceContent += "                                        if (targetScript.type != ScriptType::LUA) {\n";
                    sourceContent += "                                            if (targetScript.className == \"" + prop.ptrTypeName + "\" && targetScript.instance) {\n";
                    sourceContent += "                                                instancePtr = targetScript.instance;\n";
                    sourceContent += "                                                #ifdef JASPERENGINE_EDITOR_PLUGIN\n";
                    sourceContent += "                                                printf(\"[DEBUG]   Found matching C++ script instance: '%s'\\n\", targetScript.className.c_str());\n";
                    sourceContent += "                                                #endif\n";
                    sourceContent += "                                                break;\n";
                    sourceContent += "                                            }\n";
                    sourceContent += "                                        }\n";
                    sourceContent += "                                    }\n";
                    sourceContent += "                                }\n";
                    sourceContent += "\n";
                    if (!prop.ptrTypeName.empty()) {
                        sourceContent += "                                if (!instancePtr) {\n";
                        sourceContent += "                                    #ifdef JASPERENGINE_EDITOR_PLUGIN\n";
                        sourceContent += "                                    printf(\"[DEBUG]   No C++ script instance found, creating '" + prop.ptrTypeName + "' type\\n\");\n";
                        sourceContent += "                                    #endif\n";
                        sourceContent += "                                    instancePtr = new " + prop.ptrTypeName + "(targetScene, targetEntity);\n";
                        sourceContent += "                                    prop.ownedInstance = instancePtr;\n";
                        sourceContent += "                                }\n";
                    }
                    sourceContent += "                            }\n";
                    sourceContent += "                        }\n";
                    sourceContent += "\n";
                    sourceContent += "                        typedScript->" + prop.name + " = nullptr;\n";
                    sourceContent += "                        if (instancePtr) {\n";
                    sourceContent += "                            typedScript->" + prop.name + " = static_cast<" + prop.ptrTypeName + "*>(instancePtr);\n";
                    sourceContent += "                        }\n";
                    sourceContent += "\n";
                }

                sourceContent += "                        prop.memberPtr = &typedScript->" + prop.name + ";\n";
                sourceContent += "                    }\n";
            }

            sourceContent += "\n";
            sourceContent += "                    prop.syncToMember();\n";
            sourceContent += "                }\n";

            sourceContent += "            }\n";
        }

        sourceContent += "\n";
        sourceContent += "        }\n";
        sourceContent += "    }\n";
        sourceContent += "}\n\n";
    }

    // A scene's scripts: every instance (Lua, then C++ made and bound), then their init and
    // onEnable in one pass for both languages (ScriptRuntime), then the HTML documents' scripts
    sourceContent += "extern \"C\" void PROJECT_API initScripts(jasperengine::Scene* scene) {\n";
    sourceContent += "    LuaBinding::createLuaScripts(scene);\n";
    if (!scriptFiles.empty()) {
        sourceContent += "\n";
        // what a constructor instantiates goes to this scene, not yet the Engine's
        sourceContent += "    ObjectManager::pushActiveScene(scene);\n";
        sourceContent += "    const auto& scriptsArray = scene->getComponentArray<ScriptComponent>();\n";
        sourceContent += "    for (size_t i = 0; i < scriptsArray->size(); i++) {\n";
        sourceContent += "        createCppScripts(scene, scriptsArray->getEntity(i));\n";
        sourceContent += "    }\n";
        sourceContent += "    for (size_t i = 0; i < scriptsArray->size(); i++) {\n";
        sourceContent += "        bindCppScriptProperties(scene, scriptsArray->getEntity(i));\n";
        sourceContent += "    }\n";
        sourceContent += "    ObjectManager::popActiveScene();\n";
    }
    sourceContent += "\n";
    sourceContent += "    ScriptRuntime::awakeScene(scene);\n";
    sourceContent += "    scene->getSystem<UIDocumentSystem>()->startDocumentScripts();\n";
    sourceContent += "}\n\n";

    // GameObjects instantiated while the game runs (ObjectManager): their Lua scripts are
    // started by the engine, these are the C++ ones
    // A C++ reload during Play saves the old instances' state and loads it into the new ones
    // through their ScriptBase (S23 R11): the library knows the classes, the editor does not
    sourceContent += "template<class T>\n";
    sourceContent += "static jasperengine::ScriptBase* toScriptBase(T* instance) {\n";
    sourceContent += "    if constexpr (std::is_base_of_v<jasperengine::ScriptBase, T>) return static_cast<jasperengine::ScriptBase*>(instance);\n";
    sourceContent += "    else return nullptr;\n";
    sourceContent += "}\n\n";
    sourceContent += "extern \"C\" PROJECT_API jasperengine::ScriptBase* jasper_scriptBase(const char* className, void* instance) {\n";
    sourceContent += "    const std::string name = className ? className : \"\";\n";
    for (const auto& s : scriptFiles) {
        sourceContent += "    if (name == \"" + s.className + "\") return toScriptBase(static_cast<" + s.className + "*>(instance));\n";
    }
    sourceContent += "    (void)instance;\n";
    sourceContent += "    return nullptr;\n";
    sourceContent += "}\n\n";

    sourceContent += "extern \"C\" void PROJECT_API initEntityScripts(jasperengine::Scene* scene, const jasperengine::Entity* entities, size_t count) {\n";
    if (!scriptFiles.empty()) {
        sourceContent += "    for (size_t i = 0; i < count; i++) {\n";
        sourceContent += "        if (scene->isEntityCreated(entities[i])) createCppScripts(scene, entities[i]);\n";
        sourceContent += "    }\n";
        sourceContent += "    for (size_t i = 0; i < count; i++) {\n";
        sourceContent += "        if (scene->isEntityCreated(entities[i])) bindCppScriptProperties(scene, entities[i]);\n";
        sourceContent += "    }\n";
    } else {
        sourceContent += "    (void)scene; (void)entities; (void)count;\n";
    }
    sourceContent += "}\n\n";

    // GameObjects moved to another scene (persistence: DontDestroyOnLoad, MoveGameObjectToScene):
    // the engine rebinds ScriptBase scripts; the GameObject wrapper kind and the wrappers the
    // properties own follow here (their classes are known only to this code)
    sourceContent += "extern \"C\" void PROJECT_API moveEntityScripts(jasperengine::Scene* from, jasperengine::Scene* to, const jasperengine::Entity* oldEntities, const jasperengine::Entity* newEntities, size_t count) {\n";
    if (!scriptFiles.empty()) {
        sourceContent += "    for (size_t i = 0; i < count; i++) {\n";
        sourceContent += "        jasperengine::ScriptComponent* scriptComp = to->isEntityCreated(newEntities[i]) ? to->findComponent<jasperengine::ScriptComponent>(newEntities[i]) : nullptr;\n";
        sourceContent += "        if (!scriptComp) continue;\n";
        sourceContent += "        for (auto& scriptEntry : scriptComp->scripts) {\n";
        sourceContent += "            if (scriptEntry.type == ScriptType::LUA || !scriptEntry.instance) continue;\n";
        for (const auto& s : scriptFiles) {
            sourceContent += "            if (scriptEntry.className == \"" + s.className + "\") {\n";
            sourceContent += "                jasperengine::rebindScriptObject(static_cast<" + s.className + "*>(scriptEntry.instance), to, newEntities[i]);\n";
            std::string references;
            for (const auto& prop : s.properties) {
                if (!prop.isPtr || prop.ptrTypeName.empty()) {
                    continue;
                }
                references += "                    if (prop.name == \"" + prop.name + "\") jasperengine::rebindScriptReference(static_cast<" + prop.ptrTypeName + "*>(prop.ownedInstance), from, to, oldEntities, newEntities, count);\n";
            }
            if (!references.empty()) {
                sourceContent += "                for (auto& prop : scriptEntry.properties) {\n";
                sourceContent += "                    if (!prop.ownedInstance) continue;\n";
                sourceContent += references;
                sourceContent += "                }\n";
            }
            sourceContent += "            }\n";
        }
        sourceContent += "        }\n";
        sourceContent += "    }\n";
    } else {
        sourceContent += "    (void)from; (void)to; (void)oldEntities; (void)newEntities; (void)count;\n";
    }
    sourceContent += "}\n\n";

    return sourceContent;
}

std::string editor::Generator::buildCleanupSceneScriptsSource(const std::vector<SceneScriptSource>& scriptFiles) {
    std::string sourceContent;

    if (!scriptFiles.empty()) {
        // the scene is not needed any more: a script's subscriptions are found by its address
        sourceContent += "static void deleteCppScripts(jasperengine::ScriptComponent& scriptComp) {\n";
        sourceContent += "    {\n";
        sourceContent += "        for (auto& scriptEntry : scriptComp.scripts) {\n";
        sourceContent += "            if (scriptEntry.type == ScriptType::LUA) continue;\n";
        sourceContent += "\n";
        sourceContent += "            if (scriptEntry.instance) {\n";
        for (const auto& s : scriptFiles) {
            sourceContent += "                if (scriptEntry.className == \"" + s.className + "\") {\n";
            // its REGISTER_* subscriptions and its timers; the script runtime forgets it
            sourceContent += "                    SubscriptionOwnerIndex::releaseOwner(ownerKeyOf(scriptEntry.instance));\n";
            sourceContent += "                    jasperengine::ScriptRuntime::forgetInstance(scriptEntry.instance);\n";
            sourceContent += "                    delete static_cast<" + s.className + "*>(scriptEntry.instance);\n";
            sourceContent += "                }\n";
        }
        sourceContent += "                scriptEntry.instance = nullptr;\n";
        sourceContent += "                scriptEntry.cppScript = nullptr;\n";
        sourceContent += "                scriptEntry.cppMessageMask = 0;\n";
        sourceContent += "            }\n";

        // delete the wrappers initScripts created for entity reference members
        for (const auto& s : scriptFiles) {
            std::string deleteContent;
            for (const auto& prop : s.properties) {
                if (!prop.isPtr || prop.ptrTypeName.empty()) {
                    continue;
                }
                deleteContent += "                    if (prop.name == \"" + prop.name + "\") {\n";
                deleteContent += "                        delete static_cast<" + prop.ptrTypeName + "*>(prop.ownedInstance);\n";
                deleteContent += "                    }\n";
            }
            if (deleteContent.empty()) {
                continue;
            }

            sourceContent += "\n";
            sourceContent += "            if (scriptEntry.className == \"" + s.className + "\") {\n";
            sourceContent += "                for (auto& prop : scriptEntry.properties) {\n";
            sourceContent += "                    if (!prop.ownedInstance) continue;\n";
            sourceContent += "\n";
            sourceContent += "                    SubscriptionOwnerIndex::releaseOwner(ownerKeyOf(prop.ownedInstance));\n";
            sourceContent += "\n";
            sourceContent += deleteContent;
            sourceContent += "\n";
            sourceContent += "                    prop.ownedInstance = nullptr;\n";
            sourceContent += "                }\n";
            sourceContent += "            }\n";
        }

        sourceContent += "        }\n";
        sourceContent += "    }\n";
        sourceContent += "}\n\n";
    }

    sourceContent += "extern \"C\" void PROJECT_API cleanupScripts(jasperengine::Scene* scene) {\n";
    sourceContent += "    LuaBinding::cleanupLuaScripts(scene);\n";
    if (!scriptFiles.empty()) {
        sourceContent += "    const auto& scriptsArray = scene->getComponentArray<ScriptComponent>();\n";
        sourceContent += "    for (size_t i = 0; i < scriptsArray->size(); i++) {\n";
        sourceContent += "        deleteCppScripts(scriptsArray->getComponentFromIndex(i));\n";
        sourceContent += "    }\n";
    }
    sourceContent += "}\n\n";

    // GameObjects destroyed while the game runs (ObjectManager): the engine stops their Lua
    // scripts, these are the C++ ones
    sourceContent += "extern \"C\" void PROJECT_API cleanupEntityScripts(jasperengine::Scene* scene, const jasperengine::Entity* entities, size_t count) {\n";
    if (!scriptFiles.empty()) {
        sourceContent += "    for (size_t i = 0; i < count; i++) {\n";
        sourceContent += "        if (!scene->isEntityCreated(entities[i])) continue;\n";
        sourceContent += "        if (jasperengine::ScriptComponent* scriptComp = scene->findComponent<jasperengine::ScriptComponent>(entities[i])) {\n";
        sourceContent += "            deleteCppScripts(*scriptComp);\n";
        sourceContent += "        }\n";
        sourceContent += "    }\n";
    } else {
        sourceContent += "    (void)scene; (void)entities; (void)count;\n";
    }
    sourceContent += "}\n";

    return sourceContent;
}

std::string editor::Generator::scriptFingerprintSource() {
    // JASPER_TEST_FINGERPRINT_OVERRIDE=field=value[,field=value]: the library claims another
    // toolchain (the fingerprint's tests, S23 AC10)
    std::string fingerprint = "jasperengine::currentToolchainFingerprint()";
    if (const char* overrides = std::getenv("JASPER_TEST_FINGERPRINT_OVERRIDE")) {
        std::string list = overrides;
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find(',', start);
            if (end == std::string::npos) end = list.size();
            const std::string item = list.substr(start, end - start);
            const size_t equals = item.find('=');
            if (equals != std::string::npos && equals > 0) {
                const std::string field = item.substr(0, equals);
                const std::string value = item.substr(equals + 1);
                const bool numeric = !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
                const bool identifier = std::all_of(field.begin(), field.end(), [](unsigned char c) { return std::isalnum(c); });
                if (numeric && identifier) {
                    fingerprint = "jasperengine::withFingerprintOverride(" + fingerprint + ", \"" + field + "\", " + value + "u)";
                }
            }
            start = end + 1;
        }
    }
    std::string source;
    source += "// The toolchain this library is built with (compiler, standard library and its ABI, CRT,\n";
    source += "// layout defines, the engine build): the editor reads the marker from the file before it\n";
    source += "// loads it and compares the export after, and refuses a library that differs.\n";
    source += "#if defined(JASPERENGINE_EDITOR_PLUGIN)\n";
    source += "JASPERENGINE_SCRIPT_TOOLCHAIN_FINGERPRINT(" + fingerprint + ")\n";
    source += "#endif\n\n";
    return source;
}

bool editor::Generator::rewriteScriptSources(const std::vector<SceneScriptSource>& scriptFiles) {
    if (!lastSourceArgs.valid) return false;
    const SourceFileArgs args = lastSourceArgs;
    writeSourceFiles(args.projectPath, args.projectInternalPath, args.libName, scriptFiles, args.scenes, args.prefabs,
                     args.vsyncEnabled, args.windowSettings, args.assetsPath, args.luaPath, args.scriptDirs, args.cxxStandard);
    return true;
}

ThreadSafeSignal<const editor::ScriptBuildEvent&>& editor::Generator::buildEvents() {
    static auto* signal = new ThreadSafeSignal<const ScriptBuildEvent&>();
    return *signal;
}

bool editor::Generator::parseBuildProgress(const std::string& line, float& fraction) {
    size_t at = line.find_first_not_of(" \t");
    if (at == std::string::npos || line[at] != '[') return false;
    const size_t close = line.find(']', at);
    if (close == std::string::npos || close - at > 24) return false;
    const std::string inside = line.substr(at + 1, close - at - 1);
    // Ninja: [12/50]
    const size_t slash = inside.find('/');
    if (slash != std::string::npos) {
        const std::string done = inside.substr(0, slash), total = inside.substr(slash + 1);
        auto digits = [](const std::string& text) {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
        };
        if (!digits(done) || !digits(total) || std::stoul(total) == 0) return false;
        fraction = std::min(1.0f, static_cast<float>(std::stoul(done)) / static_cast<float>(std::stoul(total)));
        return true;
    }
    // Make: [ 45%]
    std::string percent = inside;
    percent.erase(std::remove(percent.begin(), percent.end(), ' '), percent.end());
    if (percent.size() < 2 || percent.back() != '%') return false;
    percent.pop_back();
    if (!std::all_of(percent.begin(), percent.end(), [](unsigned char c) { return std::isdigit(c); }) || percent.empty()) return false;
    fraction = std::min(1.0f, static_cast<float>(std::stoul(percent)) / 100.0f);
    return true;
}

std::string editor::Generator::getEditorPluginAbiCheck() {
    std::string cmakeContent;
#ifdef _WIN32
    // Check CMake's detected ABI: compiler names alone miss Clang targeting
    // MSVC, and saved/custom kits bypass discovery entirely.
    cmakeContent += "if(JASPERENGINE_EDITOR_PLUGIN)\n";
#if defined(__MINGW32__)
    cmakeContent += "    if(NOT WIN32 OR NOT MINGW OR MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL \"MSVC\")\n";
    cmakeContent += "        message(FATAL_ERROR \"This JasperEngine Editor requires MinGW/GNU C++ plugins. Use the same MinGW toolchain that built the editor and engine.\")\n";
#else
    cmakeContent += "    if(NOT WIN32 OR NOT (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL \"MSVC\"))\n";
    cmakeContent += "        message(FATAL_ERROR \"This JasperEngine Editor requires MSVC-compatible C++ plugins. MSYS2/MinGW GCC cannot link its engine library. Select an MSVC-compatible compiler in Preferences > External Tools, or rebuild the editor and engine with your MinGW toolchain.\")\n";
#endif
    cmakeContent += "    endif()\n";
    cmakeContent += "    if(NOT CMAKE_SIZEOF_VOID_P EQUAL " + std::to_string(sizeof(void*)) + ")\n";
    cmakeContent += "        message(FATAL_ERROR \"C++ plugins must match this JasperEngine Editor's " + std::to_string(sizeof(void*) * 8) + "-bit architecture. Select a matching compiler in Preferences > External Tools.\")\n";
    cmakeContent += "    endif()\n";
    cmakeContent += "endif()\n\n";
#endif
    return cmakeContent;
}

std::string editor::Generator::getToolchainFloorCheck() {
    // The floors of engine/cmake/JasperEngineToolchain.cmake, which the root CMakeLists.txt
    // passes to the editor; a plugin has no engine tree to include that file from
    std::string cmakeContent;
    cmakeContent += "# Oldest compilers with the C++20 JasperEngine uses (as the engine build checks)\n";
    cmakeContent += "set(JASPERENGINE_CXX_FLOOR_GNU " JASPERENGINE_CXX_FLOOR_GNU ")\n";
    cmakeContent += "set(JASPERENGINE_CXX_FLOOR_Clang " JASPERENGINE_CXX_FLOOR_CLANG ")\n";
    cmakeContent += "set(JASPERENGINE_CXX_FLOOR_AppleClang " JASPERENGINE_CXX_FLOOR_APPLECLANG ")\n";
    cmakeContent += "set(JASPERENGINE_CXX_FLOOR_MSVC " JASPERENGINE_CXX_FLOOR_MSVC ")\n";
    cmakeContent += "set(JASPERENGINE_CXX_FLOOR \"${JASPERENGINE_CXX_FLOOR_${CMAKE_CXX_COMPILER_ID}}\")\n";
    cmakeContent += "if(JASPERENGINE_CXX_FLOOR AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS JASPERENGINE_CXX_FLOOR)\n";
    cmakeContent += "    message(FATAL_ERROR \"JasperEngine requires C++20: the ${CMAKE_CXX_COMPILER_ID} compiler ${CMAKE_CXX_COMPILER_VERSION} is older than \"\n";
    cmakeContent += "        \"the supported minimum ${CMAKE_CXX_COMPILER_ID} ${JASPERENGINE_CXX_FLOOR}. Update it, or choose another compiler in \"\n";
    cmakeContent += "        \"Preferences > External Tools.\")\n";
    cmakeContent += "endif()\n\n";
    return cmakeContent;
}

void editor::Generator::writeSourceFiles(const fs::path& projectPath, const fs::path& projectInternalPath, std::string libName, const std::vector<SceneScriptSource>& scriptFiles, const std::vector<editor::SceneBuildInfo>& scenes, const std::vector<editor::PrefabSceneInfo>& prefabs, bool vsyncEnabled, const WindowSettings& windowSettings, const fs::path& assetsPath, const fs::path& luaPath, const std::vector<fs::path>& scriptDirs, int cxxStandard) {
    lastSourceArgs = {true, projectPath, projectInternalPath, assetsPath, luaPath, libName, scriptFiles, scenes, prefabs,
                      vsyncEnabled, windowSettings, scriptDirs, cxxStandard};
    // the editor regenerates its own files here (an MCP call's deny-by-default guard refuses them
    // otherwise): the internal folder, the root CMakeLists.txt, .vscode (util/SafeWrite.h)
    safewrite::EditorInternal ownInternal(projectInternalPath);
    safewrite::EditorInternal ownCMake(projectPath / "CMakeLists.txt");
    safewrite::EditorInternal ownVscode(projectPath / ".vscode");
    fs::path relativeInternalPath = fs::relative(projectInternalPath, projectPath);
    fs::path engineApiRelativePath = relativeInternalPath / "engine-api";

    std::string internalPathStr = "${CMAKE_CURRENT_SOURCE_DIR}/" + relativeInternalPath.generic_string();
    std::string engineApiPathStr = "${CMAKE_CURRENT_SOURCE_DIR}/" + engineApiRelativePath.generic_string();

    // Build FACTORY_SOURCES list for CMake (generated by Factory in configure())
    std::string factorySources = "set(FACTORY_SOURCES\n";
    std::unordered_set<std::string> addedFactorySources;
    for (const auto& sceneData : scenes) {
        std::string filename = Factory::sceneToFileName(sceneData.id, sceneData.name) + ".cpp";
        if (addedFactorySources.insert(filename).second) {
            factorySources += "    " + internalPathStr + "/generated/" + filename + "\n";
        }
    }
    for (const auto& prefab : prefabs) {
        std::string filename = Factory::prefabToFileName(prefab.prefabPath) + ".cpp";
        if (addedFactorySources.insert(filename).second) {
            factorySources += "    " + internalPathStr + "/generated/" + filename + "\n";
        }
    }
    factorySources += ")\n";

    // Build SCRIPT_SOURCES list for CMake. Insertion order is kept so the file
    // only changes when the project does, and roots are searched as listed.
    std::vector<std::string> includeDirs;
    std::unordered_set<std::string> addedIncludeDirs;
    const auto addIncludeDir = [&](const std::string& dir) {
        if (addedIncludeDirs.insert(dir).second) {
            includeDirs.push_back(dir);
        }
    };

    std::string scriptSources = "set(SCRIPT_SOURCES\n";
    std::unordered_set<std::string> addedScriptSources;
    const auto addScriptSource = [&](const fs::path& path) {
        const fs::path absolute = path.is_absolute() ? path : projectPath / path;
        if (!addedScriptSources.insert(absolute.lexically_normal().generic_string()).second) {
            return;
        }
        if (path.is_relative()) {
            scriptSources += "    ${CMAKE_CURRENT_SOURCE_DIR}/" + path.generic_string() + "\n";
        } else {
            scriptSources += "    " + path.generic_string() + "\n";
        }
    };

    for (const auto& s : scriptFiles) {
        addScriptSource(s.path);

        if (!s.headerPath.empty()) {
            if (s.headerPath.is_relative()) {
                addIncludeDir("${CMAKE_CURRENT_SOURCE_DIR}");
            } else {
                addIncludeDir(s.headerPath.parent_path().generic_string());
            }
        }
    }

    // Each script root is an include directory, and every source under it is
    // compiled: a file builds by living there, not by being referenced.
    for (const fs::path& scriptDir : scriptDirs) {
        const fs::path rootPath = (scriptDir.is_absolute() ? scriptDir : projectPath / scriptDir).lexically_normal();

        std::error_code ec;
        if (!fs::is_directory(rootPath, ec)) {
            Out::warning("Script directory not found: %s", scriptDir.generic_string().c_str());
            continue;
        }

        if (scriptDir.is_absolute()) {
            addIncludeDir(scriptDir.generic_string());
        } else {
            addIncludeDir("${CMAKE_CURRENT_SOURCE_DIR}/" + scriptDir.generic_string());
        }

        // Iteration order is unspecified, so sort to keep the file stable.
        std::vector<fs::path> rootSources;
        try {
            for (auto it = fs::recursive_directory_iterator(rootPath, fs::directory_options::skip_permission_denied);
                 it != fs::recursive_directory_iterator(); ++it) {
                // Hidden and build directories hold the generated sources the
                // target already compiles through PROJECT_SOURCE and FACTORY_SOURCES.
                const std::string name = it->path().filename().string();
                if (it->is_directory() && (name.empty() || name[0] == '.' || name == "build")) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file() || !Util::isSourceFile(it->path().string())) {
                    continue;
                }
                // the C++ editor scripts build into an editor-only library of their own
                if (EditorScripts::isEditorOnlyScript(it->path(), projectPath)) {
                    continue;
                }

                const fs::path relativePath = fs::relative(it->path(), projectPath, ec);
                const bool insideProject = !ec && !relativePath.empty() && *relativePath.begin() != "..";
                rootSources.push_back(insideProject ? relativePath : it->path());
            }
        } catch (const fs::filesystem_error& e) {
            Out::warning("Failed to scan script directory \"%s\": %s", scriptDir.generic_string().c_str(), e.what());
        }

        std::sort(rootSources.begin(), rootSources.end(), [](const fs::path& a, const fs::path& b) {
            return a.generic_string() < b.generic_string();
        });
        for (const fs::path& source : rootSources) {
            addScriptSource(source);
        }
    }
    scriptSources += ")\n";

    std::string includeDirsBlock;
    for (const std::string& includeDir : includeDirs) {
        includeDirsBlock += "    " + includeDir + "\n";
    }

    std::string cmakeContent;
    cmakeContent += "# This file is auto-generated by JasperEngine Editor. Do not edit manually.\n\n";
    cmakeContent += "cmake_minimum_required(VERSION 3.20)\n";
    cmakeContent += "project(" + libName + ")\n\n";
    cmakeContent += "# Editor-resolved parallel build job limit, supplied via -D on every editor\n";
    cmakeContent += "# configure. Declared here so it is documented and so CMake does not warn\n";
    cmakeContent += "# about an unused variable on generators that do not consume it (only the\n";
    cmakeContent += "# MSVC branch below reads it). Empty or 0 means no per-file compile limit.\n";
    cmakeContent += "set(JASPERENGINE_PARALLEL_BUILD_JOBS \"\" CACHE STRING \"Editor-resolved parallel build job limit\")\n\n";
    cmakeContent += "set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})\n";
    cmakeContent += "set(INTERNAL_DIR ${PROJECT_ROOT}/.jasperengine)\n\n";
    cmakeContent += "# JasperEngine runtime API headers for this project come from ${INTERNAL_DIR}/engine-api.\n";
    cmakeContent += "# Full engine + editor source (including YAML serialization for *.scene/*.prefab/project.yaml): https://github.com/JasperRaccoon/JasperEngine\n\n";

    cmakeContent += "set(JASPERENGINE_API_DIR " + engineApiPathStr + ")\n\n";

    cmakeContent += "# C++ standard from Project Settings\n";
    cmakeContent += "set(CMAKE_CXX_STANDARD " + std::to_string(cxxStandard) + ")\n";
    cmakeContent += "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
    cmakeContent += "set(CMAKE_OBJCXX_STANDARD ${CMAKE_CXX_STANDARD})\n";
    cmakeContent += "set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)\n\n";
    cmakeContent += "# compile_commands.json in the build tree, for IDEs and language servers (the editor\n";
    cmakeContent += "# also writes one for clangd in ${INTERNAL_DIR}/clangd, see .clangd)\n";
    cmakeContent += "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n\n";

    cmakeContent += "# Build mode: when ON, build as JasperEngine Editor plugin (shared library)\n";
    cmakeContent += "option(JASPERENGINE_EDITOR_PLUGIN \"Build as JasperEngine Editor plugin\" OFF)\n";

    // Both modes link the editor's engine library, which always has both physics backends.
    // Jolt's configuration changes its class layouts, so it has to match the engine's.
    cmakeContent += "add_compile_definitions(JASPERENGINE_PHYSICS_2D JASPERENGINE_PHYSICS_3D " + joltCompileDefinitions() + ")\n\n";

    cmakeContent += getToolchainFloorCheck();
    cmakeContent += getEditorPluginAbiCheck();
    cmakeContent += "if(JASPERENGINE_EDITOR_PLUGIN)\n";
    cmakeContent += "    add_compile_definitions(JASPERENGINE_EDITOR_PLUGIN)\n";
    cmakeContent += "    # The editor links its own engine build, which is compiled as a shared\n";
    cmakeContent += "    # library with editor-specific data layouts. The plugin must match both:\n";
    cmakeContent += "    #  - JASPERENGINE_SHARED makes JASPERENGINE_API resolve to __declspec(dllimport) so\n";
    cmakeContent += "    #    static members such as Engine::onUpdate link against the import lib on MSVC.\n";
    cmakeContent += "    #  - JASPERENGINE_EDITOR keeps HybridArray (and other editor-only layouts)\n";
    cmakeContent += "    #    ABI-compatible across the DLL boundary.\n";
    cmakeContent += "    add_compile_definitions(JASPERENGINE_SHARED JASPERENGINE_EDITOR)\n";
    cmakeContent += "endif()\n\n";

    // Match the engine DLL shipped beside this editor, which JASPERENGINE_LIB_DIR defaults
    // to; _DEBUG is defined iff this editor was built against the debug CRT.
    std::string runtimeLibrary = "MultiThreadedDLL";
#if defined(_DEBUG)
    runtimeLibrary = "MultiThreadedDebugDLL";
#endif

    cmakeContent += "# Every translation unit linking the engine DLL needs the CRT that DLL was built\n";
    cmakeContent += "# with: debug and release STL disagree on std::string/std::vector layout and use\n";
    cmakeContent += "# separate heaps, and an import library carries no /FAILIFMISMATCH records, so\n";
    cmakeContent += "# the linker cannot catch the mismatch. The build type cannot pick it either,\n";
    cmakeContent += "# since IDEs default to Debug, so it follows the editor that generated this file.\n";
    cmakeContent += "if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)\n";
    cmakeContent += "    set(CMAKE_MSVC_RUNTIME_LIBRARY \"" + runtimeLibrary + "\")\n";
    cmakeContent += "endif()\n\n";

    cmakeContent += getPlatformCMakeConfig(vsyncEnabled, windowSettings, projectPath, assetsPath, luaPath) + "\n";

    cmakeContent += scriptSources + "\n";
    cmakeContent += factorySources + "\n";
    cmakeContent += "set(PROJECT_SOURCE " + internalPathStr + "/scene_scripts.cpp)\n\n";
    cmakeContent += "# Project target\n";
    cmakeContent += "if(NOT CMAKE_SYSTEM_NAME STREQUAL \"Android\" AND NOT JASPERENGINE_EDITOR_PLUGIN)\n";
    cmakeContent += "    add_executable(" + libName + "\n";
    cmakeContent += "        ${PROJECT_SOURCE}\n";
    cmakeContent += "        ${SCRIPT_SOURCES}\n";
    cmakeContent += "        ${PLATFORM_SOURCE}\n";
    cmakeContent += "    )\n";
    cmakeContent += "else()\n";
    cmakeContent += "    add_library(" + libName + " SHARED\n";
    cmakeContent += "        ${PROJECT_SOURCE}\n";
    cmakeContent += "        ${SCRIPT_SOURCES}\n";
    cmakeContent += "        ${PLATFORM_SOURCE}\n";
    cmakeContent += "    )\n";
    cmakeContent += "endif()\n\n";

    cmakeContent += "# When building outside the editor, also compile Factory-generated sources\n";
    cmakeContent += "if(NOT JASPERENGINE_EDITOR_PLUGIN)\n";
    cmakeContent += "    target_sources(" + libName + " PRIVATE ${FACTORY_SOURCES})\n";
    cmakeContent += "endif()\n\n";
    cmakeContent += "# To suppress warnings if not Debug\n";
    cmakeContent += "if(NOT CMAKE_BUILD_TYPE STREQUAL \"Debug\")\n";
    cmakeContent += "    set(JASPERENGINE_LIB_SYSTEM SYSTEM)\n";
    cmakeContent += "endif()\n\n";
    cmakeContent += "target_include_directories(" + libName + " ${JASPERENGINE_LIB_SYSTEM} PRIVATE\n";
    cmakeContent += includeDirsBlock + "\n";
    for (const std::string& dir : engineApiIncludeDirs()) {
        cmakeContent += "    " + engineApiPathStr + (dir.empty() ? "" : "/" + dir) + "\n";
    }
    cmakeContent += ")\n\n";

    cmakeContent += "# libjasperengine is searched in JASPERENGINE_LIB_DIR; by default it points to the JasperEngine Editor\n";
    cmakeContent += "# executable directory, which differs per machine and so comes from LocalPaths.cmake.\n";
    cmakeContent += "include(${INTERNAL_DIR}/LocalPaths.cmake OPTIONAL)\n";
    cmakeContent += "if(NOT DEFINED JASPERENGINE_LIB_DIR OR JASPERENGINE_LIB_DIR STREQUAL \"\")\n";
    cmakeContent += "    set(JASPERENGINE_LIB_DIR \"${JASPERENGINE_DEFAULT_LIB_DIR}\")\n";
    cmakeContent += "    # Default target is the editor's own engine build (shared, editor layouts).\n";
    cmakeContent += "    # Match its macros so symbols link (JASPERENGINE_SHARED -> dllimport on MSVC) and\n";
    cmakeContent += "    # data layouts stay ABI-compatible (JASPERENGINE_EDITOR), avoiding ODR/ABI mismatch.\n";
    cmakeContent += "    add_compile_definitions(JASPERENGINE_SHARED JASPERENGINE_EDITOR)\n";
    cmakeContent += "endif()\n\n";

    cmakeContent += "# Find jasperengine library in specified location. find_library() caches its\n";
    cmakeContent += "# result, so a stale JASPERENGINE_LIB from a previous configure (e.g. before the\n";
    cmakeContent += "# editor/engine install moved) would otherwise stick around unchanged;\n";
    cmakeContent += "# force a fresh search on every configure instead.\n";
    cmakeContent += "unset(JASPERENGINE_LIB CACHE)\n";
    cmakeContent += "find_library(JASPERENGINE_LIB NAMES jasperengine PATHS \"${JASPERENGINE_LIB_DIR}\" NO_DEFAULT_PATH)\n";
    cmakeContent += "if(NOT JASPERENGINE_LIB)\n";
    cmakeContent += "    message(FATAL_ERROR \"JasperEngine library not found in ${JASPERENGINE_LIB_DIR}\")\n";
    cmakeContent += "endif()\n\n";
    cmakeContent += "target_link_libraries(" + libName + " PRIVATE ${JASPERENGINE_LIB} ${PLATFORM_LIBS})\n";
    // macOS: the Objective-C native plugins run in the Play library and desktop builds too
    cmakeContent += NativePlugins::macosCMake(NativePlugins::scan(projectPath), libName, "${CMAKE_CURRENT_SOURCE_DIR}/plugins/native",
                                              macOSFrameworks, engineApiPathStr + "/platform/apple");
    cmakeContent += "\n";
    cmakeContent += "# Set compile options based on compiler and platform\n";
    cmakeContent += "# A C++ reload during Play links a new library while the old one (and, under a debugger, its\n";
    cmakeContent += "# PDB) is still loaded: each reload generation gets its own PDB name.\n";
    cmakeContent += "# (Included on MSVC only: a change of the file re-runs the configure step.)\n";
    cmakeContent += "if(MSVC AND JASPERENGINE_EDITOR_PLUGIN)\n";
    cmakeContent += "    include(${INTERNAL_DIR}/PluginGeneration.cmake OPTIONAL)\n";
    cmakeContent += "    if(JASPERENGINE_PLUGIN_GENERATION GREATER 0)\n";
    cmakeContent += "        set_target_properties(" + libName + " PROPERTIES PDB_NAME \"" + libName + "_${JASPERENGINE_PLUGIN_GENERATION}\")\n";
    cmakeContent += "    endif()\n";
    cmakeContent += "endif()\n\n";
    cmakeContent += "if(MSVC)\n";
    cmakeContent += "    # C4251/C4275: exported engine classes (JASPERENGINE_API -> dllimport) expose STL\n";
    cmakeContent += "    # members (std::vector, std::string, FunctionSubscribe, ...). This is safe\n";
    cmakeContent += "    # here because the engine DLL and this plugin use the same dynamic CRT/STL,\n";
    cmakeContent += "    # so suppress the dll-interface warnings that would otherwise flood the build.\n";
    cmakeContent += "    # MSBuild's /m only parallelizes projects. /MP applies the editor-resolved\n";
    cmakeContent += "    # limit to translation units within this single-target plugin project.\n";
    cmakeContent += "    # /utf-8: generated sources embed UTF-8 literals and carry no BOM, so\n";
    cmakeContent += "    # MSVC would otherwise decode them in the active code page.\n";
    cmakeContent += "    # /Zc:__cplusplus: the real language level, as in the engine build, so header-only\n";
    cmakeContent += "    # libraries (nlohmann json, LuaBridge3) take the same code paths on both sides.\n";
    cmakeContent += "    if(DEFINED JASPERENGINE_PARALLEL_BUILD_JOBS AND JASPERENGINE_PARALLEL_BUILD_JOBS GREATER 0)\n";
    cmakeContent += "        target_compile_options(" + libName + " PRIVATE /W4 /EHsc /utf-8 /Zc:__cplusplus /MP${JASPERENGINE_PARALLEL_BUILD_JOBS} /wd4251 /wd4275)\n";
    cmakeContent += "    else()\n";
    cmakeContent += "        target_compile_options(" + libName + " PRIVATE /W4 /EHsc /utf-8 /Zc:__cplusplus /MP /wd4251 /wd4275)\n";
    cmakeContent += "    endif()\n";
    cmakeContent += "else()\n";
    cmakeContent += "    target_compile_options(" + libName + " PRIVATE -Wall -Wextra -fPIC)\n";
    cmakeContent += "    # Error on unresolved symbols at link time. Apple ld does this by default;\n";
    cmakeContent += "    # GNU ld needs '-z defs --no-undefined' and MinGW only understands '--no-undefined'.\n";
    cmakeContent += "    if(WIN32)\n";
    cmakeContent += "        target_link_options(" + libName + " PRIVATE -Wl,--no-undefined)\n";
    cmakeContent += "    elseif(NOT APPLE)\n";
    cmakeContent += "        target_link_options(" + libName + " PRIVATE -Wl,-z,defs,--no-undefined)\n";
    cmakeContent += "    endif()\n";
    cmakeContent += "endif()\n\n";
    cmakeContent += "# Set properties for the shared library\n";
    cmakeContent += "set_target_properties(" + libName + " PROPERTIES\n";
    cmakeContent += "    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    OUTPUT_NAME \"" + libName + "\"\n";
    cmakeContent += "    PREFIX \"\"\n";
    cmakeContent += ")\n\n";

    cmakeContent += "# The engine DLL lives next to the editor, and Windows searches only the\n";
    cmakeContent += "# executable directory and PATH, so a standalone build needs its own copy or\n";
    cmakeContent += "# it dies with STATUS_DLL_NOT_FOUND before main(). The other desktops get\n";
    cmakeContent += "# JASPERENGINE_LIB_DIR in the build rpath from CMake.\n";
    cmakeContent += "if(WIN32 AND NOT JASPERENGINE_EDITOR_PLUGIN)\n";
    cmakeContent += "    # Same reason as JASPERENGINE_LIB above: never reuse a cached path\n";
    cmakeContent += "    unset(JASPERENGINE_RUNTIME_LIB CACHE)\n";
    cmakeContent += "    find_file(JASPERENGINE_RUNTIME_LIB NAMES jasperengine.dll PATHS \"${JASPERENGINE_LIB_DIR}\" NO_DEFAULT_PATH)\n";
    cmakeContent += "    if(JASPERENGINE_RUNTIME_LIB)\n";
    cmakeContent += "        add_custom_command(TARGET " + libName + " POST_BUILD\n";
    cmakeContent += "            COMMAND ${CMAKE_COMMAND} -E copy_if_different\n";
    cmakeContent += "                    \"${JASPERENGINE_RUNTIME_LIB}\" \"$<TARGET_FILE_DIR:" + libName + ">\"\n";
    cmakeContent += "            COMMENT \"Copying the JasperEngine runtime next to the executable\")\n";
    cmakeContent += "    else()\n";
    cmakeContent += "        message(WARNING \"jasperengine.dll not found in ${JASPERENGINE_LIB_DIR}: the executable will not start outside the editor.\")\n";
    cmakeContent += "    endif()\n";
    cmakeContent += "endif()\n";

    cmakeContent += "\n# Optional user build file, never written by the editor: extra include roots,\n";
    cmakeContent += "# libraries and defines go there so they survive regeneration. Attach to\n";
    cmakeContent += "# JASPERENGINE_TARGET, not the literal target name, and use JASPERENGINE_SCRIPTS_DIR as the\n";
    cmakeContent += "# root of script paths; the exported build sets both, so one file serves either.\n";
    cmakeContent += "set(JASPERENGINE_TARGET " + libName + ")\n";
    cmakeContent += "set(JASPERENGINE_SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR})\n";
    cmakeContent += "include(${CMAKE_CURRENT_SOURCE_DIR}/ProjectBuild.cmake OPTIONAL)\n";

    // Build C++ source content
    std::string sourceContent;
    sourceContent += "// This file is auto-generated by JasperEngine Editor. Do not edit manually.\n\n";
    sourceContent += "// This file binds scene script metadata to compiled C++ and Lua scripts for the current build configuration.\n\n";
    // the engine build the library is made for (the toolchain fingerprint's engine ABI hash)
    sourceContent += "#define JASPERENGINE_ABI_VERSION \"" + std::string(editorAbiVersion()) + "\"\n";
    sourceContent += "#include <vector>\n";
    sourceContent += "#include <string>\n";
    sourceContent += "#include <type_traits>\n";
    sourceContent += "#include <stdio.h>\n";
    sourceContent += "#include \"JasperEngine.h\"\n";
    sourceContent += "#include \"script/ToolchainFingerprint.h\"\n\n";
    sourceContent += scriptFingerprintSource();

    for (const auto& s : scriptFiles) {
        fs::path headerPath = s.headerPath;
        if (headerPath.is_relative()) {
            headerPath = projectPath / headerPath;
        }
        if (!headerPath.empty() && fs::exists(headerPath) && fs::is_regular_file(headerPath)) {
            fs::path relativePath = fs::relative(headerPath, projectPath);
            std::string inc = relativePath.generic_string();
            sourceContent += "#include \"" + inc + "\"\n";
        }
    }

    sourceContent += buildInitSceneScriptsSource(scriptFiles);
    sourceContent += buildCleanupSceneScriptsSource(scriptFiles);

    const fs::path cmakeFile = projectPath / "CMakeLists.txt";
    const fs::path sourceFile = projectInternalPath / "scene_scripts.cpp";

    // Everything that would otherwise name this machine in CMakeLists.txt, so that
    // file is byte-identical for the whole team
    std::string localPathsContent;
    localPathsContent += "# This file is auto-generated by JasperEngine Editor. Do not edit manually.\n";
    localPathsContent += "# Paths that belong to this machine only, so that CMakeLists.txt does not have\n";
    localPathsContent += "# to. Version control ignores .jasperengine/; the editor rewrites this on every build.\n\n";
    localPathsContent += "if(NOT JASPERENGINE_SHADER_PATH)\n";
    localPathsContent += "    set(JASPERENGINE_SHADER_PATH \"" + App::getUserShaderCacheDir().generic_string() + "\")\n";
    localPathsContent += "endif()\n\n";
    localPathsContent += "# Where libjasperengine sits on this machine: the directory of the editor that wrote\n";
    localPathsContent += "# this file. -DJASPERENGINE_LIB_DIR=<dir> overrides it.\n";
    localPathsContent += "set(JASPERENGINE_DEFAULT_LIB_DIR \"" + FileUtils::getExecutableDir().generic_string() + "\")\n";
    if (!FileUtils::isEngineDirEphemeral()) {
        localPathsContent += "\n# Engine API source this editor build was compiled from, recorded for tooling.\n";
        localPathsContent += "set(JASPERENGINE_LOCAL_ENGINE_DIR \"" + FileUtils::getEngineDir().generic_string() + "\")\n";
    }

    FileUtils::writeIfChanged(cmakeFile, cmakeContent);
    FileUtils::writeIfChanged(projectInternalPath / "LocalPaths.cmake", localPathsContent);
    // a Play build is generation 0; a C++ reload during Play writes its own number (MSVC PDB names)
    FileUtils::writeIfChanged(projectInternalPath / "PluginGeneration.cmake", pluginGenerationCMake(0));
    FileUtils::writeIfChanged(sourceFile, sourceContent);

    // Generate .vscode/settings.json for VS Code if it doesn't exist
    fs::path vscodeDir = projectPath / ".vscode";
    if (!fs::exists(vscodeDir)) {
        { std::error_code dirError; FileUtils::createDirectories(vscodeDir, dirError); }
    }
    fs::path settingsFile = vscodeDir / "settings.json";
    if (!fs::exists(settingsFile)) {
        std::string settingsContent;
        settingsContent += "{\n";
        settingsContent += "    \"cmake.buildDirectory\": \"${workspaceFolder}/" + relativeInternalPath.generic_string() + "/externalbuild\"\n";
        settingsContent += "}\n";
        FileUtils::writeIfChanged(settingsFile, settingsContent);
    }

    writeAgentsFile(projectPath, projectInternalPath);
    writeEditorTooling(projectPath, projectInternalPath, cxxStandard, scriptDirs);
}

namespace {

bool isLuaIdentifier(const std::string& name) {
    static const std::unordered_set<std::string> keywords = {
        "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
        "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    if (name.empty() || keywords.count(name)) return false;
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

// The ---@field names a script declares itself under ---@class <className>: the generated class
// must not declare them again (lua-language-server reports duplicate-doc-field)
std::set<std::string> fieldsDeclaredInScript(const std::string& code, const std::string& className) {
    static const std::regex classLine(R"(^\s*---\s*@class\s+(?:\([^)]*\)\s*)?([A-Za-z_][\w.]*))");
    static const std::regex fieldLine(R"(^\s*---\s*@field\s+(?:(?:private|protected|public|package)\s+)?([A-Za-z_]\w*))");
    std::set<std::string> fields;
    bool inClass = false;
    size_t start = 0;
    while (start <= code.size()) {
        size_t end = code.find('\n', start);
        if (end == std::string::npos) end = code.size();
        const std::string line = code.substr(start, end - start);
        start = end + 1;
        std::smatch m;
        if (std::regex_search(line, m, classLine)) {
            inClass = m[1].str() == className;
        } else if (line.find_first_not_of(" \t") != std::string::npos &&
                   line.compare(line.find_first_not_of(" \t"), 3, "---") == 0) {
            if (inClass && std::regex_search(line, m, fieldLine)) fields.insert(m[1].str());
        } else {
            inClass = false;
        }
        if (end == code.size()) break;
    }
    return fields;
}

// The Lua type of a script property's value on self (LuaBinding::createLuaScriptInstance and
// resolveLuaScriptReferences). Unassigned references are nil; the type leaves that out so code
// reading a property does not need a nil check per use.
std::string luaPropertyType(const ScriptProperty& prop, const std::map<std::string, std::string>& wrappers,
                            const std::set<std::string>& scriptClasses) {
    switch (prop.type) {
        case ScriptPropertyType::Bool: return "boolean";
        case ScriptPropertyType::Int: return "integer";
        case ScriptPropertyType::Float: return "number";
        case ScriptPropertyType::String: return "string";
        case ScriptPropertyType::Vector2: return "Vector2";
        case ScriptPropertyType::Vector3:
        case ScriptPropertyType::Color3: return "Vector3";
        case ScriptPropertyType::Vector4:
        case ScriptPropertyType::Color4: return "Vector4";
        case ScriptPropertyType::Prefab: return "PrefabAsset";
        case ScriptPropertyType::Asset: return "AssetRef";
        case ScriptPropertyType::EntityReference: break;
    }
    // "entity": the GameObject's first Lua script instance, else an EntityHandle
    if (prop.ptrTypeName.empty()) return "ScriptInstance|EntityHandle";
    // a script class name: that script's instance (LuaBinding::pushScriptObject)
    if (scriptClasses.count(prop.ptrTypeName)) return prop.ptrTypeName;
    // a wrapper class, case-insensitive (LuaBinding::normalizePtrTypeName)
    std::string key = prop.ptrTypeName;
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c) || c == '*' || c == '&'; }), key.end());
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto it = wrappers.find(key);
    return it != wrappers.end() ? it->second : "EntityHandle";
}

std::string luaScriptStub(const std::string& className, const std::string& relPath, const std::vector<ScriptProperty>& properties,
                          const std::set<std::string>& declared, const std::map<std::string, std::string>& wrappers,
                          const std::set<std::string>& scriptClasses) {
    std::string out;
    out += "---@meta _\n";
    out += "-- Generated by JasperEngine Editor from the properties of " + relPath + ". Do not edit: rewritten\n";
    out += "-- when the script changes. Declare the module in the script to type self:\n";
    out += "--   ---@class " + className + " : ScriptInstance\n";
    out += "--   local " + className + " = { properties = { ... } }\n\n";
    out += "---@class " + className + " : ScriptInstance\n";
    for (const ScriptProperty& prop : properties) {
        if (!isLuaIdentifier(prop.name) || declared.count(prop.name)) continue;
        out += "---@field " + prop.name + " " + luaPropertyType(prop, wrappers, scriptClasses);
        if (!prop.displayName.empty() && prop.displayName != prop.name) out += " # " + prop.displayName;
        out += "\n";
    }
    return out;
}

} // namespace

void editor::Generator::writeLuaScriptStubs(const fs::path& projectPath, const fs::path& projectInternalPath,
                                            const std::vector<fs::path>& changedFiles) {
    // the Lua stubs live in the editor's internal folder (util/SafeWrite.h, EditorInternal)
    safewrite::EditorInternal ownInternal(projectInternalPath);
    try {
        const fs::path stubsDir = projectInternalPath / "engine-api" / "lua" / "scripts";
        // a stubs folder resolving outside the project (a link): nothing written or removed there
        if (!safewrite::isInsideResolved(stubsDir, projectPath)) {
            Out::warning("Not writing the Lua script classes: %s resolves outside the project (a symbolic link on its way); remove the link.",
                         FileUtils::pathToGenericUtf8(stubsDir).c_str());
            return;
        }
        const fs::path luaRoot = ProjectChecks::luaRootFromProjectFile(projectPath);

        // engine names: a script named like an engine class or enum keeps its own declaration
        // (a generated class would merge into the engine's)
        std::set<std::string> engineNames;
        for (const auto& file : luastubs::stubFiles) {
            std::string name = file.name;
            if (name.size() > 4) engineNames.insert(name.substr(0, name.size() - 4));
        }
        std::map<std::string, std::string> wrappers;
        for (const char* name : luastubs::entityHandleClasses) {
            std::string key = name;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            wrappers[key] = name;
        }

        // every script class: the first file of each name (sorted paths), inside the Lua directory
        std::map<std::string, fs::path> classes;
        for (const fs::path& file : ProjectChecks::findProjectFiles(projectPath, {".lua"})) {
            const std::string name = file.stem().string();
            if (!Util::isInsidePath(file, luaRoot) || !isLuaIdentifier(name) || engineNames.count(name)) continue;
            // an editor script is no script class (and reading its properties would run it)
            if (EditorScripts::isEditorOnlyScript(file, projectPath)) continue;
            classes.emplace(name, file);
        }
        std::set<std::string> scriptClasses;
        for (const auto& entry : classes) scriptClasses.insert(entry.first);

        std::vector<std::pair<std::string, fs::path>> todo;
        std::set<std::string> removed;
        if (changedFiles.empty()) {
            for (const auto& entry : classes) todo.push_back(entry);
        } else {
            for (const fs::path& changed : changedFiles) {
                if (!Util::isLuaFile(changed.string())) continue;
                const std::string name = changed.stem().string();
                auto it = classes.find(name);
                if (it == classes.end()) removed.insert(name);
                else if (it->second.lexically_normal() == changed.lexically_normal()) todo.push_back(*it);
            }
        }

        std::error_code ec;
        if (!todo.empty()) {
            FileUtils::createDirectories(stubsDir, ec);
            const std::vector<std::string> engineGlobals = ProjectChecks::engineLuaGlobals();
            for (const auto& [name, file] : todo) {
                std::string code;
                if (!ProjectChecks::readFile(file, code)) continue;
                const std::string rel = ProjectChecks::projectRelative(projectPath, file);
                lua_State* L = ProjectChecks::newIsolatedLuaState(luaRoot, engineGlobals);
                const ProjectChecks::LuaScriptCheck check = ProjectChecks::checkLuaScript(code, rel, L);
                ProjectChecks::closeIsolatedLuaState(L);
                // a script that does not load (being edited?) keeps the class of its last good version
                if (!check.findings.empty()) continue;
                FileUtils::writeIfChanged(stubsDir / (name + ".lua"),
                    luaScriptStub(name, rel, check.properties, fieldsDeclaredInScript(code, name), wrappers, scriptClasses));
            }
        }

        // classes of scripts that are gone (a full pass: anything not a script class now)
        for (auto it = fs::directory_iterator(stubsDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const fs::path path = it->path();
            if (path.extension() != ".lua") continue;
            const std::string name = path.stem().string();
            const bool stale = changedFiles.empty() ? classes.count(name) == 0 : removed.count(name) > 0;
            std::error_code removeEc;
            if (stale) FileUtils::removePath(path, removeEc);
        }
    } catch (const std::exception& e) {
        Out::warning("Could not write the Lua script classes for lua-language-server: %s", e.what());
    }
}

void editor::Generator::writeAgentsFile(const fs::path& projectPath, const fs::path& projectInternalPath) {
    const fs::path relativeInternalPath = fs::relative(projectInternalPath, projectPath);
    const fs::path engineApiRelativePath = relativeInternalPath / "engine-api";

    // AGENTS.md and CLAUDE.md are written once, when missing. After that they belong to the
    // project (agents and people edit them), so the editor never changes an existing file.
    const fs::path agentsFile = projectPath / "AGENTS.md";
    std::error_code ec;
    // any entry there, a link (even a dangling one) included: never written (through)
    const fs::file_status agentsStatus = fs::symlink_status(agentsFile, ec);
    if (fs::exists(agentsStatus) || (ec && agentsStatus.type() != fs::file_type::not_found)) {
        writeClaudeFile(projectPath);
        return;
    }
    // What an agent in this project needs, with or without the MCP server; the tools' own guide
    // (initialize.instructions, jasper://docs) holds the rest, so nothing here repeats it
    const std::string api = engineApiRelativePath.generic_string();
    const std::string internal = relativeInternalPath.generic_string();
    std::string agentsContent;
    agentsContent += "# JasperEngine Project Context\n\n";
    agentsContent += "This project was generated by JasperEngine Editor version `" JASPERENGINE_EDITOR_VERSION "`.\n\n";
    agentsContent += "## Working with the editor (MCP)\n\n";
    agentsContent += "Build scenes, prefabs, UI and project settings, and playtest, through the editor's MCP tools, never by editing "
                     "`*.scene`, `*.prefab` or `project.yaml` by hand (the open editor overwrites such edits). Start the server with "
                     "`jasperengine-editor mcp --project <this directory>` (stdio), or attach to an open editor over HTTP (Window > MCP, "
                     "`http://127.0.0.1:47615/mcp`, with the bearer token that window copies). Its instructions and the "
                     "`jasper://docs/...` resources explain the tools. From a shell, `jasperengine-editor call <tool> [--flags]` runs any of them "
                     "(`call <tool> --help` lists its parameters).\n\n";
    agentsContent += "- Scripts, shaders and other text files: edit them on disk yourself; the editor reloads them before the next tool "
                     "call and reports problems under `external_changes`.\n";
    agentsContent += "- Lua API stubs: `" + api + "/lua/` (lua-language-server: `.luarc.json`); start a script with "
                     "`---@class <FileName> : ScriptInstance`. C++ API: the headers in `" + api + "/` (clangd: `.clangd`).\n";
    agentsContent += "- Sound files: never script or download audio; the editor makes and finds them. `tool_groups` {\"enable\": [\"audio\"]}, then "
                     "`generate_sound` (a placeholder), `search_sounds` (built-in clips), `analyze_audio` (measure a clip) or `edit_sound_event` "
                     "(clips that vary: footsteps, hits).\n";
    agentsContent += "- Checks without the editor: `jasperengine-editor check --project . [--files <paths...>] [--cpp] [--ui]` (exit 1 on "
                     "errors); as an agent hook after every edit: `check --project <this directory> --hook`.\n";
    agentsContent += "- Models are cooked (smaller, same image; settings in each model's `.meta`): `inspect_models` finds the costly ones, "
                     "`check_render_budgets` holds a scene to `quality.render_budgets` (`tool_groups` `meshes`).\n\n";
    agentsContent += "## HTML/CSS UI\n\n";
    agentsContent += "Game UI is standard HTML and CSS with Lua logic (no JavaScript), one UI Document per GameObject (MCP: "
                     "`create_ui_document`), under `assets/ui/`. `check --ui` reports what the engine drops or rewrites. Supported CSS: "
                     "`jasper://docs/html-ui-support`; guide: `jasper://docs/html-ui`; examples to copy: `" + api +
                     "/html-ui-examples/`.\n\n";
    agentsContent += "## Source references\n\n";
    agentsContent += "The runtime API this project links against: `" + api + "` (use it first). Engine and editor source: "
                     "https://github.com/JasperRaccoon/JasperEngine, at the tag or commit of the editor version above (report it if you "
                     "cannot match it). The `.scene`, `.prefab` and `project.yaml` formats are defined in its `editor/` directory. An editor "
                     "key is rebound with `set_shortcut` (tool group `editor_ux`), not in that source.\n\n";
    agentsContent += "## Generated files\n\n";
    agentsContent += "The editor regenerates these on play and build; never edit them: the root `CMakeLists.txt`, `" + internal +
                     "/LocalPaths.cmake`, `" + internal + "/scene_scripts.cpp`, `" + internal + "/generated/`, `" + api + "/`. After "
                     "changing a `.scene`, `.prefab` or `project.yaml` outside the editor, play the scene in the editor to regenerate "
                     "before validating a build.\n\n";
    agentsContent += "## Version control\n\n";
    agentsContent += "Commit `project.yaml`, `*.scene`, `*.prefab`, `ProjectBuild.cmake`, `AGENTS.md` and the assets. `" + internal +
                     "/` (generated code, this machine's build settings, layouts, `data://` play data) and `CMakeLists.txt` are not "
                     "committed. Per-user or per-machine values belong in `" + internal + "/user/`, never in `project.yaml`.\n\n";
    agentsContent += "## Custom build settings\n\n";
    agentsContent += "Put your own CMake in `ProjectBuild.cmake` at the project root (the editor never writes it; play and export include "
                     "it). Use `${JASPERENGINE_TARGET}` for the target and `${JASPERENGINE_SCRIPTS_DIR}` as the root of script paths:\n\n";
    agentsContent += "```cmake\n";
    agentsContent += "target_include_directories(${JASPERENGINE_TARGET} PRIVATE ${JASPERENGINE_SCRIPTS_DIR}/Source/Game/Public)\n";
    agentsContent += "target_link_libraries(${JASPERENGINE_TARGET} PRIVATE mylib)\n";
    agentsContent += "```\n\n";
    agentsContent += "## C++ scripts\n\n";
    agentsContent += "`.h`/`.cpp` files anywhere outside `" + internal + "/`. A script class inherits `jasperengine::ScriptBase` "
                     "(`#include \"ScriptBase.h\"`) or the GameObject's own class (`jasperengine::Sprite`...), declares a "
                     "`(jasperengine::Scene* scene, jasperengine::Entity entity)` constructor, and marks Inspector fields "
                     "`DPROPERTY(\"Name\")`; it overrides `init()`, `onEnable()`, `start()`, `onDisable()`, `onDestroy()` as needed. The "
                     "editor finds and builds scripts on play (or `compile_scripts`); a script root directory (Project Settings > "
                     "Directories) is compiled whole and is an include root. The C++ standard is Project Settings > Scripting > C++ "
                     "Standard (20 or 23). Lua scripts are loaded at runtime, not compiled.\n\n";
    agentsContent += "## Prefabs at runtime\n\n";
    agentsContent += "Unity's Instantiate / Destroy are `GameObject::instantiate(prefab or GameObject [, parent | position, rotation])` "
                     "(returns `<name>(Clone)`, its scripts started) and `GameObject::destroy(object, delay)` (Lua: "
                     "`GameObject.instantiate`, `GameObject.destroy`). A prefab loaded only by path (`PrefabAsset::load`) must be listed "
                     "under `standalonePrefabs` (editor: Assets > Prefabs) or the build lacks it.\n\n";
    agentsContent += "## Physics backends\n\n";
    agentsContent += "Project Settings > Build > 2D/3D Physics only changes the export, which drops a disabled backend: code that must "
                     "build either way guards on `JASPERENGINE_PHYSICS_2D` / `JASPERENGINE_PHYSICS_3D`.\n";

    FileUtils::writeIfChanged(agentsFile, agentsContent);
    writeClaudeFile(projectPath);
}

// Claude Code reads CLAUDE.md, not AGENTS.md: a project without one gets a CLAUDE.md that imports
// AGENTS.md. Written once, when missing; an existing CLAUDE.md is never changed.
void editor::Generator::writeClaudeFile(const fs::path& projectPath) {
    const fs::path claudeFile = projectPath / "CLAUDE.md";
    std::error_code ec;
    const fs::file_status claudeStatus = fs::symlink_status(claudeFile, ec);
    if (fs::exists(claudeStatus) || (ec && claudeStatus.type() != fs::file_type::not_found)) return;
    FileUtils::writeIfChanged(claudeFile, "@AGENTS.md\n");
}

const std::vector<std::string>& editor::Generator::engineApiIncludeDirs() {
    static const std::vector<std::string> dirs = {
        // libs/json: core/native/NativeBridge.h (NativeJson) and script/LuaJson.h
        "", "libs/sokol", "libs/json", "libs/box2d/include", "libs/joltphysics", "renders",
        "core", "core/action", "core/action/keyframe", "core/buffer", "core/component", "core/ecs",
        "core/io", "core/manager", "core/math", "core/object", "core/object/sound", "core/object/ui",
        "core/object/environment", "core/object/physics", "core/pool", "core/registry", "core/render",
        "core/script", "core/shader", "core/subsystem", "core/texture",
        // not core/util: its Signal.h would shadow <signal.h> on case-insensitive file systems
        // (macOS, Windows); its headers are "util/Name.h" through "core"
    };
    return dirs;
}

namespace jasperengine::editor {
namespace {

// First line of the editor's .clangd: a file without it belongs to the user
const char* const clangdMarker = "# Generated by JasperEngine Editor.";

std::string normalizeLibraryEntry(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.rfind("./", 0) == 0) value.erase(0, 2);
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

// .jasperengine/engine-api/lua: one stub file per Lua class and enum, generated from the bindings
// at build time (generate_lua_stubs.py). Files of classes the engine no longer binds go.
void writeLuaApiStubs(const fs::path& stubsDir) {
    std::unordered_set<std::string> names;
    int failed = 0;
    for (const auto& file : luastubs::stubFiles) {
        std::string content;
        for (unsigned int i = 0; i < file.partCount; i++) {
            content += file.parts[i];
        }
        names.insert(file.name);
        if (!FileUtils::writeIfChanged(stubsDir / file.name, content)) {
            failed++;
        }
    }
    if (failed > 0) {
        Out::warning("Could not write %d Lua API stub files in %s", failed, stubsDir.string().c_str());
    }
    std::error_code ec;
    for (auto it = fs::directory_iterator(stubsDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code fileEc;
        const fs::path path = it->path();
        if (path.extension() == ".lua" && it->is_regular_file(fileEc) && names.count(path.filename().string()) == 0) {
            FileUtils::removePath(path, fileEc);
        }
    }
}

// engine-api/luals-plugin.lua.txt: the lua-language-server plugin generated with the stubs
// (generate_lua_stubs.py, LUALS_PLUGIN): it has an assignment to a static field of an engine
// class through the global table (`Engine.timeScale = "fast"`) checked against the field's type,
// which the server otherwise reads as the definition of a new global. Not a .lua file, so the
// server does not take it for a script of the project.
void writeLuaLsPlugin(const fs::path& file) {
    std::string content;
    for (unsigned int i = 0; i < luastubs::luaLsPlugin.partCount; i++) {
        content += luastubs::luaLsPlugin.parts[i];
    }
    if (!FileUtils::writeIfChanged(file, content)) {
        Out::warning("Could not write the lua-language-server plugin %s", file.string().c_str());
    }
}

// runtime.plugin of .luarc.json: the engine's plugin is added, first when the file lists plugins
// of its own (the server runs them in order and keeps the last one's result; the engine's plugin
// passes the events it does not use on). Returns whether `root` changed.
bool mergeLuaRcPlugin(nlohmann::json& root, const std::string& plugin) {
    using Json = nlohmann::json;
    Json* entry = nullptr;
    if (root.contains("runtime.plugin")) {
        entry = &root["runtime.plugin"];
    } else if (root.contains("runtime") && root["runtime"].is_object()) {
        entry = &root["runtime"]["plugin"];
    } else {
        entry = &root["runtime.plugin"];
    }
    auto isOurs = [&](const Json& value) {
        return value.is_string() && normalizeLibraryEntry(value.get<std::string>()) == normalizeLibraryEntry(plugin);
    };
    if (entry->is_null() || (entry->is_string() && entry->get<std::string>().empty())) {
        *entry = plugin;
        return true;
    }
    if (entry->is_string()) {
        if (isOurs(*entry)) {
            return false;
        }
        *entry = Json::array({plugin, *entry});
        return true;
    }
    if (entry->is_array()) {
        for (const auto& value : *entry) {
            if (isOurs(value)) {
                return false;
            }
        }
        entry->insert(entry->begin(), plugin);
        return true;
    }
    Out::warning(".luarc.json: runtime.plugin is not a path or a list; leaving it unchanged (add \"%s\" for the checks of the engine classes' static fields).", plugin.c_str());
    return false;
}

// .luarc.json at the project root, read by lua-language-server (and the VS Code Lua extension).
// An existing file is merged into: only a missing library entry, Lua version or the engine's
// plugin is added, in the style (dotted or nested keys) the file already uses.
// workspace.ignoreDir of .luarc.json: the HTML UI examples' copy is reference material, not
// project code (its Lua classes would be defined twice once a template is instantiated). The
// server's default (.vscode) is kept when the file has no list yet. Returns whether `root` changed.
bool mergeLuaRcIgnoreDir(nlohmann::json& root, const std::string& dir) {
    using Json = nlohmann::json;
    Json* entry = nullptr;
    if (root.contains("workspace.ignoreDir")) {
        entry = &root["workspace.ignoreDir"];
    } else if (root.contains("workspace") && root["workspace"].is_object()) {
        entry = &root["workspace"]["ignoreDir"];
    } else {
        entry = &root["workspace.ignoreDir"];
    }
    if (entry->is_null()) {
        *entry = Json::array({".vscode", dir});
        return true;
    }
    if (!entry->is_array()) {
        return false;
    }
    for (const auto& value : *entry) {
        if (value.is_string() && normalizeLibraryEntry(value.get<std::string>()) == normalizeLibraryEntry(dir)) {
            return false;
        }
    }
    entry->push_back(dir);
    return true;
}

void writeLuaRc(const fs::path& projectPath, const std::string& library, const std::string& plugin, const std::string& ignoreDir) {
    jasperengine::editor::safewrite::EditorInternal ownLuarc(projectPath / ".luarc.json");   // the editor's own dot-file (util/SafeWrite.h)
    using Json = nlohmann::json;
    const fs::path file = projectPath / ".luarc.json";
    const std::string luaVersion = "Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR;

    std::error_code ec;
    if (!fs::exists(file, ec)) {
        Json root = Json::object();
        root["$schema"] = "https://raw.githubusercontent.com/LuaLS/vscode-lua/master/setting/schema.json";
        root["runtime.version"] = luaVersion;
        root["workspace.library"] = Json::array({library});
        root["runtime.plugin"] = plugin;
        mergeLuaRcIgnoreDir(root, ignoreDir);
        FileUtils::writeIfChanged(file, root.dump(4) + "\n");
        return;
    }

    std::ifstream input(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input && !input.eof()) {
        Out::warning("Could not read .luarc.json; leaving it unchanged.");
        return;
    }
    Json root = Json::parse(text, nullptr, false, true);
    if (root.is_discarded() || !root.is_object()) {
        Out::warning(".luarc.json is not a JSON object; leaving it unchanged (add \"%s\" to workspace.library for the engine's Lua API).", library.c_str());
        return;
    }

    bool changed = false;
    const bool hasVersion = root.contains("runtime.version") ||
        (root.contains("runtime") && root["runtime"].is_object() && root["runtime"].contains("version"));
    if (!hasVersion) {
        if (root.contains("runtime") && root["runtime"].is_object()) {
            root["runtime"]["version"] = luaVersion;
        } else {
            root["runtime.version"] = luaVersion;
        }
        changed = true;
    }

    Json* libraries = nullptr;
    if (root.contains("workspace.library")) {
        libraries = &root["workspace.library"];
    } else if (root.contains("workspace") && root["workspace"].is_object()) {
        libraries = &root["workspace"]["library"];
    } else {
        libraries = &root["workspace.library"];
    }
    if (libraries->is_null()) {
        *libraries = Json::array();
    }
    if (!libraries->is_array()) {
        Out::warning(".luarc.json: workspace.library is not a list; leaving it unchanged (add \"%s\" for the engine's Lua API).", library.c_str());
    } else {
        bool present = false;
        for (const auto& entry : *libraries) {
            if (entry.is_string() && normalizeLibraryEntry(entry.get<std::string>()) == normalizeLibraryEntry(library)) {
                present = true;
                break;
            }
        }
        if (!present) {
            libraries->push_back(library);
            changed = true;
        }
    }

    if (mergeLuaRcPlugin(root, plugin)) {
        changed = true;
    }
    if (mergeLuaRcIgnoreDir(root, ignoreDir)) {
        changed = true;
    }

    if (changed) {
        FileUtils::writeIfChanged(file, root.dump(4) + "\n");
    }
}

void collectCppSources(const fs::path& root, std::vector<std::string>& out, std::unordered_set<std::string>& seen) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return;
    }
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code entryEc;
        const std::string name = it->path().filename().string();
        // as script discovery (Generator::writeSourceFiles): hidden and build directories hold
        // generated sources and build trees
        if (it->is_directory(entryEc)) {
            if (name.empty() || name[0] == '.' || name == "build") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_regular_file(entryEc) && Util::isSourceFile(name)) {
            const std::string path = it->path().lexically_normal().generic_string();
            if (seen.insert(path).second) {
                out.push_back(path);
            }
        }
    }
}

// compile_commands.json for clangd in <internal>/clangd: every C++ source of the project with
// the flags its plugin build uses (generated CMakeLists.txt: C++ standard, definitions, include
// directories), plus engine_api.cpp, which includes the whole engine API so clangd indexes it.
// Paths are absolute (clangd needs them); the editor rewrites the file whenever the project is
// opened or built, so a moved project is fixed the next time it is opened. clangd infers the
// flags of headers and of new files from these entries.
void writeCompileDatabase(const fs::path& projectPath, const fs::path& projectInternalPath, int cxxStandard, const std::vector<fs::path>& scriptDirs) {
    using Json = nlohmann::json;
    const fs::path dbDir = projectInternalPath / "clangd";
    const fs::path project = projectPath.lexically_normal();
    const fs::path engineApi = (projectInternalPath / "engine-api").lexically_normal();

    std::vector<std::string> flags = {
        "c++", "-std=c++" + std::to_string(cxxStandard),
        "-DJASPERENGINE_PHYSICS_2D", "-DJASPERENGINE_PHYSICS_3D",
        "-DJASPERENGINE_EDITOR_PLUGIN", "-DJASPERENGINE_SHARED", "-DJASPERENGINE_EDITOR",
        "-I" + project.generic_string(),
    };
    std::vector<fs::path> roots = {project};
    for (const fs::path& dir : scriptDirs) {
        const fs::path absolute = (dir.is_absolute() ? dir : project / dir).lexically_normal();
        flags.push_back("-I" + absolute.generic_string());
        roots.push_back(absolute);
    }
    for (const std::string& dir : editor::Generator::engineApiIncludeDirs()) {
        flags.push_back("-isystem");
        flags.push_back((dir.empty() ? engineApi : engineApi / dir).generic_string());
    }

    const fs::path anchor = dbDir / "engine_api.cpp";
    FileUtils::writeIfChanged(anchor,
        "// Generated by JasperEngine Editor: makes clangd index the engine API of this project.\n"
        "#include \"JasperEngine.h\"\n");

    std::vector<std::string> sources;
    std::unordered_set<std::string> seen;
    for (const fs::path& root : roots) {
        collectCppSources(root, sources, seen);
    }
    std::sort(sources.begin(), sources.end());
    sources.insert(sources.begin(), anchor.lexically_normal().generic_string());

    Json db = Json::array();
    for (const std::string& source : sources) {
        Json args = flags;
        args.push_back("-c");
        args.push_back(source);
        db.push_back({{"directory", project.generic_string()}, {"file", source}, {"arguments", args}});
    }
    if (!FileUtils::writeIfChanged(dbDir / "compile_commands.json", db.dump(2) + "\n")) {
        Out::warning("Could not write %s", (dbDir / "compile_commands.json").string().c_str());
    }
}

// .clangd at the project root: points clangd at the compile database. A .clangd without the
// marker line is the user's and stays as it is.
void writeClangdConfig(const fs::path& projectPath, const std::string& databaseDir) {
    const fs::path file = projectPath / ".clangd";
    std::error_code ec;
    if (fs::exists(file, ec)) {
        std::ifstream input(file);
        std::string firstLine;
        std::getline(input, firstLine);
        if (firstLine.rfind(clangdMarker, 0) != 0) {
            return;
        }
    }
    std::string content;
    content += std::string(clangdMarker) + " Delete this line to keep your own edits: the editor then leaves this file alone.\n";
    content += "# The compile commands of this project's C++ scripts (C++ standard, engine API include\n";
    content += "# directories) are in " + databaseDir + ", rewritten when the project is opened or built.\n";
    content += "CompileFlags:\n";
    content += "  CompilationDatabase: " + databaseDir + "\n";
    FileUtils::writeIfChanged(file, content);
}

} // namespace
} // namespace jasperengine::editor

void editor::Generator::writeEditorTooling(const fs::path& projectPath, const fs::path& projectInternalPath, int cxxStandard, const std::vector<fs::path>& scriptDirs) {
    // the editor's clangd/lua tooling: the internal folder, .clangd and .vscode at the project root
    safewrite::EditorInternal ownInternal(projectInternalPath);
    safewrite::EditorInternal ownClangd(projectPath / ".clangd");
    safewrite::EditorInternal ownVscode(projectPath / ".vscode");
    try {
        const fs::path relativeInternalPath = fs::relative(projectInternalPath, projectPath);
        const fs::path stubsRelative = relativeInternalPath / "engine-api" / "lua";
        const fs::path pluginRelative = relativeInternalPath / "engine-api" / luastubs::luaLsPlugin.name;
        writeLuaApiStubs(projectPath / stubsRelative);
        writeLuaLsPlugin(projectPath / pluginRelative);
        // the Lua API as data (S23 R46), next to the stubs: for tools and agents
        if (!FileUtils::writeIfChanged(projectPath / relativeInternalPath / "engine-api" / "lua-api.json", LuaApiData::jsonText())) {
            Out::warning("Could not write engine-api/lua-api.json");
        }
        writeLuaRc(projectPath, stubsRelative.generic_string(), pluginRelative.generic_string(),
                   (relativeInternalPath / "engine-api" / "html-ui-examples").generic_string());
        writeCompileDatabase(projectPath, projectInternalPath, cxxStandard, scriptDirs);
        writeClangdConfig(projectPath, (relativeInternalPath / "clangd").generic_string());
        std::string examplesError;
        if (!HtmlUiExamples::writeTo(projectInternalPath / "engine-api" / "html-ui-examples", examplesError)) {
            Out::error("Failed to write the HTML UI examples: %s", examplesError.c_str());
        }
    } catch (const std::exception& e) {
        Out::error("Failed to write the code tool files (Lua API stubs, .luarc.json, .clangd): %s", e.what());
    }
}

std::vector<editor::PrefabInstanceInfo> editor::Generator::writePrefabSources(const std::map<fs::path, Prefab>& prefabAssets, uint32_t sceneId, const fs::path& projectPath, const fs::path& projectInternalPath,
        const std::function<std::string(const fs::path&)>& prefabGuid, const std::map<fs::path, std::shared_ptr<BakedPrefab>>& baked) {
    // prefab_*.cpp/.h in the editor's generated folder (util/SafeWrite.h, EditorInternal)
    safewrite::EditorInternal ownGenerated(getGeneratedPath(projectInternalPath));
    const fs::path generatedPath = getGeneratedPath(projectInternalPath);

    std::vector<PrefabInstanceInfo> prefabInstances;

    // members plus every nested subtree, what build_prefab_* appends for that prefab
    std::map<fs::path, size_t> subtreeSizes;
    auto subtreeSize = [&](auto&& self, const fs::path& path) -> size_t {
        auto it = prefabAssets.find(path);
        if (it == prefabAssets.end() || !it->second.registry) return 0;
        auto sizeIt = subtreeSizes.find(path);
        if (sizeIt != subtreeSizes.end()) return sizeIt->second;
        subtreeSizes[path] = 0; // also stops a cycle
        EntityRegistry* registry = it->second.registry.get();
        size_t total = 0;
        for (Entity member : Factory::getPrefabMemberEntities(registry, it->second.registryEntities, it->second.root)) {
            total++;
            PrefabComponent* bc = registry->findComponent<PrefabComponent>(member);
            if (bc && !bc->path.empty()) total += self(self, bc->path);
        }
        // a Prefab Variant: its base's GameObjects after its own
        if (const PrefabComponent* base = it->second.getBaseComponent(); base && !base->path.empty()) {
            total += self(self, base->path);
        }
        subtreeSizes[path] = total;
        return total;
    };

    // The base level of a variant instance (an instance of the base at the root of an
    // instance of the variant): described with the variant's instance, not on its own
    auto variantOver = [&](const fs::path& prefabPath, Entity root) -> const fs::path* {
        for (const auto& [otherPath, other] : prefabAssets) {
            const PrefabComponent* base = other.getBaseComponent();
            if (!base || fs::path(base->path) != prefabPath) continue;
            auto sceneIt = other.instances.find(sceneId);
            if (sceneIt == other.instances.end()) continue;
            for (const auto& instance : sceneIt->second) {
                if (instance.rootEntity == root) return &otherPath;
            }
        }
        return nullptr;
    };
    std::map<std::pair<std::string, Entity>, std::shared_ptr<PrefabInstanceInfo>> baseLevels;
    std::vector<std::pair<fs::path, size_t>> variantInfos;  // (base path, index in prefabInstances)

    for (const auto& [prefabPath, prefab] : prefabAssets) {
        auto sceneIt = prefab.instances.find(sceneId);
        if (sceneIt != prefab.instances.end()) {
            for (const auto& instance : sceneIt->second) {
                PrefabInstanceInfo info;
                info.prefabPath = prefabPath;
                info.rootEntity = instance.rootEntity;
                for (const auto& member : instance.members) {
                    info.memberEntities.insert(member.localEntity);
                }
                if (prefab.registry) {
                    for (Entity regEntity : Factory::getPrefabMemberEntities(prefab.registry.get(), prefab.registryEntities, prefab.root)) {
                        info.orderedMembers.push_back(prefab.getLocalEntity(sceneId, instance.instanceId, regEntity));
                        PrefabComponent* bc = prefab.registry->findComponent<PrefabComponent>(regEntity);
                        info.nestedSizes.push_back((bc && !bc->path.empty()) ? subtreeSize(subtreeSize, bc->path) : 0);
                    }
                }

                // Build override info from instance overrides. The root is a scene entity
                // written with its own components: only what it removed is left to do.
                for (const auto& [sceneEntity, objectOverrides] : instance.overrides) {
                    if (sceneEntity == instance.rootEntity) {
                        info.rootRemovedComponents.assign(objectOverrides.removedComponents.begin(), objectOverrides.removedComponents.end());
                        continue;
                    }
                    PrefabOverrideInfo ovr;
                    ovr.sceneEntity = sceneEntity;
                    for (const auto& [componentType, properties] : objectOverrides.properties) {
                        ovr.overriddenComponents.push_back(componentType);
                    }
                    ovr.addedComponents.assign(objectOverrides.addedComponents.begin(), objectOverrides.addedComponents.end());
                    ovr.removedComponents.assign(objectOverrides.removedComponents.begin(), objectOverrides.removedComponents.end());
                    ovr.state = objectOverrides.state;
                    if (!ovr.overriddenComponents.empty() || !ovr.addedComponents.empty() || !ovr.removedComponents.empty() || ovr.state != 0) {
                        info.overrides.push_back(std::move(ovr));
                    }
                }

                if (variantOver(prefabPath, instance.rootEntity)) {
                    baseLevels[{prefabPath.generic_string(), instance.rootEntity}] = std::make_shared<PrefabInstanceInfo>(std::move(info));
                    continue;
                }
                if (const PrefabComponent* base = prefab.getBaseComponent(); base && !base->path.empty()) {
                    variantInfos.emplace_back(fs::path(base->path), prefabInstances.size());
                }
                prefabInstances.push_back(std::move(info));
            }
        }

        if (!prefab.registry) {
            continue;
        }

        // Write prefab .h and .cpp files, from the prefab's instance as the editor makes it
        std::string headerContent = Factory::createPrefabHeader(prefabPath);
        auto bakedIt = baked.find(prefabPath);
        static const BakedPrefab unbaked;
        std::string sourceContent = Factory::createPrefab(prefabPath, bakedIt != baked.end() && bakedIt->second ? *bakedIt->second : unbaked,
            projectPath, generatedPath, prefabGuid);

        std::string prefabFileName = Factory::prefabToFileName(prefabPath);
        FileUtils::writeIfChanged(generatedPath / (prefabFileName + ".h"), headerContent);
        FileUtils::writeIfChanged(generatedPath / (prefabFileName + ".cpp"), sourceContent);
    }

    // Each variant instance gets its base level (and that one its own, down the chain)
    std::function<void(PrefabInstanceInfo&, const fs::path&, size_t)> attachBase = [&](PrefabInstanceInfo& info, const fs::path& basePath, size_t depth) {
        auto found = baseLevels.find({basePath.generic_string(), info.rootEntity});
        if (found == baseLevels.end() || depth > 64) {
            return;
        }
        info.baseLevel = found->second;
        info.baseSize = subtreeSize(subtreeSize, basePath);
        auto baseIt = prefabAssets.find(basePath);
        if (baseIt != prefabAssets.end()) {
            if (const PrefabComponent* deeper = baseIt->second.getBaseComponent(); deeper && !deeper->path.empty()) {
                attachBase(*info.baseLevel, fs::path(deeper->path), depth + 1);
            }
        }
    };
    for (const auto& [basePath, index] : variantInfos) {
        attachBase(prefabInstances[index], basePath, 0);
    }

    return prefabInstances;
}

void editor::Generator::writeSceneSource(Scene* scene, uint32_t sceneId, const std::string& sceneName, const std::vector<Entity>& entities, const Entity camera, const fs::path& projectPath, const fs::path& projectInternalPath, std::vector<PrefabInstanceInfo>& prefabInstances, const SceneResources& resources){
    safewrite::EditorInternal ownInternal(projectInternalPath);   // generated sources under .jasperengine
    const fs::path generatedPath = getGeneratedPath(projectInternalPath);

    std::string sceneContent = Factory::createScene(0, scene, sceneId, sceneName, entities, camera, projectPath, generatedPath, prefabInstances, resources);

    std::string filename = Factory::sceneToFileName(sceneId, sceneName) + ".cpp";
    const fs::path sourceFile = generatedPath / filename;

    FileUtils::writeIfChanged(sourceFile, sceneContent);
}

void editor::Generator::clearSceneSource(uint32_t sceneId, const std::string& sceneName, const fs::path& projectInternalPath) {
    safewrite::EditorInternal ownInternal(projectInternalPath);   // generated sources under .jasperengine
    const fs::path generatedPath = getGeneratedPath(projectInternalPath);
    std::string filename = Factory::sceneToFileName(sceneId, sceneName) + ".cpp";
    const fs::path sourceFile = generatedPath / filename;

    if (fs::exists(sourceFile)) {
        std::error_code ec;
        FileUtils::removePath(sourceFile, ec);
        if (ec) {
            Out::error("Failed to remove scene source file '%s': %s", sourceFile.string().c_str(), ec.message().c_str());
        }
    }
}

bool editor::Generator::hasSceneSource(uint32_t sceneId, const std::string& sceneName, const fs::path& projectInternalPath) const {
    // one written by an older editor lacks what main.cpp calls
    std::ifstream file(getGeneratedPath(projectInternalPath) / (Factory::sceneToFileName(sceneId, sceneName) + ".cpp"));
    std::string header;
    return std::getline(file, header) && header == Factory::sceneSourceHeader;
}

void editor::Generator::configure(const std::vector<editor::SceneBuildInfo>& scenes, std::string libName, const std::vector<SceneScriptSource>& scriptFiles, const std::vector<editor::PrefabSceneInfo>& prefabs, const fs::path& projectPath, const fs::path& projectInternalPath, const fs::path& assetsPath, const fs::path& luaPath, const std::vector<fs::path>& scriptDirs, int cxxStandard, Scaling scalingMode, TextureStrategy textureStrategy, unsigned int canvasWidth, unsigned int canvasHeight, bool vsyncEnabled, const WindowSettings& windowSettings, const LoadingSettings& loadingSettings, const TagsAndLayers& tagsAndLayers, int htmlUiMsaaSamples, const ThreadingProjectSettings& threading, const HtmlUiRenderSettings& htmlUiRender, PbrTier mobilePbrTier, const ShaderCreationProjectSettings& shaderCreation, bool allowInsecureHttp, const std::vector<ShaderGlobalDecl>& shaderGlobals, const PlayerSettings& playerSettings, const AudioSettings& audioSettings, const DebugSettings& debugSettings){
    // the generated sources (main.cpp, scene and prefab factories) live in the editor's internal
    // folder, which an MCP call's deny-by-default guard refuses otherwise (util/SafeWrite.h)
    safewrite::EditorInternal ownInternal(projectInternalPath);
    const fs::path generatedPath = getGeneratedPath(projectInternalPath);

    // The editor used to emit a GLFW application host into every project. It is
    // gone, but projects created before that still carry the files, and nothing
    // else removes them; the exported build globs every *.cpp under the project
    // root, so a leftover would be compiled against an engine that no longer
    // has GLFW in it.
    std::error_code removeError;
    FileUtils::removePath(generatedPath / "PlatformEditor.h", removeError);
    FileUtils::removePath(generatedPath / "PlatformEditor.cpp", removeError);

    // Build main.cpp content
    std::string mainContent;
    mainContent += "// This file is auto-generated by JasperEngine Editor. Do not edit manually.\n\n";
    mainContent += "#include \"JasperEngine.h\"\n";
    mainContent += "#ifdef JASPERENGINE_PROJECT_PATH\n";
    mainContent += "#include \"shader/ShaderManifest.h\"\n";
    mainContent += "#include \"shader/ShaderVariantCompiler.h\"\n";
    mainContent += "#include \"render/shadow/ShadowTiers.h\"\n";
    mainContent += "#include <fstream>\n";
    mainContent += "#include <iterator>\n";
    mainContent += "#endif\n";
    // Project Settings > Graphics > Light Mode, the editor's own (Project::setLightMode): only
    // when not Automatic, the engine's default
    const LightMode lightMode = LightingTiers::getLightMode();
    if (lightMode != LightMode::AUTOMATIC)
        mainContent += "#include \"render/LightingTiers.h\"\n";

    // Include prefab headers (contain declarations for prefab creation functions)
    for (const auto& prefabData : prefabs) {
        std::string headerName = Factory::prefabToFileName(prefabData.prefabPath) + ".h";
        mainContent += "#include \"" + headerName + "\"\n";
    }
    if (!prefabs.empty()) {
        mainContent += "\n";
    }

    mainContent += "using namespace jasperengine;\n\n";

    // Forward declarations for per-scene initialization functions (defined in generated scene .cpp files)
    for (const auto& sceneData : scenes) {
        std::string sceneName = Factory::sceneToFileName(sceneData.id, sceneData.name);
        mainContent += "void create_" + sceneName + "(Scene* scene);\n";
        mainContent += "SceneResources resources_" + sceneName + "();\n";
    }
    mainContent += "extern \"C\" void initScripts(jasperengine::Scene* scene);\n";
    mainContent += "extern \"C\" void cleanupScripts(jasperengine::Scene* scene);\n";
    mainContent += "extern \"C\" void initEntityScripts(jasperengine::Scene* scene, const jasperengine::Entity* entities, size_t count);\n";
    mainContent += "extern \"C\" void cleanupEntityScripts(jasperengine::Scene* scene, const jasperengine::Entity* entities, size_t count);\n";
    mainContent += "extern \"C\" void moveEntityScripts(jasperengine::Scene* from, jasperengine::Scene* to, const jasperengine::Entity* oldEntities, const jasperengine::Entity* newEntities, size_t count);\n";
    mainContent += "\n";

    std::map<uint32_t, std::string> sceneIdToName;
    for (const auto& sceneData : scenes) {
        std::string sceneName = "_" + Factory::sceneToFileName(sceneData.id, sceneData.name);
        mainContent += "static Scene* " + sceneName + " = nullptr;\n";
        sceneIdToName[sceneData.id] = sceneName;
    }
    mainContent += "\n";

    // Shared by both stack functions: create the scenes, register their pointers so
    // cross-scene references resolve, then build and start the new ones
    auto emitStackSetup = [&](const SceneBuildInfo& sceneData) {
        for (const auto sceneId : sceneData.involvedScenes) {
            std::string sceneName = sceneIdToName[sceneId];
            mainContent += "    bool " + sceneName + "_needsInit = false;\n";
        }
        mainContent += "\n";
        for (const auto sceneId : sceneData.involvedScenes) {
            std::string sceneName = sceneIdToName[sceneId];
            mainContent += "    if (!" + sceneName + "){\n";
            mainContent += "        " + sceneName + " = new Scene();\n";
            mainContent += "        SceneManager::setScenePtr(" + std::to_string(sceneId) + ", " + sceneName + ");\n";
            mainContent += "        " + sceneName + "_needsInit = true;\n";
            mainContent += "    }\n";
        }
        mainContent += "\n";
        for (const auto sceneId : sceneData.involvedScenes) {
            std::string sceneName = sceneIdToName[sceneId];
            mainContent += "    if (" + sceneName + "_needsInit) {\n";
            mainContent += "        create" + sceneName + "(" + sceneName + ");\n";
            mainContent += "    }\n";
        }
        mainContent += "\n";
        for (const auto sceneId : sceneData.involvedScenes) {
            std::string sceneName = sceneIdToName[sceneId];
            mainContent += "    if (" + sceneName + "_needsInit) {\n";
            mainContent += "        initScripts(" + sceneName + ");\n";
            mainContent += "    }\n";
        }
    };

    // Per-stack load and add functions. The add one only creates the scenes, leaving the
    // running stack alone; SceneManager::addChildScene() puts them on screen.
    for (const auto& sceneData : scenes) {
        std::string stackId = Factory::sceneToFileName(sceneData.id, sceneData.name);
        mainContent += "// --- Scene stack: " + sceneData.name + " ---\n";

        mainContent += "void load_" + stackId + "() {\n";
        // A loaded stack starts again, except scenes still on screen (the loading scene)
        for (const auto sceneId : sceneData.involvedScenes) {
            std::string sceneName = sceneIdToName[sceneId];
            mainContent += "    if (" + sceneName + " && !Engine::isSceneRunning(" + sceneName + ")) {\n";
            mainContent += "        cleanupScripts(" + sceneName + ");\n";
            mainContent += "        SceneManager::removeScenePtr(" + std::to_string(sceneId) + ");\n";
            mainContent += "        delete " + sceneName + ";\n";
            mainContent += "        " + sceneName + " = nullptr;\n";
            mainContent += "    }\n";
        }
        mainContent += "\n";
        emitStackSetup(sceneData);
        mainContent += "\n";
        for (const auto sceneId : sceneData.activeScenes) {
            std::string sceneName = sceneIdToName[sceneId];
            if (sceneData.id == sceneId) {
                mainContent += "    Engine::setScene(" + sceneName + ");\n";
            } else {
                mainContent += "    Engine::addSceneLayer(" + sceneName + ");\n";
            }
        }
        mainContent += "\n";
        for (const auto& sceneDataAux: scenes) {
            if (sceneDataAux.id == sceneData.id) continue;
            if (std::find(sceneData.involvedScenes.begin(), sceneData.involvedScenes.end(), sceneDataAux.id) != sceneData.involvedScenes.end()) continue;
            std::string sceneName = "_" + Factory::sceneToFileName(sceneDataAux.id, sceneDataAux.name);

            // A script run by initScripts() above may have added a stack of its own through
            // SceneManager::addChildScene(). Deleting any of its scenes here would leave a
            // freed Scene behind, in the Engine or in a cross-scene reference, so the scene is
            // kept while any stack that involves it is on screen. Only the stack's active
            // scenes are shown, which is why its own pointer is not enough to decide.
            std::string guard = sceneName;
            for (const auto& owner : scenes) {
                if (std::find(owner.involvedScenes.begin(), owner.involvedScenes.end(), sceneDataAux.id) == owner.involvedScenes.end()) continue;
                guard += " && !Engine::isSceneRunning(_" + Factory::sceneToFileName(owner.id, owner.name) + ")";
            }
            mainContent += "    if (" + guard + ") {\n";
            mainContent += "        cleanupScripts(" + sceneName + ");\n";
            mainContent += "        SceneManager::removeScenePtr(" + std::to_string(sceneDataAux.id) + ");\n";
            mainContent += "        delete " + sceneName + ";\n";
            mainContent += "        " + sceneName + " = nullptr;\n";
            mainContent += "    }\n";
        }
        mainContent += "}\n\n";

        mainContent += "void add_" + stackId + "() {\n";
        emitStackSetup(sceneData);
        mainContent += "}\n\n";
    }

    // Entry point of the native application backend for this OS. On Apple the
    // process starts in platform/apple/macos/main.m instead, so defining main()
    // here would collide with it.
    // The markers let Exporter strip this block wholesale: an exported project
    // compiles the engine's own platform main.cpp, which already defines main().
    mainContent += "// JASPERENGINE_ENTRY_POINT_BEGIN\n";
    mainContent += "#if defined(_WIN32)\n";
    mainContent += "#include \"JasperEngineWin.h\"\n";
    mainContent += "int main(int argc, char* argv[]) {\n";
    mainContent += "    return JasperEngineWin::init(argc, argv);\n";
    mainContent += "}\n";
    mainContent += "#elif !defined(__APPLE__)\n";
    mainContent += "#include \"JasperEngineLinux.h\"\n";
    mainContent += "int main(int argc, char* argv[]) {\n";
    mainContent += "    return JasperEngineLinux::init(argc, argv);\n";
    mainContent += "}\n";
    mainContent += "#endif\n";
    mainContent += "// JASPERENGINE_ENTRY_POINT_END\n\n";

    mainContent += "JASPERENGINE_INIT void init() {\n";
    // A build of this project (not an export) reads the assets directory, while a shader fork is
    // named by its project-relative path wherever it is (the fork default <project>/shaders, or
    // assets/shaders/...): its manifest and sources come from the project root first, then the
    // assets directory, as in the editor (Project::findShaderFile). An export copies every fork
    // to its asset root at that path, so it needs none of this.
    mainContent += "#ifdef JASPERENGINE_PROJECT_PATH\n";
    mainContent += "    ShaderManifests::setForkReader([](const std::string& path, std::string& content) {\n";
    mainContent += "        if (!ShaderVariantCompiler::isSafeRelativePath(path))\n";
    mainContent += "            return false;\n";
    mainContent += "        for (const char* root : {JASPERENGINE_PROJECT_PATH, JASPERENGINE_ASSET_PATH}) {\n";
    mainContent += "            std::ifstream file(std::string(root) + \"/\" + path, std::ios::binary);\n";
    mainContent += "            if (file) {\n";
    mainContent += "                content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());\n";
    mainContent += "                return true;\n";
    mainContent += "            }\n";
    mainContent += "        }\n";
    mainContent += "        return false;\n";
    mainContent += "    });\n";
    mainContent += "#endif\n";
    // Threading first: the job system and the asset readers start with these counts
    const ThreadingProjectSettings threadingDefaults;
    if (threading.workers != threadingDefaults.workers || threading.reservedFrameWorkers != threadingDefaults.reservedFrameWorkers
        || threading.ioThreads != threadingDefaults.ioThreads || threading.webPumpBudgetMs != threadingDefaults.webPumpBudgetMs) {
        mainContent += "    {\n";
        mainContent += "        ThreadingSettings threading;\n";
        mainContent += "        threading.workers = " + std::to_string(threading.workers) + ";\n";
        mainContent += "        threading.reservedFrameWorkers = " + std::to_string(threading.reservedFrameWorkers) + ";\n";
        mainContent += "        threading.ioThreads = " + std::to_string(threading.ioThreads) + ";\n";
        mainContent += "        threading.webPumpBudgetMs = " + Factory::formatFloat(threading.webPumpBudgetMs) + ";\n";
        mainContent += "        Engine::setThreadingSettings(threading);\n";
        mainContent += "    }\n";
    }
    // Shader Creation (spec S32): applied when the view loads, which comes after main
    const ShaderCreationSettings shaderCreationEngine = shaderCreation.engineSettings();
    if (shaderCreationEngine != ShaderCreationSettings()) {
        mainContent += "    {\n";
        mainContent += "        ShaderCreationSettings shaderCreation;\n";
        mainContent += std::string("        shaderCreation.asyncCreation = ") + (shaderCreationEngine.asyncCreation ? "true" : "false") + ";\n";
        mainContent += "        shaderCreation.mainThreadBudgetMs = " + Factory::formatFloat(shaderCreationEngine.mainThreadBudgetMs) + ";\n";
        mainContent += std::string("        shaderCreation.glWarmDraws = ") + (shaderCreationEngine.glWarmDraws ? "true" : "false") + ";\n";
        mainContent += std::string("        shaderCreation.holdFirstSceneForWarmup = ") + (shaderCreationEngine.holdFirstSceneForWarmup ? "true" : "false") + ";\n";
        mainContent += "        Engine::setShaderCreationSettings(shaderCreation);\n";
        mainContent += "    }\n";
    }
    mainContent += "    Engine::setCanvasSize(" + std::to_string(canvasWidth) + ", " + std::to_string(canvasHeight) + ");\n";

    // Scaling mode
    switch (scalingMode) {
        case Scaling::FITWIDTH:  mainContent += "    Engine::setScalingMode(Scaling::FITWIDTH);\n"; break;
        case Scaling::FITHEIGHT: mainContent += "    Engine::setScalingMode(Scaling::FITHEIGHT);\n"; break;
        case Scaling::LETTERBOX: mainContent += "    Engine::setScalingMode(Scaling::LETTERBOX);\n"; break;
        case Scaling::CROP:      mainContent += "    Engine::setScalingMode(Scaling::CROP);\n"; break;
        case Scaling::STRETCH:   mainContent += "    Engine::setScalingMode(Scaling::STRETCH);\n"; break;
        case Scaling::NATIVE:    mainContent += "    Engine::setScalingMode(Scaling::NATIVE);\n"; break;
    }

    // the Canvas Scaler's project default (absent: Follow Game Canvas, the engine default)
    if (uiScale != CanvasScalerSettings())
        mainContent += "    Engine::setDefaultCanvasScaler(" + Factory::formatCanvasScaler(uiScale) + ");\n";

    // Texture strategy
    switch (textureStrategy) {
        case TextureStrategy::FIT:    mainContent += "    Engine::setTextureStrategy(TextureStrategy::FIT);\n"; break;
        case TextureStrategy::RESIZE: mainContent += "    Engine::setTextureStrategy(TextureStrategy::RESIZE);\n"; break;
        case TextureStrategy::NONE:   mainContent += "    Engine::setTextureStrategy(TextureStrategy::NONE);\n"; break;
    }

    // Atmosphere Quality per platform group (Project Settings > Quality), before any scene draws
    auto qualityName = [](AtmosphereQuality quality) -> std::string {
        switch (quality) {
            case AtmosphereQuality::OFF: return "AtmosphereQuality::OFF";
            case AtmosphereQuality::LOW: return "AtmosphereQuality::LOW";
            case AtmosphereQuality::MEDIUM: return "AtmosphereQuality::MEDIUM";
            case AtmosphereQuality::HIGH: return "AtmosphereQuality::HIGH";
        }
        return "AtmosphereQuality::LOW";
    };
    mainContent += "    Engine::setAtmosphereQualityDefaults(" + qualityName(atmosphereQuality[0]) + ", " + qualityName(atmosphereQuality[1]) + ", " +
                   qualityName(atmosphereQuality[2]) + ");\n";

    // Project Settings > Input: the deadzone, then the default Input Actions asset
    mainContent += inputSettingsInitSource(inputActionsAsset, inputDeadzone);
    // Project Settings > Navigation: the agent types and areas every navmesh and grid reads
    mainContent += navigationSettingsCode;
    // the texture quality, memory budget and low-memory fallback of the platform group
    // (Project Settings > Texture), before any texture loads
    {
        std::string groups[3];
        for (int g = 0; g < 3; g++) {
            groups[g] = "{" + std::to_string(textureQuality[g]) + ", " + std::to_string(textureBudgetMB[g]) + ", " +
                        (textureLowMemory[g] ? "true" : "false") + "}";
        }
        mainContent += "    Engine::setTextureDefaults(" + groups[0] + ", " + groups[1] + ", " + groups[2] + ");\n";
    }

    // the Device Tier the GPU gets and its overrides (Project Settings > Quality > Device Tiers),
    // before any scene: what differs from the defaults, "{}" for a default project
    mainContent += "    Device::configure(R\"JEDEVICE(" + deviceSettingsJson + ")JEDEVICE\");\n";

    // MSAA of the HTML UI targets (Project Settings > Graphics), before any document draws
    mainContent += "    UIDocumentSystem::setPreferredSampleCount(" + std::to_string(htmlUiMsaaSamples) + ");\n";
    // the HTML UI's glyph atlas budget, SDF primitives and transform-aware text (S16)
    mainContent += "    UIDocumentSystem::setHtmlUiRenderOptions(" + std::to_string(htmlUiRender.glyphAtlasMiB) + ", " +
                   (htmlUiRender.sdfPrimitives ? "true" : "false") + ", " + (htmlUiRender.transformAwareText ? "true" : "false") + ");\n";
    // the lobes a mobile or web build draws (Project Settings > Graphics > Mobile PBR Tier);
    // Reduced is the engine's default, desktop always draws them all
    if (mobilePbrTier != PbrTier::REDUCED)
        mainContent += "    MaterialPbr::setMobileTier(PbrTier::FULL);\n";

    // the shadow quality tiers of the platforms (Project Settings > Graphics > Shadows), the
    // editor's own: a scene of Quality Auto renders with its platform's
    static const char* const tierNames[] = {"LOW", "MEDIUM", "HIGH", "ULTRA"};
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++) {
        const ShadowTier tier = ShadowTiers::getPlatformTier(platform);
        if (tier != ShadowTiers::defaultTier(platform)) {
            mainContent += "    ShadowTiers::setPlatformTier(" + std::to_string(platform) + ", ShadowTier::" +
                tierNames[std::clamp(static_cast<int>(tier), 0, 3)] + ");\n";
        }
    }
    // GPU Instancing (spec S55), written when off
    if (!drawSubmission.gpuInstancing)
        mainContent += "    Engine::setGpuInstancingEnabled(false);\n";
    if (lightMode != LightMode::AUTOMATIC)
        mainContent += std::string("    LightingTiers::setLightMode(") +
            (lightMode == LightMode::TILED ? "LightMode::TILED" : "LightMode::PER_OBJECT") + ");\n";
    // Project Settings > Player > Allow Insecure HTTP: the same policy as the editor's Play
    mainContent += std::string("    Http::setInsecureHttpAllowed(") + (allowInsecureHttp ? "true" : "false") + ");\n";
    // The native plugins of plugins/native that the platform being built cannot run: a call to
    // one fails with "unavailable" and says where it runs (the editor's Play does the same)
    {
        const std::vector<NativePlugins::PluginInfo> plugins = NativePlugins::listPlugins(NativePlugins::scan(projectPath));
        auto platformOnlyCall = [&plugins](const std::string& running) {
            const std::map<std::string, std::string> only = NativePlugins::platformOnlyPlugins(plugins, running);
            std::string call = "    NativeBridge::setPlatformOnlyPlugins({";
            bool first = true;
            for (const auto& [name, message] : only) {
                call += std::string(first ? "" : ", ") + "{\"" + name + "\", \"" + message + "\"}";
                first = false;
            }
            return only.empty() ? std::string() : call + "});\n";
        };
        const std::string android = platformOnlyCall("android");
        const std::string ios = platformOnlyCall("ios");
        const std::string macos = platformOnlyCall("macos");
        const std::string macosCMake = platformOnlyCall("macos-cmake");
        const std::string web = platformOnlyCall("web");
        const std::string desktop = platformOnlyCall("desktop");
        if (!android.empty() || !ios.empty() || !macos.empty() || !macosCMake.empty() || !web.empty() || !desktop.empty()) {
            mainContent += "#if defined(__ANDROID__)\n" + android;
            mainContent += "#elif defined(__EMSCRIPTEN__)\n" + web;
            mainContent += "#elif defined(__APPLE__)\n";
            mainContent += "#include <TargetConditionals.h>\n";
            mainContent += "#if TARGET_OS_IPHONE\n" + ios + "#elif defined(JASPERENGINE_MACOS_CMAKE)\n" + macosCMake + "#else\n" + macos + "#endif\n";
            mainContent += "#else\n" + desktop;
            mainContent += "#endif\n";
        }
    }

    // Player > Resolution and Presentation (before any script, which may change them) and
    // what System.getAppName / getAppVersion... return where there is no package to ask
    {
        static const char* orientationNames[] = {"", "Portrait", "PortraitUpsideDown", "LandscapeLeft", "LandscapeRight", "AutoRotation"};
        const int orientation = std::clamp((int)playerSettings.defaultOrientation, 1, 5);
        char mask[8];
        snprintf(mask, sizeof(mask), "0x%02X", (unsigned int)playerSettings.allowedOrientations);
        mainContent += "    Engine::setVSyncCount(" + std::string(vsyncEnabled ? "1" : "0") + ");\n";
        mainContent += "    Engine::setTargetFrameRate(" + std::to_string(playerSettings.targetFrameRate) + ");\n";
        mainContent += "    Screen::initialize(ScreenOrientation::" + std::string(orientationNames[orientation]) + ", " + mask + ", " +
            (playerSettings.keepScreenOn ? "true" : "false") + ");\n";
        auto appInfo = [](const ApplicationInfo& info) {
            return "System::setApplicationInfo({" + Factory::formatString(info.name) + ", " + Factory::formatString(info.identifier) + ", " +
                Factory::formatString(info.version) + ", " + std::to_string((long long)info.build) + "});\n";
        };
        mainContent += "#if defined(_WIN32)\n";
        mainContent += "    " + appInfo(playerSettings.windowsApp);
        mainContent += "#elif defined(__EMSCRIPTEN__)\n";
        mainContent += "    " + appInfo(playerSettings.webApp);
        mainContent += "#elif defined(__APPLE__)\n";
        mainContent += "    " + appInfo(playerSettings.macosApp);
        mainContent += "#else\n";
        mainContent += "    " + appInfo(playerSettings.linuxApp);
        mainContent += "#endif\n";
    }

    // Tags and Layers, before any scene or script can ask for them
    if (!tagsAndLayers.tags.empty()) {
        mainContent += "    TagManager::setTags({";
        for (size_t i = 0; i < tagsAndLayers.tags.size(); i++) {
            if (i > 0) mainContent += ", ";
            mainContent += Factory::formatString(tagsAndLayers.tags[i]);
        }
        mainContent += "});\n";
    }
    for (size_t layer = 0; layer < tagsAndLayers.layerNames.size(); layer++) {
        if (tagsAndLayers.layerNames[layer].empty()) continue;
        mainContent += "    TagManager::setLayerName(" + std::to_string(layer) + ", " + Factory::formatString(tagsAndLayers.layerNames[layer]) + ");\n";
    }

    // Layer Collision Matrices: only the rows that do not collide with every layer
    auto emitLayerCollisionMatrix = [&mainContent](const char* physicsClass, const LayerCollisionRows& rows) {
        for (int layer = 0; layer < MAX_LAYERS; layer++) {
            if (rows[layer] == LAYER_MASK_EVERYTHING) continue;
            char mask[16];
            snprintf(mask, sizeof(mask), "0x%08Xu", (unsigned int)rows[layer]);
            mainContent += std::string("    ") + physicsClass + "::setLayerCollisionMask(" + std::to_string(layer) + ", " + mask + ");\n";
        }
    };
    emitLayerCollisionMatrix("Physics", tagsAndLayers.layerCollisionMatrix3D);
    emitLayerCollisionMatrix("Physics2D", tagsAndLayers.layerCollisionMatrix2D);

    // 3D world capacity, before any scene (and its world) is made: the same as in the editor
    mainContent += "#ifdef JASPERENGINE_PHYSICS_3D\n";
    mainContent += "    PhysicsSystem::setWorldCapacity3D(" + std::to_string(tagsAndLayers.maxBodies3D) + ", " +
                   std::to_string(tagsAndLayers.maxBodyPairs3D) + ", " + std::to_string(tagsAndLayers.maxContactConstraints3D) + ");\n";
    mainContent += "#endif\n";

    // Project Settings > Audio: the voices, then the mixer (its remembered values come back
    // before any sound plays)
    mainContent += audioInitSource(audioSettings);

    // Project Settings > Graphics > Shader Globals, declared with their defaults before any scene draws
    mainContent += shaderGlobalsInitSource(shaderGlobals);

    // Project Settings > Debug, before the first scene (its scripts may read DebugOverlay)
    mainContent += debugInitSource(debugSettings);

    mainContent += "\n";

    // Register all stacks with SceneManager
    for (const auto& sceneData : scenes) {
        std::string stackId = Factory::sceneToFileName(sceneData.id, sceneData.name);
        std::string sceneIds;
        for (size_t i = 0; i < sceneData.activeScenes.size(); i++) {
            if (i > 0) sceneIds += ", ";
            sceneIds += std::to_string(sceneData.activeScenes[i]);
        }
        mainContent += "    SceneManager::registerScene(" + std::to_string(sceneData.id) + ", \"" + sceneData.name + "\", load_" + stackId + ", add_" + stackId + ", {" + sceneIds + "}, resources_" + stackId + ");\n";
    }
    mainContent += "\n";

    // Register all prefabs with PrefabManager: ids by path order (collectAllPrefabs), the
    // ones the editor's Play registers, with their GUIDs (what script properties hold)
    for (size_t i = 0; i < prefabs.size(); i++) {
        fs::path noExt = prefabs[i].prefabPath;
        noExt.replace_extension();
        std::string prefabName = noExt.generic_string();
        mainContent += "    PrefabManager::registerPrefab(" + std::to_string(i + 1) + ", " + Factory::formatString(prefabName) + ", " +
            Factory::formatString(prefabs[i].guid) + ", " + prefabs[i].functionName + ");\n";
    }
    if (!prefabs.empty()) {
        mainContent += "\n";
    }

    // C++ scripts of the GameObjects scripts instantiate and destroy (scene_scripts.cpp)
    mainContent += "    ObjectManager::setCppScriptHooks(initEntityScripts, cleanupEntityScripts);\n";
    // ...and of those DontDestroyOnLoad / MoveGameObjectToScene move to another scene
    mainContent += "    ObjectManager::setCppScriptMoveHook(moveEntityScripts);\n\n";

    // Loading screen, after the scene it names is registered
    const LoadingSettings defaults;
    std::string loadingContent;
    if (loadingSettings.asyncLoading) {
        // web builds without pthreads load over several frames too (the job system's pump)
        loadingContent += "    Engine::setAsyncLoading(true);\n";
    }
    for (const auto& sceneData : scenes) {
        if (sceneData.id == loadingSettings.sceneId) {
            loadingContent += "    SceneManager::setLoadingScene(" + std::to_string(sceneData.id) + ");\n";
            break;
        }
    }
    if (loadingSettings.delay != defaults.delay) {
        loadingContent += "    SceneManager::setLoadingDelay(" + Factory::formatFloat(loadingSettings.delay) + ");\n";
    }
    if (loadingSettings.timeout != defaults.timeout) {
        loadingContent += "    SceneManager::setLoadingTimeout(" + Factory::formatFloat(loadingSettings.timeout) + ");\n";
    }
    if (!loadingContent.empty()) {
        mainContent += loadingContent + "\n";
    }

    for (const auto& scene : scenes) {
        if (scene.isMain) {
            mainContent += "    SceneManager::loadScene(\"" + scene.name + "\");\n";
            break; // Load the first main scene found, or the only scene if none are marked main
        }
    }
    mainContent += "}\n";

    const fs::path mainFile = generatedPath / "main.cpp";
    FileUtils::writeIfChanged(mainFile, mainContent);

    writeSourceFiles(projectPath, projectInternalPath, libName, scriptFiles, scenes, prefabs, vsyncEnabled, windowSettings, assetsPath, luaPath, scriptDirs, cxxStandard);
}

std::string editor::Generator::shaderGlobalsInitSource(const std::vector<ShaderGlobalDecl>& shaderGlobals) {
    if (shaderGlobals.empty()) return "";
    std::string source = "    ShaderGlobals::declare({\n";
    for (size_t i = 0; i < shaderGlobals.size(); i++) {
        source += "        " + Factory::formatShaderGlobalDecl(shaderGlobals[i]) + (i + 1 < shaderGlobals.size() ? ",\n" : "\n");
    }
    source += "    });\n";
    return source;
}

std::string editor::Generator::inputSettingsInitSource(const std::string& actionsAsset, float deadzone) {
    std::string source;
    if (deadzone != 0.15f)
        source += "    InputActions::setDefaultDeadzone(" + Factory::formatFloat(deadzone) + ");\n";
    // a project without an actions asset keeps the engine's Unity Input Manager defaults
    if (!actionsAsset.empty())
        source += "    InputActions::loadDefault(" + Factory::formatString(actionsAsset) + ");\n";
    return source;
}

std::string editor::Generator::debugInitSource(const DebugSettings& debug) {
    const DebugSettings defaults;
    std::string out;
    // (by value, not defined(): the engine's CMake defines it for its users as 0 or 1)
    out += "#if JASPERENGINE_DEVELOPMENT_BUILD || defined(DEBUG)\n";
    out += "    Engine::setDevelopmentBuild(true);\n";
    out += "#endif\n";
    if (debug.logBufferSize != defaults.logBufferSize)
        out += "    Log::setBufferSize(" + std::to_string(debug.logBufferSize) + ");\n";
    if (debug.logStackTraces != defaults.logStackTraces) {
        static const char* const traces[] = {"Off", "Errors", "Warnings", "All"};
        out += std::string("    Log::setStackTraces(LogStackTraces::") + traces[static_cast<int>(debug.logStackTraces)] + ");\n";
    }

    // "development" follows the build type at run time
    auto byMode = [](bool always, bool development) -> std::string {
        if (always) return "true";
        if (development) return "Engine::isDevelopmentBuild()";
        return "false";
    };
    const DebugOverlaySettings overlayDefaults;
    out += "    {\n";
    out += "        DebugOverlaySettings debugOverlay;\n";
    out += "        debugOverlay.enabled = " + byMode(debug.overlay == DebugSettings::Mode::AllBuilds, debug.overlay == DebugSettings::Mode::Development) + ";\n";
    if (debug.toggleKey != overlayDefaults.toggleKey)
        out += "        debugOverlay.toggleKey = " + Factory::formatString(debug.toggleKey) + ";\n";
    if (debug.statsKey != overlayDefaults.statsKey)
        out += "        debugOverlay.statsKey = " + Factory::formatString(debug.statsKey) + ";\n";
    if (debug.gesture != overlayDefaults.gesture) {
        static const char* const gestures[] = {"ThreeFingerHold", "FourFingerTap", "Off"};
        out += std::string("        debugOverlay.gesture = DebugOverlaySettings::Gesture::") + gestures[static_cast<int>(debug.gesture)] + ";\n";
    }
    if (debug.popup != overlayDefaults.popup) {
        static const char* const popups[] = {"Never", "TouchDevices", "AllDevices"};
        out += std::string("        debugOverlay.popup = DebugOverlaySettings::Popup::") + popups[static_cast<int>(debug.popup)] + ";\n";
    }
    if (debug.openOnError != overlayDefaults.openOnError)
        out += std::string("        debugOverlay.openOnError = ") + (debug.openOnError ? "true" : "false") + ";\n";
    if (debug.stats != overlayDefaults.stats) {
        static const char* const stats[] = {"Off", "Fps", "Full"};
        out += std::string("        debugOverlay.stats = DebugOverlaySettings::Stats::") + stats[static_cast<int>(debug.stats)] + ";\n";
    }
    if (debug.statsCorner != overlayDefaults.statsCorner) {
        static const char* const corners[] = {"TopLeft", "TopRight", "BottomLeft", "BottomRight"};
        out += std::string("        debugOverlay.statsCorner = DebugOverlaySettings::Corner::") + corners[static_cast<int>(debug.statsCorner)] + ";\n";
    }
    out += "        debugOverlay.allowLua = " + byMode(debug.allowLua == DebugSettings::AllowLua::AllBuilds, debug.allowLua == DebugSettings::AllowLua::Development) + ";\n";
    out += "        DebugOverlay::configure(debugOverlay);\n";
    out += "    }\n";
    return out;
}

std::string editor::Generator::audioInitSource(const AudioSettings& audioSettings) {
    std::string source;
    const AudioSettings defaults;
    const bool outputChanged = audioSettings.outputStage != defaults.outputStage || audioSettings.iosSessionCategory != defaults.iosSessionCategory;
    if (audioSettings.maxRealVoices != defaults.maxRealVoices || audioSettings.maxVoices != defaults.maxVoices ||
        audioSettings.sampleRate != defaults.sampleRate || outputChanged || audioSettings.seed != defaults.seed) {
        source += "    AudioSystem::configure({" + std::to_string(audioSettings.maxRealVoices) + ", " +
                  std::to_string(audioSettings.maxVoices) + ", " + std::to_string(audioSettings.sampleRate);
        // positional: a changed seed names the two fields before it
        const bool seedChanged = audioSettings.seed != defaults.seed;
        if (outputChanged || seedChanged) {
            source += std::string(", AudioOutputStage::") + (audioSettings.outputStage == "linear" ? "Linear" : "Roundoff");
            const std::string& session = audioSettings.iosSessionCategory;
            source += std::string(", IosAudioSession::") +
                      (session == "ambient" ? "Ambient" : session == "solo_ambient" ? "SoloAmbient" : session == "playback" ? "Playback" : "Default");
        }
        if (seedChanged) source += ", " + std::to_string(audioSettings.seed) + "u";
        source += "});\n";
    }
    if (!audioSettings.mixer.empty()) {
        source += "    AudioMixer::load(" + Factory::formatString(audioSettings.mixer) + ");\n";
    }
    if (audioSettings.musicGroup != defaults.musicGroup) {
        source += "    { AudioConfig audioConfig = AudioSystem::getConfig(); audioConfig.musicGroup = " + Factory::formatString(audioSettings.musicGroup) +
                  "; AudioSystem::configure(audioConfig); }\n";
    }
    return source;
}

std::string editor::Generator::resolveCMakePath(const std::string& userPath) {
    if (userPath.empty()) return "";

    std::error_code ec;
    fs::path candidate = fs::path(userPath);

#ifdef _WIN32
    const std::string exeName = "cmake.exe";
#else
    const std::string exeName = "cmake";
#endif

    if (fs::is_regular_file(candidate, ec)) {
        // CMake.app/Contents/MacOS/CMake is the GUI, which answers --version
        // like the real thing; the command line tool is next door in bin.
        if (candidate.parent_path().filename() == "MacOS") {
            const fs::path cli = candidate.parent_path().parent_path() / "bin" / exeName;
            if (fs::is_regular_file(cli, ec)) {
                return cli.string();
            }
        }

        // The dialog lists every file, and an installer or a disk image must not
        // become the cmake every later build runs.
        std::string name = candidate.filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return (name == "cmake" || name == "cmake.exe") ? candidate.string() : std::string();
    }

    if (fs::is_directory(candidate, ec)) {
        // A pick can land on the install root, its bin directory or the macOS
        // prefab, all of which the user thinks of as "CMake".
        const fs::path insideDir[] = {
            candidate / exeName,
            candidate / "bin" / exeName,
            candidate / "Contents" / "bin" / exeName,
            candidate / "CMake.app" / "Contents" / "bin" / exeName
        };
        for (const auto& path : insideDir) {
            if (fs::is_regular_file(path, ec)) {
                return path.string();
            }
        }
    }

    return "";
}

std::string editor::Generator::cmakeExecutable() {
    const std::string configured = AppSettings::getCMakePath();
    // Quoted: an install path may contain spaces ("Program Files", a bundle).
    return configured.empty() ? "cmake" : ("\"" + configured + "\"");
}

std::string editor::Generator::cmakeProgram() {
    // The program path unquoted, for an argv array (CommandRunner::run quotes nothing: argv[0] is
    // passed as-is / looked up on PATH).
    const std::string configured = AppSettings::getCMakePath();
    return configured.empty() ? "cmake" : configured;
}

bool editor::Generator::isKnownGenerator(const std::string& generator) {
    // CMake's generator names (cmake --help "Generators"); the base name of a Visual Studio entry
    // carries a version, so those are matched by prefix. A value outside this set is refused rather
    // than handed to CMake.
    static const char* const kExact[] = {
        "Ninja", "Ninja Multi-Config", "Unix Makefiles", "MinGW Makefiles", "MSYS Makefiles",
        "NMake Makefiles", "NMake Makefiles JOM", "Borland Makefiles", "Watcom WMake", "Xcode",
        "Green Hills MULTI",
    };
    for (const char* name : kExact) {
        if (generator == name) return true;
    }
    return generator.rfind("Visual Studio ", 0) == 0;
}

std::string editor::Generator::probeCMakeVersion(const std::string& path) {
    // run directly (no shell), killed after 5 s: a GUI program chosen by mistake never answers
    const std::string output = CommandRunner::capture({path, "--version"}, 5000);

    // "cmake version 3.31.6" on the first line.
    const std::string prefix = "cmake version ";
    const size_t at = output.find(prefix);
    if (at == std::string::npos) return "";

    size_t end = output.find_first_of("\r\n", at + prefix.size());
    if (end == std::string::npos) end = output.size();

    std::string version = output.substr(at + prefix.size(), end - (at + prefix.size()));
    while (!version.empty() && (version.back() == '\r' || version.back() == ' ')) {
        version.pop_back();
    }
    return version;
}

editor::CMakeInfo editor::Generator::detectCMake() {
    CMakeInfo info;

    const std::string configured = AppSettings::getCMakePath();
    if (!configured.empty()) {
        std::error_code ec;
        if (!fs::is_regular_file(configured, ec)) {
            info.error = "The configured CMake path no longer exists: " + configured;
            return info;
        }
        // A hand-picked path is trusted only once it answers --version, so it
        // cannot show as healthy here and then fail on Play.
        info.version = probeCMakeVersion(configured);
        if (info.version.empty()) {
            info.error = "The configured CMake path does not run as CMake: " + configured;
            return info;
        }
        info.found = true;
        info.path = configured;
        info.source = "configured path";
        return info;
    }

    // What PATH yields is an executable named cmake that the shell would run
    // too, so it stays usable even if the version cannot be read.
    const std::string onPath = ShellEnv::findExecutable("cmake");
    if (onPath.empty()) return info;

    info.found = true;
    info.path = onPath;
    info.source = "on PATH";
    info.version = probeCMakeVersion(onPath);

    return info;
}

std::vector<editor::CMakeKit> editor::Generator::detectAvailableKits() {
    std::vector<CMakeKit> kits;

    // Each probe runs its program DIRECTLY with an argument list (CommandRunner::capture: no shell,
    // so a compiler / tool path holding spaces or metacharacters is one literal argument); its
    // output when it exits 0, "" otherwise.
    auto runCmd = [](const std::vector<std::string>& argv) -> std::string {
        return CommandRunner::capture(argv);
    };

    auto findCompiler = [&runCmd](const std::string& compiler) -> std::string {
#ifdef _WIN32
        std::string result = runCmd({"where", compiler});
#else
        std::string result = runCmd({"which", compiler});
#endif
        size_t nl = result.find('\n');
        if (nl != std::string::npos) result = result.substr(0, nl);
        while (!result.empty() && (result.back() == '\r' || result.back() == ' '))
            result.pop_back();
        return result;
    };

#ifdef _WIN32
    // The editor and the engine library it links (jasperengine.lib/.dll) are built with a
    // single C++ toolchain. A plugin compiled for a different C++ ABI cannot link
    // against that library, and would crash even if it did: MSVC and MinGW/GNU
    // disagree on name mangling, class layout, RTTI, exception handling and STL
    // types. Flag kits whose ABI does not match this editor's build so they cannot
    // be selected (otherwise the mismatch surfaces only as a cryptic wall of
    // "undefined reference" at link time). This is a Windows-only concern; other
    // platforms have a single system C++ ABI.
    enum class CxxAbi { MSVC, GNU };
#if defined(__MINGW32__)
    const CxxAbi editorAbi = CxxAbi::GNU;
    const char* editorAbiName = "MinGW/GNU";
#else
    const CxxAbi editorAbi = CxxAbi::MSVC;
    const char* editorAbiName = "MSVC";
#endif
    auto enforceAbi = [&](CMakeKit& kit, CxxAbi kitAbi, const char* kitAbiName) {
        if (kitAbi != editorAbi) {
            kit.available = false;
            kit.unavailableReason = std::string("incompatible C++ ABI (kit is ") + kitAbiName
                + ", this editor is " + editorAbiName + "): build plugins with the editor's "
                "toolchain, or rebuild the editor with this compiler";
        }
    };
#endif

    // --- GCC ---
    {
        std::string cxxPath = findCompiler("g++");
        if (!cxxPath.empty()) {
            std::string ccPath = findCompiler("gcc");
            std::string version = runCmd({cxxPath, "-dumpfullversion", "-dumpversion"});
            std::string machine = runCmd({cxxPath, "-dumpmachine"});

            CMakeKit kit;
            kit.cxxCompiler = cxxPath;
            kit.cCompiler = ccPath;
            kit.displayName = "GCC " + version;
            if (!machine.empty()) kit.displayName += " " + machine;
#ifdef _WIN32
            kit.generator = "MinGW Makefiles";
            enforceAbi(kit, CxxAbi::GNU, "MinGW/GNU");
#endif
            kits.push_back(kit);
        }
    }

    // --- Clang ---
    {
        std::string cxxPath = findCompiler("clang++");
        if (!cxxPath.empty()) {
            std::string ccPath = findCompiler("clang");
            std::string versionOutput = runCmd({cxxPath, "--version"});
            std::string version;
            size_t nl = versionOutput.find('\n');
            std::string firstLine = (nl != std::string::npos) ? versionOutput.substr(0, nl) : versionOutput;
            size_t vpos = firstLine.find("version ");
            if (vpos != std::string::npos) {
                vpos += 8;
                size_t vend = firstLine.find_first_of(" (", vpos);
                version = (vend != std::string::npos) ? firstLine.substr(vpos, vend - vpos) : firstLine.substr(vpos);
            }
            std::string machine = runCmd({cxxPath, "-dumpmachine"});

            CMakeKit kit;
            kit.cxxCompiler = cxxPath;
            kit.cCompiler = ccPath;
            kit.displayName = "Clang " + version;
            if (!machine.empty()) kit.displayName += " " + machine;
#ifdef _WIN32
            // Determine generator from the target triple.
            // MinGW-targeting Clang uses MinGW Makefiles;
            // MSVC-targeting Clang requires Ninja: the Visual Studio
            // generator ignores CMAKE_C/CXX_COMPILER (MSBuild always drives
            // cl.exe), so without Ninja this kit has no usable generator.
            if (machine.find("mingw") != std::string::npos) {
                kit.generator = "MinGW Makefiles";
            } else {
                std::string ninjaPath = findCompiler("ninja");
                // Verify ninja actually executes: a broken binary (e.g. a pip
                // wrapper copied out of its Scripts folder, or a blocked
                // download) passes the PATH check but fails when CMake runs it.
                if (!ninjaPath.empty() && !runCmd({ninjaPath, "--version"}).empty()) {
                    kit.generator = "Ninja";
                } else if (ninjaPath.empty()) {
                    kit.available = false;
                    kit.unavailableReason = "requires Ninja on PATH (https://ninja-build.org)";
                } else {
                    kit.available = false;
                    kit.unavailableReason = "Ninja at '" + ninjaPath + "' failed to run; reinstall from https://ninja-build.org";
                }
            }
            // ABI follows the target triple: MinGW triples are GNU ABI; everything
            // else (e.g. x86_64-pc-windows-msvc) is MSVC ABI and can link an
            // MSVC-built engine. This is why Clang, unlike GCC, can target either.
            {
                CxxAbi clangAbi = (machine.find("mingw") != std::string::npos) ? CxxAbi::GNU : CxxAbi::MSVC;
                enforceAbi(kit, clangAbi, (clangAbi == CxxAbi::GNU) ? "MinGW/GNU" : "MSVC");
            }
#endif
            kits.push_back(kit);
        }
    }

#ifdef _WIN32
    // --- MSVC ---
    {
        std::string vswhere = CommandRunner::findVswherePath();
        std::string vsName;
        if (!vswhere.empty()) {
            vsName = runCmd({vswhere, "-products", "*", "-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "displayName"});
        }
        // cl.exe on PATH (e.g. a Developer Command Prompt) also signals MSVC is
        // available even if vswhere cannot be located.
        bool clOnPath = !findCompiler("cl").empty();
        if (!vsName.empty() || clOnPath) {
            CMakeKit kit;
            kit.displayName = vsName.empty() ? "MSVC" : vsName;
            // Pin a generator both the selected CMake and an installed VS
            // support, newest first: MSYS2 CMake would default to Ninja/GCC.
            const std::string cmakeHelp = runCmd({cmakeProgram(), "--help"});
            const std::regex vsGenerator("Visual Studio ([0-9]+) [0-9]{4}");
            std::map<int, std::string, std::greater<int>> generators;
            for (std::sregex_iterator it(cmakeHelp.begin(), cmakeHelp.end(), vsGenerator), end; it != end; ++it) {
                generators.emplace(std::stoi((*it)[1].str()), it->str());
            }
            for (const auto& entry : generators) {
                if (!vswhere.empty()) {
                    const std::string versionRange = "[" + std::to_string(entry.first) + "," + std::to_string(entry.first + 1) + ")";
                    const std::string name = runCmd({vswhere, "-products", "*", "-latest", "-version", versionRange, "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "displayName"});
                    if (name.empty()) continue;
                    kit.displayName = name;
                } else {
                    // A Developer Command Prompt can work without vswhere.
                    const char* version = std::getenv("VisualStudioVersion");
                    if (!version || std::string(version).substr(0, std::string(version).find('.')) != std::to_string(entry.first)) continue;
                }
                kit.generator = entry.second;
                break;
            }
            if (kit.generator.empty()) {
                // Also supports CMake distributions without VS generators.
                // vcvars supplies cl/SDK paths when cl is not already on PATH.
                const std::string ninjaPath = findCompiler("ninja");
                if (!ninjaPath.empty() && !runCmd({ninjaPath, "--version"}).empty() &&
                    (clOnPath || !CommandRunner::findVcvarsall().empty())) {
                    kit.generator = "Ninja";
                    kit.cCompiler = "cl";
                    kit.cxxCompiler = "cl";
                } else {
                    kit.available = false;
                    kit.unavailableReason = "requires a CMake version supporting the installed Visual Studio, or Ninja and an MSVC toolchain environment";
                }
            }
            enforceAbi(kit, CxxAbi::MSVC, "MSVC");
            kits.push_back(kit);
        }
    }
#endif

    return kits;
}

std::string editor::Generator::checkBuildTools(bool requireEditorCompatibility, CMakeKit* resolvedDefaultKit) {
    std::string missing;

    // No shell: Windows runs `where <name>` directly (CommandRunner::capture, the argument quoted
    // by its helper), elsewhere the PATH is searched in-process like `command -v` would.
    auto commandExists = [](const char* cmd) -> bool {
#ifdef _WIN32
        return !CommandRunner::capture({"where", cmd}).empty();
#else
        return !ShellEnv::findExecutable(cmd).empty();
#endif
    };

    const CMakeInfo cmakeInfo = detectCMake();
    if (!cmakeInfo.error.empty()) {
        missing += "- CMake: " + cmakeInfo.error + ". Choose it again in Preferences > External Tools.\n";
    } else if (!cmakeInfo.found) {
#ifdef _WIN32
        missing += "- CMake: not found. Download from https://cmake.org/download/ and ensure it is added to PATH during installation.\n";
#elif defined(__APPLE__)
        missing += "- CMake: not found. Install with: brew install cmake, or choose an existing install in Preferences > External Tools.\n";
#else
        missing += "- CMake: not found. Install with: sudo apt install cmake (Debian/Ubuntu) or sudo dnf install cmake (Fedora), or choose an existing install in Preferences > External Tools.\n";
#endif
    }

    bool hasCompiler = false;
#ifdef _WIN32
    if (requireEditorCompatibility) {
        const auto kits = detectAvailableKits();
        const CMakeKit* chosen = chooseDefaultKit(kits);
        hasCompiler = chosen != nullptr;
        if (chosen && resolvedDefaultKit && missing.empty()) {
            *resolvedDefaultKit = *chosen;
            Out::info("Default compiler resolved to: %s", chosen->displayName.c_str());
        }
        if (!hasCompiler) {
            missing += "- C++ toolchain compatible with this editor: not found. Select an available compiler in Preferences > External Tools.\n";
            for (const auto& kit : kits) {
                missing += "  " + kit.displayName + ": " + kit.unavailableReason + "\n";
            }
        }
    } else {
        hasCompiler = commandExists("cl") || commandExists("g++") || commandExists("clang++") || CommandRunner::hasVSWithCppTools();
    }
#elif defined(__APPLE__)
    hasCompiler = commandExists("clang++") || commandExists("g++");
#else
    hasCompiler = commandExists("g++") || commandExists("clang++");
#endif

    if (!hasCompiler) {
#ifdef _WIN32
        if (!requireEditorCompatibility) {
            missing += "- C++ compiler: not found. Install Visual Studio (https://visualstudio.microsoft.com/) and select the \"Desktop development with C++\" workload.\n";
        }
#elif defined(__APPLE__)
        missing += "- C++ compiler: not found. Install Xcode Command Line Tools by running: xcode-select --install\n";
#else
        missing += "- C++ compiler (g++ or clang++): not found. Install with: sudo apt install build-essential (Debian/Ubuntu) or sudo dnf install gcc-c++ (Fedora).\n";
#endif
    }

    (void)requireEditorCompatibility;
    (void)resolvedDefaultKit;
    return missing;
}

void editor::Generator::build(const fs::path projectPath, const fs::path buildPath, const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator, unsigned int parallelJobs) {
    cancelBuild();
    waitForBuildToComplete();

    lastBuildSucceeded.store(false, std::memory_order_relaxed);
    commandRunner.resetCancel();

    // the build runs in the write scope of the thread that started it (an MCP call's project);
    // the build tree is the editor's own, named on the thread that runs it (a token is per thread)
    const std::vector<fs::path> writeRoot = safewrite::Scope::currentRoots();
    const bool writeStrict = safewrite::Scope::currentStrict();
    if (jobs::isInChannelJob()) {
        // already on a blocking thread, which would otherwise wait on its own channel
        inlineBuilds.fetch_add(1, std::memory_order_acq_rel);
        {
            safewrite::Scope scope(writeRoot, writeStrict);
            safewrite::EditorInternal ownBuild(buildPath);
            runBuild(projectPath, buildPath, cCompiler, cxxCompiler, generator, parallelJobs);
        }
        inlineBuilds.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    jobs::SubmitOptions options;
    options.name = "cmake-build";
    buildTask = jobs::system().processChannel().submit([this, projectPath, buildPath, cCompiler, cxxCompiler, generator, parallelJobs, writeRoot, writeStrict]() {
        safewrite::Scope scope(writeRoot, writeStrict);
        safewrite::EditorInternal ownBuild(buildPath);
        runBuild(projectPath, buildPath, cCompiler, cxxCompiler, generator, parallelJobs);
    }, options);
}

void editor::Generator::runBuild(const fs::path& projectPath, const fs::path& buildPath, const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator, unsigned int parallelJobs) {
    // the build's state for the script status (S23 R17): started, then finished with its lines
    const uint64_t firstLog = Out::lastLogId();
    const auto eventStart = std::chrono::steady_clock::now();
    {
        ScriptBuildEvent started;
        started.kind = ScriptBuildEvent::Kind::Started;
        started.row = statusRow;
        started.projectPath = projectPath;
        buildEvents().invoke(started);
    }
    struct FinishedEvent {
        Generator& generator;
        fs::path projectPath;
        uint64_t firstLog;
        std::chrono::steady_clock::time_point start;
        ~FinishedEvent() {
            ScriptBuildEvent finished;
            finished.kind = ScriptBuildEvent::Kind::Finished;
            finished.row = generator.statusRow;
            finished.success = generator.didLastBuildSucceed();
            finished.cancelled = generator.commandRunner.isCancelRequested();
            finished.projectPath = projectPath;
            finished.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            for (const LogEntry& entry : Out::getLogsSince(firstLog)) {
                if (entry.type == LogType::Build || entry.type == LogType::Error || entry.type == LogType::Warning) finished.output.push_back(entry.message);
            }
            buildEvents().invoke(finished);
        }
    } finishedEvent{*this, projectPath, firstLog, eventStart};

    try {
        auto startTime = std::chrono::steady_clock::now();

        // The compiled plugin is loaded into the running editor and shares its
        // engine library, CRT and STL across the shared-library boundary, so it
        // must use the same build configuration. On Windows especially, a Debug
        // plugin (/MDd, _ITERATOR_DEBUG_LEVEL=2) loaded by a Release editor (/MD)
        // has incompatible std::vector/std::string layouts and crashes the moment
        // the editor calls into it. Match the editor's own configuration; _DEBUG
        // is defined iff this editor was built against the debug CRT. Elsewhere
        // NDEBUG decides, since Jolt derives JPH_DEBUG (and its layouts) from it.
        std::string configType = "Debug";
#if (defined(_WIN32) && !defined(_DEBUG)) || (!defined(_WIN32) && defined(NDEBUG))
        configType = "Release";
#endif

        // Resolve a "Default" selection to a concrete ABI-compatible kit once,
        // so configure and the build step (which both need the generator, e.g.
        // for the vcvars prefix) agree on the same toolchain.
        std::string cc = cCompiler, cxx = cxxCompiler, gen = generator;
        if (!resolveDefaultKit(cc, cxx, gen)) {
            return;
        }

        // Resolve automatic mode and the per-machine safety cap once. The
        // exact same value must drive both MSVC /MP and cmake --parallel.
        const unsigned int requestedOrAutomaticJobs = parallelJobs == 0
            ? getAutomaticParallelBuildJobs()
            : parallelJobs;
        const unsigned int effectiveParallelJobs =
            std::min(requestedOrAutomaticJobs, getMaxParallelBuildJobs());

        if (!configureCMake(projectPath, buildPath, configType, cc, cxx, gen, effectiveParallelJobs)) {
            if (commandRunner.isCancelRequested()) {
                Out::warning("Build configuration cancelled.");
            } else {
                Out::error("CMake configuration failed");
            }
            lastBuildSucceeded.store(false, std::memory_order_relaxed);
            return;
        }

        if (!buildProject(buildPath, configType, gen, effectiveParallelJobs)) {
            if (commandRunner.isCancelRequested()) {
                Out::warning("Build cancelled.");
            } else {
                Out::error("Build failed");
            }
            lastBuildSucceeded.store(false, std::memory_order_relaxed);
            return;
        }

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        double seconds = duration.count() / 1000.0;
        Out::build("Build completed successfully in %.2f seconds.", seconds);
        lastBuildSucceeded.store(true, std::memory_order_relaxed);
    } catch (const std::exception& ex) {
        Out::error("Build exception: %s", ex.what());
        lastBuildSucceeded.store(false, std::memory_order_relaxed);
    }
}

bool editor::Generator::isBuildInProgress() const {
    return (buildTask.isValid() && !buildTask.isReady()) || inlineBuilds.load(std::memory_order_acquire) > 0;
}

void editor::Generator::waitForBuildToComplete() {
    buildTask.wait();
    while (inlineBuilds.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool editor::Generator::didLastBuildSucceed() const {
    return lastBuildSucceeded.load(std::memory_order_relaxed);
}

void editor::Generator::cancelBuild() {
    if (!isBuildInProgress()) {
        commandRunner.resetCancel();
        return;
    }

    Out::warning("Cancelling build process...");
    // a build not started never runs; a running one has its process terminated
    buildTask.cancel();
    commandRunner.cancel();
    lastBuildSucceeded.store(false, std::memory_order_relaxed);
}
