// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem hot reload and editor support (phase 1, p1-editor, spec §3.8.6-§3.8.7):
// HtmlUIReloadState (file stamps, runtime file log, messages), reloads with state
// preservation.

#include "UIDocumentSystem.h"

#include "Scene.h"

using namespace jasperengine;

Rml::ElementDocument* UIDocumentSystem::getDocument(Entity entity) const{
    // defined by the wave-0 commit: p1-lua and p1-anim reach documents through it before
    // p1-editor lands. p1-rtt keeps texture-mode documents in `documents` too, or extends this
    auto it = documents.find(entity);
    return (it != documents.end()) ? it->second : nullptr;
}

void UIDocumentSystem::reloadDocument(Entity entity, bool preserveState){
    // HTMLUI_P1_STUB: p1-editor (a plain reload: nothing is preserved until the full reload lands)
    (void)preserveState;
    UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(entity);
    if (!document)
        return;

    document->needReload = true;
    // without a view the next update loads it
    loadDocumentNow(entity);
}
