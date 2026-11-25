// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The prebuilt player (docs/specs/S30-shipping.md R1, D3): one executable per platform, backend
// and flavour that runs any game's cooked content. Before the window exists it finds its content
// (next to the executable, the bundle's resources, --content), checks content-manifest.json
// (exit 5 on a mismatch) and reads game.json into the platform configuration; its init() then
// runs the game (GameRunner.h), as a Compile build's generated init() does.

#include "JasperEngine.h"

#include "PlatformConfig.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "io/ResourcePack.h"
#include "io/StrippedAssets.h"
#include "player/ContentManifest.h"
#include "player/GameRunner.h"
#include "player/PlayerLaunch.h"
#include "player/PlayerConfig.h"
#include "player/RunControl.h"
#include "util/Sha256.h"

#include <cstdio>
#include <filesystem>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#endif

using namespace jasperengine;
using namespace jasperengine::player;

namespace {

    PlayerConfig game;

    // where a player looks for its content when --content does not say (R2)
    std::string defaultContentRoot() {
#if defined(__ANDROID__)
        // the APK's assets/game/ (the APK's assets root keeps no other tree)
        return "game";
#elif defined(__EMSCRIPTEN__)
        // the fetched content.jpak, mounted before main
        return "";
#elif defined(__APPLE__)
        std::string root;
        if (CFBundleRef bundle = CFBundleGetMainBundle()) {
            if (CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle)) {
                if (CFURLRef absolute = CFURLCopyAbsoluteURL(url)) {
                    char path[4096];
                    if (CFURLGetFileSystemRepresentation(absolute, true, reinterpret_cast<UInt8*>(path), sizeof(path))) root = path;
                    CFRelease(absolute);
                }
                CFRelease(url);
            }
        }
    #if TARGET_OS_IPHONE
        // the iOS template stages the content into Content/ of the app
        return root.empty() ? "Content" : root + "/Content";
    #else
        // a .app keeps it in Contents/Resources; a bare executable next to itself
        if (!root.empty() && std::filesystem::exists(std::filesystem::path(root) / "game.json")) return root;
        char executable[4096];
        uint32_t size = sizeof(executable);
        if (_NSGetExecutablePath(executable, &size) == 0)
            return std::filesystem::weakly_canonical(executable).parent_path().string();
        return root.empty() ? "." : root;
    #endif
#elif defined(_WIN32)
        wchar_t path[32768];
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path)) return ".";
        // (UTF-8 bytes: C++20's u8string is std::u8string)
        const std::u8string utf8 = std::filesystem::path(std::wstring(path, length)).parent_path().u8string();
        return std::string(utf8.begin(), utf8.end());
#else
        char path[4096];
        const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (length <= 0) return ".";
        path[length] = '\0';
        return std::filesystem::path(path).parent_path().string();
#endif
    }

    bool readContent(const std::string& relative, std::string& out) {
        Data data;
        if (data.open(("content://" + relative).c_str()) != FileErrors::FILEDATA_OK) return false;
        out.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        return true;
    }

    bool contentExists(const std::string& relative) {
        if (ResourcePack::contains("content://" + relative)) return true;
        // the web's pack was fetched by the page and mounted from memory
        if (relative == "content.jpak" && ResourcePack::contentPack() && platformConfig().contentRoot.empty()) return true;
        FILE* file = System::hasInstance() ? System::instance().platformFopen(FileData::getSystemPath("content://" + relative).c_str(), "rb")
                                           : std::fopen((platformConfig().contentRoot + "/" + relative).c_str(), "rb");
        if (!file) return false;
        std::fclose(file);
        return true;
    }

    bool contentSha256(const std::string& relative, std::string& hex, uint64_t& size) {
        std::string bytes;
        if (!readContent(relative, bytes)) return false;
        hex = sha256Hex(bytes);
        size = bytes.size();
        return true;
    }

    // a system is needed for content:// before the platform made its own: the paths of the
    // content root, the same the platform's will report
    class ContentSystem : public System {
    public:
        int getScreenWidth() override { return platformConfig().width; }
        int getScreenHeight() override { return platformConfig().height; }
        std::string getContentPath() override { return platformConfig().contentRoot; }
        std::string getAssetPath() override {
            return platformConfig().contentRoot.empty() ? std::string() : platformConfig().contentRoot + "/assets";
        }
    };

    // what a player tells before it gives up: stderr, and logcat on Android (no stderr there)
    void report(const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, "JasperEngine", "%s", message.c_str());
