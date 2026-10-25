// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Finds ImGui items by label, for tests that drive the editor with real mouse and key input
// (MCP locate_ui_item): they click where an item is drawn now instead of at positions measured
// once, so a layout change does not break them. ImGui reports every item it draws through its
// test-engine hooks (IMGUI_ENABLE_TEST_ENGINE); they are on only while a lookup records.

#ifndef UIITEMLOCATOR_H
#define UIITEMLOCATOR_H

#include <string>
#include <vector>

namespace jasperengine::editor::UiItemLocator {

    struct Item {
        std::string label;   // as drawn, "##" suffix included
        std::string window;  // the ImGui window it is drawn in
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;   // main viewport coordinates
        bool visible = false;   // inside its window's clip rect (an item scrolled away is not drawn)
        // the window it is drawn in, main viewport coordinates (to scroll it with the wheel)
        float windowX = 0.0f, windowY = 0.0f, windowWidth = 0.0f, windowHeight = 0.0f;
    };

    // Starts recording the items drawn with `label` (the whole label, or the text before
    // "##") in windows whose name contains `window` (empty: any window).
    void start(const std::string& label, const std::string& window);
    // Stops recording: each item recorded, as drawn last.
    std::vector<Item> stop();

}

#endif // UIITEMLOCATOR_H
