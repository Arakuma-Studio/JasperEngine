// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleDesignerHooks.h"

#include "titleart/runtime/TitleArtRuntimeDesigner.h"
#include "titleart/three_d/Title3DDesigner.h"

void jasperengine::editor::titledesigner::registerTitleDesignerExtensions() {
    // each registers once; a spec left out of the build has no line here and no UI
    registerTitleArtRuntimeDesigner();
    registerTitle3DDesigner();
}
