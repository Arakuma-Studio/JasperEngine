// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include "imgui.h"
#include "Catalog.h"

namespace jasperengine {
namespace editor {

struct ComponentEntry {
    ComponentType type;
    const char* description;
};

struct ComponentCategory {
    const char* name;
    const char* icon;
    std::vector<ComponentEntry> entries;
};

// Unity's Add Component popup: a search field, then categories that open in place
// (a back arrow in the header returns), and a "New Script" entry at the bottom
class ComponentAddDialog {
private:
    bool m_isOpen = false;
    bool m_justOpened = false;
    bool m_focusSearch = false;
    char m_searchBuffer[128] = "";
    int m_category = -1;   // open category, -1 at the root
    int m_highlighted = 0; // keyboard row
    bool m_scrollToHighlighted = false;

    ImVec2 m_anchorMin = ImVec2(0, 0); // rect of the button that opened the popup
    ImVec2 m_anchorMax = ImVec2(0, 0);

    std::vector<ComponentCategory> m_categories;
    std::function<void(ComponentType)> m_onAdd;
    std::function<void()> m_onCancel;
    std::function<bool(ComponentType)> m_canAddComponent;
    std::function<void()> m_onNewScript;

    void initializeCategories();
    void finish(bool cancelled);

public:
    ComponentAddDialog();

    // anchorMin/anchorMax: screen rect of the Add Component button, the popup opens below it
    void open(
        std::function<void(ComponentType)> onAdd,
        std::function<bool(ComponentType)> canAddComponent,
        std::function<void()> onCancel = nullptr,
        std::function<void()> onNewScript = nullptr,
        ImVec2 anchorMin = ImVec2(0, 0),
        ImVec2 anchorMax = ImVec2(0, 0)
    );

    void show();
    bool isOpen() const { return m_isOpen; }

    // The popup offers this component type: what Add Component, and so Paste Component As New, can add
    static bool isListed(ComponentType type);
    void close() { m_isOpen = false; }
};

} // namespace editor
} // namespace jasperengine
