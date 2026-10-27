// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SettingsSearch.h"

#include "search/SearchScorer.h"

#include "imgui.h"

#include <cassert>

using namespace jasperengine::editor;

namespace {

    struct State {
        std::string page;
        std::string rawQuery;
        std::string query;            // normalized
        bool pageMatchesItself = false;
        std::string revealKey;
        bool revealPending = false;   // not drawn yet
        double flashUntil = 0.0;
        std::string currentKey;       // the row being drawn
        bool currentRevealed = false;
    };

    State& state() {
        static State instance;
        return instance;
    }

    bool rowVisible(const char* key) {
        State& s = state();
        const SettingRow* row = SettingsRegistry::get().find(key);
        // every drawn row is registered (R30): a new row needs its registry entry
        assert(row && "a settings row is drawn that SettingsRegistry does not have");
        if (s.query.empty() || s.pageMatchesItself) return true;
        return row && SettingsRegistry::rowMatches(*row, s.query);
    }

    void noteRow(const char* key) {
        State& s = state();
        s.currentKey = key;
        s.currentRevealed = s.revealKey == key;
        if (s.currentRevealed && s.revealPending) {
            s.revealPending = false;
            ImGui::SetScrollHereY(0.3f);
            s.flashUntil = ImGui::GetTime() + 1.5;
        }
    }

}

void settingsui::beginPage(const std::string& page, const char* search) {
    State& s = state();
    s.page = page;
    s.rawQuery = search ? search : "";
    s.query = jasperengine::editor::search::normalize(s.rawQuery);
    s.pageMatchesItself = false;
    if (!s.query.empty()) {
        if (const SettingsPage* settingsPage = SettingsRegistry::get().findPage(page)) {
            s.pageMatchesItself = jasperengine::editor::search::filterMatch(
                s.query, jasperengine::editor::search::normalize(settingsPage->displayName() + " " + settingsPage->keywords));
        }
    }
}

void settingsui::endPage() {
    State& s = state();
    s.page.clear();
    s.currentKey.clear();
    s.currentRevealed = false;
}

bool settingsui::searching() {
    return !state().query.empty();
}

bool settingsui::beginRow(const char* key) {
    if (!rowVisible(key)) return false;
    noteRow(key);
    return true;
}

bool settingsui::beginRow(std::initializer_list<const char*> keys) {
    const char* shown = nullptr;
    for (const char* key : keys) {
        if (rowVisible(key) && !shown) shown = key;
    }
    if (!shown) return false;
    // a reveal of any of them lands on the row
    for (const char* key : keys) {
        if (state().revealKey == key) shown = key;
    }
    noteRow(shown);
    return true;
}

bool settingsui::sectionVisible(const char* section) {
    State& s = state();
    if (s.query.empty() || s.pageMatchesItself) return true;
    for (const SettingRow* row : SettingsRegistry::get().rowsOf(s.page)) {
        if (row->section == section && SettingsRegistry::rowMatches(*row, s.query)) return true;
    }
    return false;
}

void settingsui::highlightedText(const char* text) {
    State& s = state();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (!s.query.empty()) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 color = ImGui::GetColorU32(ImVec4(0.95f, 0.75f, 0.2f, 0.35f));
        for (const auto& [begin, end] : jasperengine::editor::search::highlightRanges(s.rawQuery, text)) {
            const float x0 = ImGui::CalcTextSize(text, text + begin).x;
            const float x1 = ImGui::CalcTextSize(text, text + end).x;
            drawList->AddRectFilled(ImVec2(pos.x + x0, pos.y), ImVec2(pos.x + x1, pos.y + ImGui::GetTextLineHeightWithSpacing()), color, 2.0f);
        }
    }
    ImGui::TextUnformatted(text);
}

void settingsui::rowLabel(const char* label) {
    State& s = state();
    // every row is a registry row: drawn after its beginRow (R30)
    assert(!s.currentKey.empty() && "a settings row is drawn without settingsui::beginRow");
    if (s.currentRevealed && ImGui::GetTime() < s.flashUntil) {
        // the row the palette jumped to: a fading flash behind its label
        const float alpha = static_cast<float>((s.flashUntil - ImGui::GetTime()) / 1.5);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(pos.x - 4.0f, pos.y - 2.0f), ImVec2(pos.x + ImGui::GetContentRegionAvail().x, pos.y + ImGui::GetFrameHeight()),
            ImGui::GetColorU32(ImVec4(0.30f, 0.55f, 0.95f, 0.45f * alpha)), 3.0f);
    }
    highlightedText(label);
    s.currentKey.clear();
    s.currentRevealed = false;
}

void settingsui::requestReveal(const std::string& key) {
    State& s = state();
    s.revealKey = key;
    s.revealPending = true;
}

std::string settingsui::pendingRevealPage() {
    State& s = state();
    if (!s.revealPending) return {};
    const SettingRow* row = SettingsRegistry::get().find(s.revealKey);
    return row ? row->page : std::string();
}
