// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem texture-mode documents (phase 1, p1-rtt, spec §3.12): HtmlUITextureState
// (one context and target per document) and world-space pointer input.

#include "UIDocumentSystem.h"

using namespace jasperengine;

Framebuffer* UIDocumentSystem::getTargetTexture(Entity entity) const{
    // HTMLUI_P1_STUB: p1-rtt (no texture-mode documents yet)
    (void)entity;
    return nullptr;
}
