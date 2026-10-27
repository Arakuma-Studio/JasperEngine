// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Unity-style settings layout shared by Preferences and Project Settings: a
// searchable page list on the left, a bold page title over the page.

#include "Theme.h"
#include "imgui.h"
#include "search/SearchScorer.h"
#include "settings/SettingsRegistry.h"
#include "settings/SettingsSearch.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace jasperengine::editor::settingsui {

    // Text drawn twice, one pixel apart: the editor ships no bold face.
    inline void boldText(const char* text, float scale = 1.0f) {
        const float baseSize = ImGui::GetStyle().FontSizeBase;
        if (scale != 1.0f) ImGui::PushFont(nullptr, baseSize * scale);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 1.0f, pos.y), ImGui::GetColorU32(ImGuiCol_Text), text);
        ImGui::TextUnformatted(text);
        if (scale != 1.0f) ImGui::PopFont();
    }

    // Bold page title with a rule under it, as at the top of each Unity settings page.
    inline void categoryTitle(const char* title) {
        boldText(title, 1.3f);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // Search field over the page list (the settings registry's pages of a scope, S25 R29-R30):
    // the search finds pages by their rows (label, keywords, key), and the page draws only the
    // rows that match. Keeps `selected` (an index into the scope's pages) on a visible page
    // while a search is active.
    inline void pageList(const char* id, SettingsScope scope, int& selected, char* search, size_t searchSize, ImVec2 size) {
        ImGui::BeginGroup();
        ImGui::PushID(id);

        ImGui::SetNextItemWidth(size.x);
        ImGui::InputTextWithHint("##search", "Search", search, searchSize);
        ImGui::SetItemTooltip("Finds settings by name: the pages that have one, then only its rows");

        const SettingsRegistry& registry = SettingsRegistry::get();
        const std::vector<const SettingsPage*> pages = registry.pages(scope);
        const std::string query = jasperengine::editor::search::normalize(search);
        const int count = static_cast<int>(pages.size());
        bool selectedVisible = selected >= 0 && selected < count && registry.pageMatches(*pages[selected], query);
        if (!selectedVisible) {
            for (int i = 0; i < count; i++) {
                if (registry.pageMatches(*pages[i], query)) {
                    selected = i;
                    break;
                }
            }
        }

        const float listHeight = std::max(1.0f, size.y - ImGui::GetFrameHeightWithSpacing());
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(6.0f, 6.0f)));
        if (ImGui::BeginChild("##categories", ImVec2(size.x, listHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding)) {
            bool any = false;
            for (int i = 0; i < count; i++) {
                if (!registry.pageMatches(*pages[i], query)) continue;
                any = true;
                ImGui::PushID(i);
                if (ImGui::Selectable(pages[i]->displayName().c_str(), selected == i)) {
                    selected = i;
                }
                ImGui::PopID();
            }
            if (!any) ImGui::TextDisabled("No results");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::PopID();
        ImGui::EndGroup();
    }

    // The scope's page at an index, and the index of a page name (-1 when none)
    inline const SettingsPage* pageAt(SettingsScope scope, int index) {
        const std::vector<const SettingsPage*> pages = SettingsRegistry::get().pages(scope);
        return index >= 0 && index < static_cast<int>(pages.size()) ? pages[index] : nullptr;
    }
    inline int pageIndex(SettingsScope scope, const std::string& name) {
        const std::vector<const SettingsPage*> pages = SettingsRegistry::get().pages(scope);
        for (int i = 0; i < static_cast<int>(pages.size()); i++) {
            if (pages[i]->name == name || pages[i]->displayName() == name) return i;
        }
        return -1;
    }

}
