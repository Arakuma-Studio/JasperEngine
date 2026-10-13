// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// HTML/CSS UI helpers of HtmlUiTools.cpp shared with PlayTools.cpp (send_input target /
// to_target). Main thread only.

#include "mcp/McpJson.h"

#include <string>

namespace jasperengine::editor::mcp {
    class McpContext;
}

namespace jasperengine::editor::mcp::htmlui_mcp {

    // The canvas point (CSS px = canvas px) at the center of the first visible element matching `selector`.
    // documentRef null: every UI Document of the running scenes (else the open scenes), highest sortOrder
    // first; the first document with a visible match wins. Fails with a message listing the documents searched.
    // "Visible" is not hidden nor zero-size; a transparent match (opacity 0, which the pointer still reaches, as
    // in a browser) is taken when no opaque one exists. Texture-mode documents are skipped (their elements are
    // not at canvas points); naming one is an error.
    // found (optional): {scene, entity: {id, name, path}, html, ref} of the document and element the point is in.
    bool elementCenter(McpContext& ctx, const Json& documentRef, const std::string& selector, double& x, double& y,
                       std::string& error, Json* found = nullptr);

}
