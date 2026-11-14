// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIFONTDIAGNOSTICS_H
#define HTMLUIFONTDIAGNOSTICS_H

// Engine-internal (RmlUi types): S93 R17, HTMLUI-FONT-0001 for text that the first available face of its font-family list
// cannot draw and a fallback face draws: an info when the text draws, a warning from the checks.

#include "Export.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"

#include <vector>

namespace jasperengine::htmlui::compat {
    struct CompatDocument;
}

namespace jasperengine {

    class JASPERENGINE_INTERNAL HtmlUIFontDiagnostics {
    public:
        // once per frame (UIDocumentSystem::update): what the contexts' text drew from fallback faces since the last call, as
        // a diagnostic on each document that draws it, once per (document, family, code point set)
        static void update();
        // check --ui and inspect_ui of a file (CompatApi::checkDocument), without RmlUi: the compile's RML and RCSS give each
        // text its font-family, weight and style (type, class, id, descendant and child selectors; states and attribute
        // selectors are not known statically), the document's @font-face files read through `files` give the faces, and the
        // runtime's chain decides; warnings, at the text's element
        static std::vector<htmlui::compat::Diagnostic> checkStatic(const htmlui::compat::CompatDocument& compiled, const htmlui::compat::FileAccess& files);
    };

}

#endif //HTMLUIFONTDIAGNOSTICS_H
