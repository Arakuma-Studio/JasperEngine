// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem rendering (phase 1, p1-render, spec §3.5): HtmlUIRenderState, the
// camera-aware composite, render statistics and the MSAA sample count.

#include "UIDocumentSystem.h"

using namespace jasperengine;

namespace {

    // HTMLUI_P1_STUB: p1-render (forwards to HtmlUIContext::setPreferredSampleCount)
    int preferredSampleCount = 4;

}

void UIDocumentSystem::setPreferredSampleCount(int samples){
    // HTMLUI_P1_STUB: p1-render
    preferredSampleCount = samples;
}

int UIDocumentSystem::getPreferredSampleCount(){
    // HTMLUI_P1_STUB: p1-render
    return preferredSampleCount;
}
