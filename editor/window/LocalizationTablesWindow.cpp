// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LocalizationTablesWindow.h"

#include "Out.h"
#include "Project.h"
#include "localization/Localization.h"
#include "localization/MessageFormat.h"
#include "localization/StringTableCook.h"
#include "localization/StringTableEditCmd.h"
#include "util/TitleArtLanguage.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    const ImVec4 kMissing(0.85f, 0.25f, 0.25f, 1.0f);
    const ImVec4 kReview(0.95f, 0.70f, 0.20f, 1.0f);

    size_t codePointCount(const std::string& text) {
        size_t count = 0;
        for (unsigned char c : text) if ((c & 0xC0) != 0x80) count++;
        return count;
    }

    bool containsText(const std::string& haystack, const char* needle) {
        if (!needle[0]) return true;
        std::string a = haystack;
        std::string b = needle;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return a.find(b) != std::string::npos;
    }

}

LocalizationTablesWindow::LocalizationTablesWindow(Project* project): project(project) {
    listener = StringTableEditCmd::addChangeListener([this](const std::vector<fs::path>&) { dirty = true; });
}

LocalizationTablesWindow::~LocalizationTablesWindow() {
    StringTableEditCmd::removeChangeListener(listener);
}

void LocalizationTablesWindow::setOpen(bool open) {
    windowOpen = open;
    if (open) {
        focusRequested = true;
        dirty = true;
    }
}

void LocalizationTablesWindow::open(const fs::path& table) {
    setOpen(true);
    reload();
    for (size_t i = 0; i < tables.size(); i++) {
        if (tables[i].owner == table) selected = static_cast<int>(i);
    }
}

void LocalizationTablesWindow::reload() {
    const l10n::ProjectTables loaded = l10n::loadProjectTables(project->getProjectPath());
    const fs::path current = selected >= 0 && selected < static_cast<int>(tables.size()) ? tables[selected].owner : fs::path();
    tables = loaded.sets;
    errors = loaded.errors;
    selected = tables.empty() ? -1 : 0;
    for (size_t i = 0; i < tables.size(); i++) {
        if (tables[i].owner == current) selected = static_cast<int>(i);
    }
    dirty = false;
}

bool LocalizationTablesWindow::apply(const l10n::Json& ops, const std::string& label, const std::string& mergeKey) {
    if (selected < 0) return false;
    const l10n::StringTableSet before = tables[selected];
    l10n::StringTableSet after = before;
    std::string error;
    if (!l10n::applyOps(after, ops, error) || !l10n::commitSet(project, &before, after, label, &error, mergeKey)) {
        if (!error.empty()) Out::error("Localization Tables: %s", error.c_str());
        return false;
    }
    tables[selected] = std::move(after);
    return true;
}

void LocalizationTablesWindow::drawTableList() {
    ImGui::TextDisabled("Tables");
    for (size_t i = 0; i < tables.size(); i++) {
        const l10n::StringTableSet& set = tables[i];
        std::string label = set.table.ns + "##table" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), selected == static_cast<int>(i))) selected = static_cast<int>(i);
        if (ImGui::IsItemHovered()) {
            std::string coverage;
            for (const std::string& code : set.languages()) {
                coverage += code + " " + std::to_string(static_cast<int>(set.coverage(code) * 100.0 + 0.5)) + "%  ";
            }
            ImGui::SetTooltip("%s\n%s", set.ownerRelative.c_str(), coverage.c_str());
        }
    }
    for (const std::string& error : errors) ImGui::TextColored(kMissing, "%s", error.c_str());
    if (tables.empty()) ImGui::TextDisabled("No String Tables: Create > Localization > String Table.");
}

