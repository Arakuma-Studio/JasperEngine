// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SEAM (phase-2 §4): the document half of CompatApi.h. p2-html owns this file and
// implements it over DocumentLoader; until then checkDocument reports HTMLUI-GEN-0004 and
// collectRemoteReferences handles stylesheets only.

#include "htmlui/compat/CompatApi.h"

using namespace jasperengine::htmlui::compat;

namespace {

    bool isStylesheetPath(const std::string& path){
        if (path.size() < 4)
            return false;
        std::string extension = path.substr(path.size() - 4);
        for (char& c : extension){
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        }
        return extension == ".css";
    }

}

CheckReport jasperengine::htmlui::compat::checkDocument(const std::string& htmlPath, const CheckOptions&){
    CheckReport report;
    Diagnostic diagnostic;
    diagnostic.code = codes::InternalError;
    diagnostic.severity = Severity::Error;
    diagnostic.message = "checkDocument is not available in this build (the HTML stage of the compatibility layer is missing)";
    diagnostic.file = htmlPath;
    diagnostic.subject = htmlPath;
    report.diagnostics.push_back(diagnostic);
    return report;
}

std::optional<Diagnostic> jasperengine::htmlui::compat::locateElement(const CompatDocument&, const Rml::Element*){
    return std::nullopt;
}

std::vector<RemoteReference> jasperengine::htmlui::compat::collectRemoteReferences(const std::string& htmlPath, const FileAccess& files){
    if (!isStylesheetPath(htmlPath))
        return {};
    CheckOptions options;
    options.verifyWithRmlUi = false;
    options.files = files;
    return checkStylesheet(htmlPath, options).remoteRefs;
}
