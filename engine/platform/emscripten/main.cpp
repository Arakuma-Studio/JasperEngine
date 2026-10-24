// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineWeb.h"

#include <emscripten.h>

#include <cstdlib>

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

int main(int argc, char **argv) {
    if (jasperengineReadySignalRequested())
        setenv("JASPERENGINE_READY_SIGNAL", "1", 1);
    return JasperEngineWeb::init(argc, argv);
}
