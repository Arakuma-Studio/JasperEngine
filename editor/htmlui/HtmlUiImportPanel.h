// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Entity.h"

#include <string>

namespace jasperengine {
    class UIDocumentSystem;
}

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::htmlui {

    // The Inspector's "Remote resources" section of a UI Document (phase-2 §3.16 H10):
    // "Remote resources: 4 imported, 1 missing [Import] [Re-import all] [Remove unused]", the
    // per-URL status (imported, queued, importing, failed with the reason, file missing), and the
    // document's compat diagnostics count with "Show in Console".
    class HtmlUiImportPanel {
    public:
        static void draw(Project* project, SceneProject* sceneProject, Entity entity, const std::string& documentPath,
                         UIDocumentSystem* documents);
    };

}
