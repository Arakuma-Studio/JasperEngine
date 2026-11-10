// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace jasperengine::editor {

    // Test-only widget locator. With JASPER_UI_PROBE=<file> in the editor's environment (the MCP
    // test harness sets it for Editor(ui_probe=True)), the widgets the editor names here are
    // written to that file as JSON, {"frame": n, "items": {name: [x0, y0, x1, y1]}, "scroll":
    // {name: [y, maxY]}, "begins": {name: n}, "counts": {name: n}}, in pixels of the editor window (what xdotool and an
    // editor screenshot use), so a UI test can click a widget wherever the layout puts it instead
    // of at a fixed position. Only items drawn whole inside their window's visible area in the last
    // frame are listed. "scroll" holds the vertical scroll of the windows that name it, so a test
    // scrolling one with the mouse wheel sees each notch applied and knows when it can go no further.
    // "begins" lists the top-level windows begun more than once a frame: a panel shown twice
    // appends to itself, and nothing on screen says so. "counts" holds running totals the editor
    // adds to (the selected Scene view's redraws), so a test reads what happened over the frames
    // it waited instead of timing them.
    // Without the variable every call returns at once (one getenv, cached).
    class UIProbe {
    public:
        static bool enabled();
        // the last ImGui item (ImGui::GetItemRectMin/Max), when it is fully visible
        static void item(const std::string& name);
        // a rectangle of the current ImGui window, in screen pixels, when it is fully visible
        static void rect(const std::string& name, float x0, float y0, float x1, float y1);
        // the current ImGui window's vertical scroll (ImGuiWindow::Scroll.y and ScrollMax.y)
        static void scroll(const std::string& name);
        // adds one to the running total `name` (never reset while the editor runs)
        static void count(const std::string& name);
        // once a frame: what the last frame named, written when it changed
        static void flush();
    };

}
