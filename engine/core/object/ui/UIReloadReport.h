// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIRELOADREPORT_H
#define UIRELOADREPORT_H

#include <string>
#include <vector>

namespace jasperengine{

    // What a hot reload of an HTML document did (S17 R31). A patch reconciles the live document
    // with the new compile: elements kept as they were, updated (an attribute, class, inline style
    // or text the author changed), inserted, removed and replaced (a changed element type or data
    // binding), the nodes scripts added that stayed, the document scripts that ran again. A
    // rebuild loads the document again (onUnload, onLoad); fallbackReason says why a patch was not
    // possible ("" when it was, or when a rebuild was asked for).
    struct UIReloadReport {
        std::string mode;                  // "patch" or "rebuild"
        std::string file;                  // the document
        int kept = 0;
        int updated = 0;
        int inserted = 0;
        int removed = 0;
        int replaced = 0;
        int runtimeKept = 0;
        int scriptsRerun = 0;
        std::string fallbackReason;
        double ms = 0.0;                   // the reload, the compile excluded
        std::vector<std::string> causes;   // each replaced element and why
    };

    // how hot reloads of HTML documents run (Preferences > HTML UI > Hot reload); .rml documents
    // always rebuild
    enum class UIReloadMode { PATCH, REBUILD };

}

#endif //UIRELOADREPORT_H
