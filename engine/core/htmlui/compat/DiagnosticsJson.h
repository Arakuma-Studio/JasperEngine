// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_DIAGNOSTICS_JSON_H
#define COMPAT_DIAGNOSTICS_JSON_H

// The JSON form of diagnostics (phase 4's check --ui and MCP findings). Apart from
// Diagnostics.h, which is part of the engine API that the C++ scripts of a project compile
// against (UIDocument.h includes it): nlohmann's header is not in that API.

#include "Diagnostics.h"

#include "json.hpp"

#include <vector>

namespace jasperengine::htmlui::compat {

    // {severity, code, name, message, suggestion, replacement, subject, file, line, column,
    //  length, origin}; severity "error"/"warning"/"info", code "HTMLUI-CSS-0006", name the
    // registry's kebab-case name
    JASPERENGINE_API nlohmann::json toJson(const Diagnostic& diagnostic);
    JASPERENGINE_API nlohmann::json toJson(const std::vector<Diagnostic>& diagnostics);

}

#endif //COMPAT_DIAGNOSTICS_JSON_H
