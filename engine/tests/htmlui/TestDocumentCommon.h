// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers of the UIDocumentComponent / UIDocumentSystem tests. They drive the document
// system's update() only: loading, layout, z-order and input need no rendering.

#ifndef HTMLUI_TEST_DOCUMENT_COMMON_H
#define HTMLUI_TEST_DOCUMENT_COMMON_H

#include "harness/GpuEnvironment.h"
#include "harness/SceneFixture.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "HtmlUIContext.h"
#include "Scene.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <string>

namespace jasperengine { namespace test {

    inline UIDocumentSystem& documentSystem(Scene& scene){
        return *scene.getSystem<UIDocumentSystem>();
    }

    inline void updateDocuments(Scene& scene){
        documentSystem(scene).update(0);
    }

    inline Rml::Context* rmlContext(Scene& scene){
        HtmlUIContext* context = documentSystem(scene).getContext();
        return context ? context->getRmlContext() : nullptr;
    }

    // documents still attached to the context (an unloaded one leaves at once)
    inline int documentCount(Scene& scene){
        Rml::Context* context = rmlContext(scene);
        return context ? context->GetNumDocuments() : 0;
    }

    // a loaded document by its <title>
    inline Rml::ElementDocument* findDocument(Scene& scene, const std::string& title){
        Rml::Context* context = rmlContext(scene);
        if (!context)
            return nullptr;
        for (int i = 0; i < context->GetNumDocuments(); i++){
            Rml::ElementDocument* document = context->GetDocument(i);
            if (document && document->GetTitle() == title)
                return document;
        }
        return nullptr;
    }

} }

#endif //HTMLUI_TEST_DOCUMENT_COMMON_H
