// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENT_COMPONENT_H
#define UIDOCUMENT_COMPONENT_H

#include "Engine.h"
#include <string>

namespace jasperengine{

    // An HTML/CSS document drawn over the scene (Unity's UIDocument). The document is
    // loaded into the scene's UIDocumentSystem; sortOrder orders the scene's documents.
    struct JASPERENGINE_API UIDocumentComponent{
        std::string filename;     // project-relative .html/.rml (asset path rules of FileData)
        int sortOrder = 0;        // higher draws on top (document z-index)
        bool enabled = true;      // shown and receiving pointer input

        // runtime, not serialized
        bool needReload = true;   // filename changed or reload() requested
        bool loaded = false;      // the document exists in the context
        bool loadFailed = false;  // last load failed (error logged once per attempt)
        int appliedSortOrder = 0;
        bool appliedVisible = false;
    };

}

#endif //UIDOCUMENT_COMPONENT_H
