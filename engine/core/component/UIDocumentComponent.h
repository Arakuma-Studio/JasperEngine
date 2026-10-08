// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENT_COMPONENT_H
#define UIDOCUMENT_COMPONENT_H

#include "Engine.h"
#include <string>

namespace jasperengine{

    // SCREEN: drawn over the scene in its camera pass; TEXTURE: rendered into its own
    // target texture (UIDocument::getTargetTexture), shown on a mesh or an Image
    enum class UIDocumentRenderMode { SCREEN, TEXTURE };

    // An HTML/CSS document drawn over the scene (Unity's UIDocument). The document is
    // loaded into the scene's UIDocumentSystem; sortOrder orders the scene's documents.
    struct JASPERENGINE_API UIDocumentComponent{
        // phase 0
        std::string filename;     // asset-relative .html/.htm/.rml (asset path rules of FileData); MCP validates it
        int sortOrder = 0;        // higher draws on top (document z-index)
        bool enabled = true;      // shown and receiving pointer input (Inspector header checkbox, UIDocument::setVisible)

        // phase 1
        bool interactive = true;                                    // receives input
        UIDocumentRenderMode renderMode = UIDocumentRenderMode::SCREEN;
        int textureWidth = 1024;                                    // TEXTURE mode
        int textureHeight = 1024;
        Entity inputSurface = NULL_ENTITY;                          // TEXTURE mode: mesh hit-tested for pointer input; NULL = own entity
        float pixelRatio = 0.0f;                                    // 0 = automatic (phase-0 dp ratio); TEXTURE mode default 1
        FunctionSubscribe<void()> onLoad;                           // after every (re)load
        FunctionSubscribe<void()> onUnload;

        // runtime, not serialized (phase 0; phase-1 runtime state lives in UIDocumentSystem)
        bool needReload = true;   // filename changed or reload() requested
        bool loaded = false;      // the document exists in the context
        bool loadFailed = false;  // last load failed (error logged once per attempt)
        int appliedSortOrder = 0;
        bool appliedVisible = false;
    };

}

#endif //UIDOCUMENT_COMPONENT_H