#endif
    }

    int failContent(const std::string& message) {
        report("Content check failed: " + message);
        return EXIT_CONTENT_MISMATCH;
    }

    // the player's step before the window (PlayerLaunch.h)
    int configure(const LaunchOptions& options) {
        PlatformConfig& platform = platformConfig();
        if (platform.contentRoot.empty()) platform.contentRoot = defaultContentRoot();

        // the platform's System comes with its window; until then the content root's
        ContentSystem early;
        EarlySystemScope earlyScope(&early);

        // a packed build's content.jpak (the web mounted its fetched one already)
        if (!ResourcePack::contentPack()) {
            const std::filesystem::path pack = std::filesystem::path(platform.contentRoot) / "content.jpak";
            std::error_code ec;
            if (std::filesystem::is_regular_file(pack, ec)) {
                std::string error;
                if (!ResourcePack::mountContentPack(pack, &error)) return failContent("content.jpak: " + error);
            }
        }

        std::string text;
        if (!readContent("content-manifest.json", text))
            return failContent("no content-manifest.json in " + (platform.contentRoot.empty() ? std::string("the content pack") : platform.contentRoot) +
                               ": run a game built for this player (Build Settings > Build, or jasperengine-editor build), or give "
                               "--content <folder of the build>");
        ContentManifest manifest;
        std::string error;
        if (!ContentManifest::parse(text, manifest, error)) return failContent(error);
        std::vector<std::string> problems;
        const ContentAccess access{&contentExists, &contentSha256};
        if (!checkContent(manifest, runtimePlayerId(), access, options.verifyContent, problems)) {
            for (const std::string& problem : problems) report("Content check failed: " + problem);
            return EXIT_CONTENT_MISMATCH;
        }

        // a Development Build names the setting at a load of an asset its build left out
        StrippedAssets::set(manifest.stripped);

        if (!readContent("game.json", text)) return failContent("game.json is missing");
        if (!PlayerConfig::parse(text, game, error)) return failContent(error);
        if (!options.scene.empty() && !game.findScene(options.scene)) {
            std::fprintf(stderr, "--scene %s: no scene of that name or id; the game has", options.scene.c_str());
            for (const GameScene& scene : game.scenes) std::fprintf(stderr, " \"%s\" (%u)", scene.name.c_str(), scene.id);
            std::fprintf(stderr, "\n");
            return EXIT_BAD_ARGUMENTS;
        }

        platform.windowMode = game.windowMode;
        platform.width = game.windowWidth;
        platform.height = game.windowHeight;
        platform.resizable = game.resizable;
        platform.vsync = game.vsync;
        platform.title = game.title;
        platform.appId = game.appId.empty() ? game.name : game.appId;
        platform.storageId = game.storageId;
        if (!game.icon.empty() && contentExists(game.icon) && !platform.contentRoot.empty())
            platform.iconPath = platform.contentRoot + "/" + game.icon;
        platform.logLevel = game.logLevel;
        platform.logFile = game.logFile ? 1 : 0;
        platform.logBacktrace = game.logBacktrace ? 1 : 0;
        platform.crashHandler = game.crashHandler ? 1 : 0;
        platform.crashLogTail = game.crashLogTail ? 1 : 0;
        platform.crashDumpWithHeap = game.crashDumpWithHeap ? 1 : 0;
        platform.hangWatchdog = game.hangWatchdog ? 1 : 0;
        platform.hangMs = game.hangMs;
        platform.crashWebOverlay = game.crashWebOverlay ? 1 : 0;
        return -1;
    }

    struct ConfiguratorRegistrar {
        ConfiguratorRegistrar() { setGameConfigurator(&configure); }
    } configuratorRegistrar;

}

JASPERENGINE_INIT void init() {
    runGame(game);
}
