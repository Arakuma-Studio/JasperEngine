// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace jasperengine::editor {

    // Test-only widget locator. With JASPER_UI_PROBE=<file> in the editor's environment (the MCP
    // test harness sets it for Editor(ui_probe=True)), the widgets the editor names here are
    // written to that file as JSON, {"frame": n, "items": {name: [x0, y0, x1, y1]}}, in pixels of
    // the editor window (what xdotool and an editor screenshot use), so a UI test can click a
    // widget wherever the layout puts it instead of at a fixed position. Only items drawn whole
    // inside their window's visible area in the last frame are listed. Without the variable
    // every call returns at once (one getenv, cached).
    class UIProbe {
    public:
        static bool enabled();
        // the last ImGui item (ImGui::GetItemRectMin/Max), when it is fully visible
        static void item(const std::string& name);
        // a rectangle of the current ImGui window, in screen pixels, when it is fully visible
        static void rect(const std::string& name, float x0, float y0, float x1, float y1);
        // once a frame: what the last frame named, written when it changed
        static void flush();
    };

}
