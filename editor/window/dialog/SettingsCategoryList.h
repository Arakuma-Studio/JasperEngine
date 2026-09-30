// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Unity-style settings layout shared by Preferences and Project Settings: a
// searchable category list on the left, a bold category title over the page.

#include "Theme.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace jasperengine::editor::settingsui {

    struct Category {
        const char* name;
        const char* icon;
        // Setting labels on the page, so the search finds a category by its contents.
        const char* keywords;
    };

    inline bool containsNoCase(const char* haystack, const char* needle) {
        if (!needle || !*needle) return true;
        if (!haystack) return false;
        const size_t needleLen = std::strlen(needle);
        for (const char* h = haystack; *h; ++h) {
            size_t i = 0;
            while (i < needleLen && h[i] &&
                   std::tolower(static_cast<unsigned char>(h[i])) == std::tolower(static_cast<unsigned char>(needle[i]))) {
                ++i;
            }
            if (i == needleLen) return true;
        }
        return false;
    }

    inline bool categoryMatches(const Category& category, const char* search) {
        return containsNoCase(category.name, search) || containsNoCase(category.keywords, search);
    }

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

    // Search field over the category list. Keeps `selected` on a visible category
    // while a search is active. Returns true when the selection changed.
    inline bool categoryList(const char* id, const Category* categories, int count, int& selected,
                             char* search, size_t searchSize, ImVec2 size) {
        bool changed = false;
        ImGui::BeginGroup();
        ImGui::PushID(id);

        ImGui::SetNextItemWidth(size.x);
        ImGui::InputTextWithHint("##search", "Search", search, searchSize);

        bool selectedVisible = selected >= 0 && selected < count && categoryMatches(categories[selected], search);
        if (!selectedVisible) {
            for (int i = 0; i < count; i++) {
                if (categoryMatches(categories[i], search)) {
                    changed = changed || selected != i;
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
                if (!categoryMatches(categories[i], search)) continue;
                any = true;
                ImGui::PushID(i);
                char label[128];
                if (categories[i].icon && *categories[i].icon) {
                    std::snprintf(label, sizeof(label), "%s  %s", categories[i].icon, categories[i].name);
                } else {
                    std::snprintf(label, sizeof(label), "%s", categories[i].name);
                }
                if (ImGui::Selectable(label, selected == i)) {
                    changed = changed || selected != i;
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
        return changed;
    }

}
