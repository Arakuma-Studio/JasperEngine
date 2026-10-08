// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_COMPATAPI_H
#define COMPAT_COMPATAPI_H

// The compat layer's facade for phase 4 (check --ui, MCP, ui-support) and the editor
// (phase-2 §3.12.1). No RmlUi or lexbor type in the exported signatures (locateElement is
// engine-internal); with verifyWithRmlUi = false and files set, the checks need neither
// Engine::systemInit nor an RmlUi context.

#include "Export.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/Diagnostics.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
    class Element;
}

namespace jasperengine::htmlui::compat {

    struct CompatDocument;   // DocumentLoader.h (p2-html)

    struct CheckOptions {
        CompatOptions compat;
        // RmlUi parses the produced RCSS (and RML) and its warnings come back as
        // HTMLUI-GEN-0006 (must stay empty: a test invariant). Initialises RmlUi for the check
        // when nothing else has (HtmlUI::acquire/release).
        bool verifyWithRmlUi = true;
        FileAccess files;
    };

    struct CheckReport {
        std::vector<Diagnostic> diagnostics;      // sorted (file, line, column, code)
        std::vector<std::string> files;           // every file read, asset-relative
        std::vector<RemoteReference> remoteRefs;
    };

    // A whole document as it loads (HTML, its stylesheets, imports): the same diagnostics as
    // UIDocument::getDiagnostics() for the compile. htmlPath asset-relative.
    JASPERENGINE_API CheckReport checkDocument(const std::string& htmlPath, const CheckOptions& options);
    // One stylesheet on its own (an author sheet as a document links it).
    JASPERENGINE_API CheckReport checkStylesheet(const std::string& cssPath, const CheckOptions& options);

    // Source of an element of a loaded document (reads data-htmlui-loc): location fields only.
    std::optional<Diagnostic> locateElement(const CompatDocument& document, const Rml::Element* element);

    // Every remote URL an .html (or .css) references, as the compat pipeline collects them
    // (the editor importers' discovery, §3.13.3).
    JASPERENGINE_API std::vector<RemoteReference> collectRemoteReferences(const std::string& htmlPath);
    JASPERENGINE_API std::vector<RemoteReference> collectRemoteReferences(const std::string& htmlPath, const FileAccess& files);

    // The CSS name an author writes for an RCSS property compat emits ("decorator" ->
    // "background-image", "font-effect" -> "text-shadow", "-rmlui-direction" -> "direction");
    // the input when the names agree. Table-driven from PropertySupport::output (phase 4's
    // inspector).
    JASPERENGINE_API std::string cssPropertyName(std::string_view rcssProperty);

    // RmlUi's verdict on RCSS compat produced (verifyWithRmlUi): each warning or error RmlUi
    // logs while parsing `rcss` as a stylesheet named `virtualPath`, mapped through `lines`
    // into `files` (HTMLUI-GEN-0006; unmapped ones keep RmlUi's text and no location).
    // Needs RmlUi initialised; returns nothing otherwise.
    std::vector<Diagnostic> verifyRcssWithRmlUi(const std::string& rcss, const std::string& virtualPath,
                                                const RcssLineMap& lines, const SourceFiles& files);

}

#endif //COMPAT_COMPATAPI_H
