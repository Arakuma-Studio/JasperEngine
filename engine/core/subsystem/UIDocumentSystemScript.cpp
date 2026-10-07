// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem scripting (phase 1, p1-lua, spec §3.9): HtmlUIScriptState (listeners,
// detached pool, data models) and the Lua access of phase 4.

#include "UIDocumentSystem.h"

using namespace jasperengine;

void UIDocumentSystem::scriptDocumentReloaded(Entity entity, const std::unordered_map<Rml::Element*, Rml::Element*>& oldToNew){
    // HTMLUI_P1_STUB: p1-lua (moves the listeners of the old document's id-ed elements to the new ones)
    (void)entity;
    (void)oldToNew;
}
