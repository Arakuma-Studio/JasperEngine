// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// JASPER_HTMLUI_S16_OFF=1 (docs/specs/S16-ui-render-text.md D1, AC1): the suite runs with the
// batched replay, SDF primitives and transform-aware text off, the renderer as it was before S16:
// every golden must pass unchanged. Set before main(), so every test sees it; the tests of S16's
// own features that turn a toggle on for themselves still do.

#include "RmlFontEngine.h"
#include "RmlRenderer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

    // distance glyphs are made when asked for in this binary: what a golden shows does not depend on
    // the background lane's timing
    [[maybe_unused]] const bool synchronousDistanceGlyphs = [](){
        jasperengine::RmlFontEngine::setSynchronousDistanceGlyphsForTesting(true);
        return true;
    }();

    [[maybe_unused]] const bool s16Off = [](){
        const char* value = std::getenv("JASPER_HTMLUI_S16_OFF");
        if (!value || std::strcmp(value, "1") != 0)
            return false;
        jasperengine::RmlRenderer::setBatchingForTesting(false);
        jasperengine::RmlRenderer::setSdfPrimitivesForTesting(false);
        jasperengine::RmlFontEngine::setTransformAwareTextForTesting(false);
        std::printf("[ S16      ] JASPER_HTMLUI_S16_OFF=1: batching, SDF primitives and transform-aware text are off\n");
        return true;
    }();

}
