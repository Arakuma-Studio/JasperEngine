// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineWeb.h"
#include "FileWatchService.h"
#include "../common/PlatformDefaults.h"
#include "io/ResourcePack.h"
#include "player/PlayerLaunch.h"

#include <emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// A page cannot set the environment Engine::signalReadyOnce reads, so the page URL stands in for
// it: ?jasperengine_ready_signal=1 (tools/web/browser_gate.py) logs the engine's ready line, as
// JASPERENGINE_READY_SIGNAL=1 does on desktop (tools/smoke-test.sh)
EM_JS(int, jasperengineReadySignalRequested, (), {
    try {
        return new URLSearchParams(globalThis.location ? globalThis.location.search : "").get("jasperengine_ready_signal") === "1" ? 1 : 0;
    } catch (e) {
        return 0;
    }
});

// ?jasperengine_watch_probe=1 (tools/web/browser_gate.py --expect, S05 AC23): logs what
// FileWatchService::start does in this build. Players never watch (R20): the web has no
// backend, so it must refuse every kind, polling included
EM_JS(int, jasperengineWatchProbeRequested, (), {
    try {
        return new URLSearchParams(globalThis.location ? globalThis.location.search : "").get("jasperengine_watch_probe") === "1" ? 1 : 0;
    } catch (e) {
        return 0;
    }
});

// The page's URL query as the launch options (?quit-after=30&fixed-fps=60, S30 R17)
EM_JS(char*, jasperengineLocationQuery, (), {
    const query = globalThis.location ? globalThis.location.search : "";
    const length = lengthBytesUTF8(query) + 1;
    const buffer = _malloc(length);
    stringToUTF8(query, buffer, length);
    return buffer;
});

// A prebuilt web player's content.jpak, which its page fetched before the player started
// (Module.jasperContentPack, an ArrayBuffer): its size, then its bytes into the heap, where the
// pack is read in place for the whole run (docs/specs/S30-shipping.md D6)
EM_JS(double, jasperengineContentPackSize, (), {
    const pack = Module["jasperContentPack"];
    return pack ? pack.byteLength : 0;
});

EM_JS(void, jasperengineTakeContentPack, (unsigned char* destination), {
    const pack = Module["jasperContentPack"];
    HEAPU8.set(new Uint8Array(pack), destination);
    Module["jasperContentPack"] = null;
});

// ... and its content-manifest.json (Module.jasperContentManifest, text), which the player reads
// from the content root: MEMFS's "/" on the web
EM_JS(char*, jasperengineTakeContentManifest, (), {
    const text = Module["jasperContentManifest"];
    if (typeof text !== "string") return 0;
    Module["jasperContentManifest"] = null;
    const length = lengthBytesUTF8(text) + 1;
    const buffer = _malloc(length);
    stringToUTF8(text, buffer, length);
    return buffer;
});

static void writeFetchedContentManifest() {
    char* text = jasperengineTakeContentManifest();
    if (!text)
        return;
    if (FILE* file = std::fopen("/content-manifest.json", "wb")) {
        std::fwrite(text, 1, std::strlen(text), file);
        std::fclose(file);
    } else {
        std::printf("content-manifest.json: cannot write it to the file system\n");
    }
    std::free(text);
}

static void mountFetchedContentPack() {
    const double size = jasperengineContentPackSize();
    if (size <= 0)
        return;
    static unsigned char* bytes = nullptr;
    bytes = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(size)));
    if (!bytes) {
        std::printf("content.jpak: out of memory for %.0f bytes\n", size);
        return;
    }
    jasperengineTakeContentPack(bytes);
    std::string error;
    if (!jasperengine::ResourcePack::mountContentPack(std::span<const uint8_t>(bytes, static_cast<size_t>(size)), &error))
        std::printf("content.jpak: %s\n", error.c_str());
}

static void probeFileWatcher() {
    const std::filesystem::path folder = "/jasperengine-watch-probe";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    jasperengine::FileWatchService& watcher = jasperengine::FileWatchService::get();
    const bool native = watcher.start(folder, jasperengine::WatchBackendKind::Native);
    const bool polling = watcher.start(folder, jasperengine::WatchBackendKind::Polling);
    const bool automatic = watcher.start(folder, jasperengine::WatchBackendKind::Auto);
    const bool attached = watcher.isAttached();
    watcher.stop();
    std::printf("File watcher probe: start(native)=%d start(polling)=%d start(auto)=%d attached=%d backend='%s'\n", native, polling,
                automatic, attached, watcher.stats().backend.c_str());
    std::fflush(stdout);
}

int main(int argc, char **argv) {
    if (jasperengineReadySignalRequested())
        setenv("JASPERENGINE_READY_SIGNAL", "1", 1);
    if (jasperengineWatchProbeRequested())
        probeFileWatcher();
    mountFetchedContentPack();
    writeFetchedContentManifest();
    // the build's settings, then the page's query (and a player's game.json) over them; a page's
    // URL carries queries of its own, so unknown items are not errors
    jasperengine::applyPlatformDefaults();
    char* query = jasperengineLocationQuery();
    const std::vector<std::string> args = jasperengine::player::argsFromQuery(query ? query : "");
    std::free(query);
    const int launched = jasperengine::player::launch(args, jasperengine::player::ParseMode::Lenient);
    if (launched >= 0) {
        std::printf("JasperEngine exit code %d\n", launched);
        std::fflush(stdout);
        return launched;
    }
    return JasperEngineWeb::init(argc, argv);
}
