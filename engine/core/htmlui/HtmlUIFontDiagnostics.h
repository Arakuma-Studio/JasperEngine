// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIFONTDIAGNOSTICS_H
#define HTMLUIFONTDIAGNOSTICS_H

// Engine-internal (RmlUi types): S93 R17, HTMLUI-FONT-0001 (Info) for text that the first available face of its
// font-family list cannot draw and a fallback face draws.

#include "Export.h"
#include "htmlui/compat/Diagnostics.h"

#include <vector>

namespace Rml {
    class ElementDocument;
}

namespace jasperengine::htmlui::compat {
    struct CompatDocument;
}

namespace jasperengine {

    class JASPERENGINE_INTERNAL HtmlUIFontDiagnostics {
    public:
        // once per frame (UIDocumentSystem::update): what the contexts' text drew from fallback faces since the last call, as
        // a diagnostic on each document that draws it, once per (document, family, code point set)
        static void update();
        // the checks that lay a document out (CompatApi::checkDocument with verifyWithRmlUi): its static text, located in its
        // HTML source
        static std::vector<htmlui::compat::Diagnostic> check(Rml::ElementDocument* document, const htmlui::compat::CompatDocument& compiled);
    };

}

#endif //HTMLUIFONTDIAGNOSTICS_H