void LocalizationTablesWindow::drawGrid(l10n::StringTableSet& set) {
    // toolbar: Add Entry, Add Language, search and filters
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##newKey", "menu.play", newKey, sizeof(newKey));
    ImGui::SameLine();
    ImGui::BeginDisabled(!l10n::isValidKey(newKey) || set.table.find(newKey));
    if (ImGui::Button("Add Entry")) {
        apply(l10n::Json::array({{{"op", "set"}, {"key", newKey}, {"text", ""}}}), "Add Entry " + std::string(newKey));
        newKey[0] = '\0';
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputTextWithHint("##newLanguage", "de", newLanguage, sizeof(newLanguage));
    ImGui::SameLine();
    ImGui::BeginDisabled(!TitleArtLanguage::isValid(newLanguage));
    if (ImGui::Button("Add Language")) {
        apply(l10n::Json::array({{{"op", "add_language"}, {"language", newLanguage}}}), "Add Language " + std::string(newLanguage));
        newLanguage[0] = '\0';
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##search", "Search keys and texts", search, sizeof(search));
    ImGui::SameLine();
    ImGui::Checkbox("Missing", &filterMissing);
    ImGui::SameLine();
    ImGui::Checkbox("Needs Review", &filterReview);
    ImGui::SameLine();
    ImGui::Checkbox("Over Length", &filterOverLength);

    const std::vector<std::string> languages = set.languages();
    const int columns = 3 + static_cast<int>(languages.size()) + 1;
    std::string removeKey;
    if (ImGui::BeginTable("##grid", columns, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable)) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Context");
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        for (const std::string& code : languages) {
            ImGui::TableSetupColumn((code == set.table.sourceLanguage ? code + " (source)" : code).c_str());
        }
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();
        for (const l10n::TableEntry& entry : set.table.entries) {
            bool match = containsText(entry.key, search) || containsText(entry.text, search);
            bool anyMissing = false, anyReview = false, anyOver = entry.maxChars > 0 && codePointCount(entry.text) > static_cast<size_t>(entry.maxChars);
            for (const auto& [code, doc] : set.translations) {
                const auto found = doc.entries.find(entry.key);
                if (found == doc.entries.end() || found->second.text.empty()) {
                    anyMissing = true;
                    continue;
                }
                match = match || containsText(found->second.text, search);
                anyReview = anyReview || found->second.status == "needs_review" || found->second.sourceHash != l10n::sourceHash(entry.text);
                anyOver = anyOver || (entry.maxChars > 0 && codePointCount(found->second.text) > static_cast<size_t>(entry.maxChars));
            }
            if (!match || (filterMissing && !anyMissing) || (filterReview && !anyReview) || (filterOverLength && !anyOver)) continue;
            ImGui::PushID(entry.key.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.key.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.context.c_str());
            ImGui::TableNextColumn();
            if (entry.maxChars > 0) ImGui::Text("%d", entry.maxChars);
            for (const std::string& code : languages) {
                ImGui::TableNextColumn();
                ImGui::PushID(code.c_str());
                const bool source = code == set.table.sourceLanguage;
                std::string text = entry.text;
                bool missing = false, review = false;
                if (!source) {
                    const auto& doc = set.translations.at(code);
                    const auto found = doc.entries.find(entry.key);
                    missing = found == doc.entries.end() || found->second.text.empty();
                    if (!missing) {
                        text = found->second.text;
                        review = found->second.status == "needs_review" || found->second.sourceHash != l10n::sourceHash(entry.text);
                    } else {
                        text.clear();
                    }
                }
                if (missing) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(kMissing.x, kMissing.y, kMissing.z, 0.35f)));
                else if (review) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(kReview.x, kReview.y, kReview.z, 0.35f)));
                const bool editing = editKey == entry.key && editLanguage == code;
                char cell[4096];
                std::snprintf(cell, sizeof(cell), "%s", (editing ? editBuffer : text).c_str());
                ImGui::SetNextItemWidth(-1.0f);
                const bool changed = ImGui::InputTextMultiline("##cell", cell, sizeof(cell), ImVec2(-1.0f, ImGui::GetTextLineHeight() * 2.2f));
                const std::string buffer = cell;
                if (ImGui::IsItemActivated()) {
                    editKey = entry.key;
                    editLanguage = code;
                    editBuffer = text;
                    editSession++;
                }
                if (changed && editKey == entry.key && editLanguage == code) {
                    editBuffer = buffer;
                    jasperengine::l10n::MessageError problem;
                    editProblem = jasperengine::l10n::MessageFormat::validate(buffer, &problem)
                                      ? std::string()
                                      : "column " + std::to_string(jasperengine::l10n::MessageFormat::column(buffer, problem.offset)) + ": " + problem.message;
                    // a valid text is written as typed, one undo step per typing session
                    if (editProblem.empty()) {
                        l10n::Json op = {{"op", "set"}, {"key", entry.key}, {"text", buffer}};
                        if (!source) op["language"] = code;
                        apply(l10n::Json::array({op}), "Edit " + set.table.ns + "." + entry.key + " (" + code + ")",
                              "cell:" + set.ownerRelative + ":" + entry.key + ":" + code + ":" + std::to_string(editSession));
                    }
                }
                if (ImGui::IsItemDeactivated() && editKey == entry.key && editLanguage == code) {
                    editKey.clear();
                    editLanguage.clear();
                    editProblem.clear();
                }
                if (entry.maxChars > 0 && codePointCount(text) > static_cast<size_t>(entry.maxChars)) {
                    ImGui::TextColored(kMissing, "%zu/%d", codePointCount(text), entry.maxChars);
                }
                if (editing && !editProblem.empty()) ImGui::TextColored(kMissing, "%s", editProblem.c_str());
                if (ImGui::SmallButton("Insert Plural")) {
                    l10n::Json op = {{"op", "set"}, {"key", entry.key}, {"text", l10n::pluralSkeleton(code)}};
                    if (!source) op["language"] = code;
                    apply(l10n::Json::array({op}), "Insert Plural " + set.table.ns + "." + entry.key + " (" + code + ")");
                }
                if (review) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Mark Reviewed")) {
                        apply(l10n::Json::array({{{"op", "mark_reviewed"}, {"key", entry.key}, {"language", code}}}),
                              "Mark Reviewed " + set.table.ns + "." + entry.key + " (" + code + ")");
                    }
                }
                ImGui::PopID();
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) removeKey = entry.key;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!removeKey.empty()) apply(l10n::Json::array({{{"op", "remove"}, {"key", removeKey}}}), "Remove " + set.table.ns + "." + removeKey);
}

void LocalizationTablesWindow::drawMisses() {
    const std::vector<Localization::Miss> misses = Localization::getMisses();
    if (misses.empty()) {
        ImGui::TextDisabled("No misses this Play (untranslated, unknown or stale keys appear here while playing).");
        return;
    }
    if (ImGui::BeginTable("##misses", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Language");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Where");
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableHeadersRow();
        for (const Localization::Miss& miss : misses) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(miss.key.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(miss.language.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(miss.kind == "stale" ? kReview : kMissing, "%s", miss.kind.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(miss.where.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%u", miss.count);
        }
        ImGui::EndTable();
    }
}

void LocalizationTablesWindow::show() {
    if (!windowOpen) return;
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    if (dirty) reload();
    if (ImGui::BeginTabBar("##localizationTabs")) {
        if (ImGui::BeginTabItem("Tables")) {
            if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("##list", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                ImGui::TableSetupColumn("##grid");
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                drawTableList();
                ImGui::TableNextColumn();
                if (selected >= 0 && selected < static_cast<int>(tables.size())) drawGrid(tables[selected]);
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Play Misses")) {
            drawMisses();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
