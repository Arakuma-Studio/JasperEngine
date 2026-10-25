// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineWeb.h"
#include "FileWatchService.h"

#include <emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

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
    return JasperEngineWeb::init(argc, argv);
}
