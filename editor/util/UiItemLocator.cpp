// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UiItemLocator.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstring>
#include <map>

using namespace jasperengine::editor;

namespace {

    struct LocatorState {
        bool recording = false;
        std::string label;
        std::string window;
        std::map<ImGuiID, UiItemLocator::Item> items;   // by item id: an item drawn again replaces itself
    };

    LocatorState& state() {
        static LocatorState locator;
        return locator;
    }

    bool labelMatches(const std::string& wanted, const char* label) {
        if (wanted == label) return true;
        const char* hidden = std::strstr(label, "##");
        return hidden && hidden != label && wanted.compare(0, std::string::npos, label, (size_t)(hidden - label)) == 0;
    }

    bool windowMatches(const std::string& wanted, const ImGuiWindow* window) {
        if (wanted.empty()) return true;
        for (const ImGuiWindow* w = window; w; w = w->ParentWindow) {
            if (std::strstr(w->Name, wanted.c_str())) return true;
        }
        return false;
    }

}

void UiItemLocator::start(const std::string& label, const std::string& window) {
    LocatorState& locator = state();
    locator.recording = true;
    locator.label = label;
    locator.window = window;
    locator.items.clear();
    if (ImGuiContext* context = ImGui::GetCurrentContext()) context->TestEngineHookItems = true;
}

std::vector<UiItemLocator::Item> UiItemLocator::stop() {
    LocatorState& locator = state();
    locator.recording = false;
    if (ImGuiContext* context = ImGui::GetCurrentContext()) context->TestEngineHookItems = false;
    std::vector<Item> items;
    for (auto& entry : locator.items) items.push_back(std::move(entry.second));
    locator.items.clear();
    return items;
}

namespace {

    void record(ImGuiWindow* window, ImGuiID id, const char* label, const ImRect& rect) {
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        UiItemLocator::Item item;
        item.label = label;
        item.window = window->Name;
        item.x = rect.Min.x - origin.x;
        item.y = rect.Min.y - origin.y;
        item.width = rect.GetWidth();
        item.height = rect.GetHeight();
        item.visible = !window->Hidden && window->ClipRect.Contains(rect.GetCenter());
        item.windowX = window->Pos.x - origin.x;
        item.windowY = window->Pos.y - origin.y;
        item.windowWidth = window->Size.x;
        item.windowHeight = window->Size.y;
        state().items[id] = std::move(item);
    }

}

// ImGui's test-engine hooks: ImGui calls them only while TestEngineHookItems is set (a lookup
// records). ItemAdd registers every item, by id: the label's id in the current window (its ID
// stack, as the widget hashed it) finds the widgets that report no label, combos among them.
// ItemInfo adds the label of the item ItemAdd registered last, for a match on its visible text.

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* context, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* itemData) {
    LocatorState& locator = state();
    ImGuiWindow* window = context->CurrentWindow;
    if (!locator.recording || !window || id == 0 || !windowMatches(locator.window, window)) return;
    if (id != window->GetID(locator.label.c_str())) return;
    record(window, id, locator.label.c_str(), itemData ? itemData->Rect : bb);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* context, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    LocatorState& locator = state();
    ImGuiWindow* window = context->CurrentWindow;
    if (!locator.recording || !label || !window || context->LastItemData.ID != id) return;
    if (!labelMatches(locator.label, label) || !windowMatches(locator.window, window)) return;
    record(window, id, label, context->LastItemData.Rect);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) {
    return nullptr;
}
