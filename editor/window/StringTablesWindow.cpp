// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringTablesWindow.h"

#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "titleart/localisation/TitleLanguageNames.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string stwLower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    int stwResize(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* text = static_cast<std::string*>(data->UserData);
            text->resize((size_t)data->BufTextLen);
            data->Buf = text->data();
        }
        return 0;
    }

    // what a cell shows: the text, or the plural forms in one line
    std::string stwShown(const StringTableValue* value) {
        if (!value) return std::string();
        if (!value->plural) return value->text;
        std::string out;
        for (int c = 0; c < PLURAL_CATEGORY_COUNT; c++) {
            if (!value->has((PluralCategory)c)) continue;
            out += (out.empty() ? "" : " | ") + std::string(PluralRules::name((PluralCategory)c)) + ": " + value->forms[(size_t)c];
        }
        return out;
    }

    bool stwMissing(const StringTableValue* value, const std::string& language) {
        if (!value || value->empty()) return true;
        if (!value->plural) return false;
        for (PluralCategory category : stringtables::requiredForms(language)) {
            if (!value->has(category)) return true;
        }
        return false;
    }

}

StringTablesWindow::StringTablesWindow(Project* project): project(project) {}

void StringTablesWindow::open(const fs::path& table) {
    windowOpen = true;
    if (!table.empty()) wanted = table.lexically_normal();
    listedAt = -1000.0;
    refreshList(true);
    ImGui::SetWindowFocus(WINDOW_NAME);
}

bool StringTablesWindow::isOpen() const {
    return windowOpen;
}

void StringTablesWindow::resetForProjectSwitch() {
    tables.clear();
    listedVersion = ~uint64_t(0);
    selected = 0;
    wanted.clear();
    selectedKey.clear();
    notice.clear();
    editId = 0;
}

void StringTablesWindow::refreshList(bool force) {
    const uint64_t version = project->getAssetDatabase().getVersion();
    if (!force && version == listedVersion) return;
    listedVersion = version;
    const fs::path current = !wanted.empty() ? wanted : (selected < tables.size() ? tables[selected].path : fs::path());
    tables = stringtables::readAll(project->getProjectPath(), project->getAssetsPath());
    selected = 0;
    for (size_t i = 0; i < tables.size(); i++) {
        if (tables[i].path.lexically_normal() == current.lexically_normal()) selected = i;
    }
    wanted.clear();
    reloadSelected(true);
}

void StringTablesWindow::reloadSelected(bool force) {
    if (selected >= tables.size()) {
        languages.clear();
        return;
    }
    stringtables::TableFile& table = tables[selected];
    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(table.path, ec);
    const uintmax_t size = ec ? 0 : fs::file_size(table.path, ec);
    if (!force && stamp == loadedStamp && size == loadedSize) return;
    loadedStamp = stamp;
    loadedSize = size;
    table.problems.clear();
    table.error.clear();
    stringtables::read(table.path, table.table, table.error, &table.problems);
    languages = stringtables::editLanguages(project, table.table);
}

void StringTablesWindow::select(size_t index) {
    selected = index;
    selectedKey.clear();
    editId = 0;
    reloadSelected(true);
}

bool StringTablesWindow::commit(const StringTable& changed, const std::string& stepName) {
    if (selected >= tables.size()) return false;
    std::string error;
    if (!stringtables::write(project, tables[selected].path, changed, stepName, error)) {
        notice = error;
        noticeError = true;
        return false;
    }
    tables[selected].table = changed;
    reloadSelected(true);
    return true;
}

bool StringTablesWindow::matches(const StringTableEntry& entry) const {
    if (!search[0]) return true;
    const std::string wantedText = stwLower(search);
    if (stwLower(entry.key).find(wantedText) != std::string::npos || stwLower(entry.comment).find(wantedText) != std::string::npos) return true;
    for (const auto& value : entry.values) {
        if (stwLower(stwShown(&value.second)).find(wantedText) != std::string::npos) return true;
    }
    return false;
}

bool StringTablesWindow::editCell(const char* id, const std::string& value, std::string& out, float width, bool multiline) {
    const ImGuiID itemId = ImGui::GetID(id);
    std::string shown = value;
    std::string* buffer = itemId == editId ? &editBuffer : &shown;
    ImGui::SetNextItemWidth(width);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    if (multiline) {
        ImGui::InputTextMultiline(id, buffer->data(), buffer->capacity() + 1, ImVec2(width, Theme::dpi(60.0f)), flags, stwResize, buffer);
    } else {
        ImGui::InputText(id, buffer->data(), buffer->capacity() + 1, flags, stwResize, buffer);
    }
    if (ImGui::IsItemActivated()) {
        editId = itemId;
        editBuffer = value;
    }
    if (itemId == editId && ImGui::IsItemDeactivated()) {
        editId = 0;
        if (ImGui::IsItemDeactivatedAfterEdit() && editBuffer != value) {
            out = editBuffer;
            return true;
        }
    }
    return false;
}

void StringTablesWindow::show() {
    if (!windowOpen) return;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(900.0f), Theme::dpi(560.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    UIProbe::rect("stringTables", ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                  ImGui::GetWindowPos().y + ImGui::GetWindowSize().y);
    refreshList(false);
    const double now = ImGui::GetTime();
    if (now - checkedAt > 0.5) {
        checkedAt = now;
        // an undo, an agent's edit or another program's: the file on disk is the truth
        if (editId == 0) reloadSelected(false);
    }
    drawToolbar();
    if (selected < tables.size()) {
        const stringtables::TableFile& table = tables[selected];
        if (!table.error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s does not load: %s", table.relative.c_str(), table.error.c_str());
        } else {
            for (const std::string& problem : table.problems) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", problem.c_str());
            const float detailsHeight = selectedKey.empty() ? 0.0f : Theme::dpi(190.0f);
            ImGui::BeginChild("##stringTablesGrid", ImVec2(0.0f, -detailsHeight), ImGuiChildFlags_None);
            drawGrid();
            ImGui::EndChild();
            if (!selectedKey.empty()) drawDetails();
        }
    } else {
        ImGui::TextDisabled("No String Table in the project. New Table makes one (or Assets > Create > String Table).");
    }
    ImGui::End();
}

void StringTablesWindow::drawToolbar() {
    const std::string current = selected < tables.size() ? tables[selected].relative : std::string("(none)");
    ImGui::SetNextItemWidth(Theme::dpi(260.0f));
    if (ImGui::BeginCombo("##stringTablesTable", current.c_str())) {
        for (size_t i = 0; i < tables.size(); i++) {
            if (ImGui::Selectable(tables[i].relative.c_str(), i == selected)) select(i);
        }
        ImGui::EndCombo();
    }
    UIProbe::item("stringTables.table");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS " New Table")) {
        std::string error;
        const fs::path created = stringtables::createNew(project, project->getAssetsPath(), error);
        if (created.empty()) {
            notice = error;
            noticeError = true;
        } else {
            wanted = created;
            refreshList(true);
        }
    }
    const bool hasTable = selected < tables.size() && tables[selected].error.empty();
    ImGui::BeginDisabled(!hasTable);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_IMPORT " Import...")) importFile();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Export")) ImGui::OpenPopup("##stringTablesExport");
    if (ImGui::BeginPopup("##stringTablesExport")) {
        if (ImGui::MenuItem("CSV...")) exportCsv();
        if (ImGui::MenuItem("XLIFF 1.2...")) exportXliff();
        ImGui::EndPopup();
    }

    ImGui::SetNextItemWidth(Theme::dpi(220.0f));
    ImGui::InputTextWithHint("##stringTablesSearch", ICON_FA_MAGNIFYING_GLASS " Search keys and values", search, sizeof(search));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(200.0f));
    const bool entered = ImGui::InputTextWithHint("##stringTablesNewKey", "new.key", newKey, sizeof(newKey), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const std::string why = StringTable::keyProblem(newKey);
    const bool taken = hasTable && tables[selected].table.find(newKey) != nullptr;
    ImGui::BeginDisabled(!why.empty() || taken);
    if ((ImGui::Button(ICON_FA_PLUS " Add Key") || entered) && why.empty() && !taken) {
        StringTable changed = tables[selected].table;
        StringTableEntry entry;
        entry.key = newKey;
        changed.entries.push_back(entry);
        if (commit(changed, "Add String Table Key " + std::string(newKey))) {
            selectedKey = newKey;
            newKey[0] = '\0';
        }
    }
    ImGui::EndDisabled();
    if (newKey[0] && (taken || !why.empty())) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", taken ? "the table has this key" : why.c_str());
    }
    ImGui::EndDisabled();

    // missing values per language, the cells marked below
    if (hasTable && !languages.empty()) {
        std::string summary = "Missing:";
        for (const std::string& code : languages) {
            summary += " " + code + " " + std::to_string(stringtables::missingKeys(tables[selected].table, code).size());
        }
        ImGui::TextDisabled("%s  (%zu keys)", summary.c_str(), tables[selected].table.entries.size());
    }
    if (!notice.empty()) {
        ImGui::TextColored(noticeError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", notice.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss##stringTablesNotice")) notice.clear();
    }
}

void StringTablesWindow::drawGrid() {
    const StringTable& table = tables[selected].table;
    const int columns = 3 + (int)languages.size();
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##stringTablesCells", columns, flags)) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(170.0f));
    ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(140.0f));
    for (const std::string& code : languages) {
        ImGui::TableSetupColumn(titleLanguageLabel(code).c_str(), ImGuiTableColumnFlags_WidthFixed, Theme::dpi(200.0f));
    }
    ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(28.0f));
    ImGui::TableHeadersRow();

    std::string removeKey;
    StringTable changed;
    std::string stepName;
    for (size_t row = 0; row < table.entries.size(); row++) {
        const StringTableEntry& entry = table.entries[row];
        if (!matches(entry)) continue;
        ImGui::PushID((int)row);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool isSelected = entry.key == selectedKey;
        if (ImGui::Selectable(entry.key.c_str(), isSelected, ImGuiSelectableFlags_AllowOverlap)) selectedKey = entry.key;
        ImGui::TableSetColumnIndex(1);
        std::string edited;
        if (editCell("##comment", entry.comment, edited, -1.0f)) {
            changed = table;
            changed.find(entry.key)->comment = edited;
            stepName = "Edit String Table Comment " + entry.key;
        }
        for (size_t l = 0; l < languages.size(); l++) {
            ImGui::TableSetColumnIndex(2 + (int)l);
            const StringTableValue* value = entry.find(languages[l]);
            if (stwMissing(value, languages[l])) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0.75f, 0.25f, 0.2f, 0.35f)));
            ImGui::PushID((int)l);
            if (value && value->plural) {
                // plural forms are edited in the details below
                if (ImGui::Selectable(stwShown(value).c_str(), false)) selectedKey = entry.key;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Plural: edit its forms in the details below");
            } else if (editCell("##value", value ? value->text : std::string(), edited, -1.0f)) {
                changed = table;
                if (edited.empty()) changed.find(entry.key)->erase(languages[l]);
                else changed.find(entry.key)->set(languages[l], StringTableValue::ofText(edited));
                stepName = "Edit String Table " + entry.key + " (" + languages[l] + ")";
            }
            ImGui::PopID();
        }
        ImGui::TableSetColumnIndex(columns - 1);
        if (ImGui::SmallButton(ICON_FA_XMARK)) removeKey = entry.key;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the key %s", entry.key.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (!removeKey.empty()) {
        changed = table;
        changed.erase(removeKey);
        if (selectedKey == removeKey) selectedKey.clear();
        commit(changed, "Remove String Table Key " + removeKey);
    } else if (!stepName.empty()) {
        commit(changed, stepName);
    }
}

void StringTablesWindow::drawDetails() {
    const StringTable& table = tables[selected].table;
    const StringTableEntry* entry = table.find(selectedKey);
    if (!entry) {
        selectedKey.clear();
        return;
    }
    ImGui::Separator();
    ImGui::BeginChild("##stringTablesDetails", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    StringTable changed;
    std::string stepName;
    std::string edited;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Key");
    ImGui::SameLine(Theme::dpi(90.0f));
    if (editCell("##detailsKey", entry->key, edited, Theme::dpi(260.0f))) {
        const std::string why = StringTable::keyProblem(edited);
        if (!why.empty() || table.find(edited)) {
            notice = !why.empty() ? why : "the table already has the key '" + edited + "'";
            noticeError = true;
        } else {
            changed = table;
            changed.find(entry->key)->key = edited;
            stepName = "Rename String Table Key " + entry->key;
            selectedKey = edited;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Lua: Localization.get(\"%s\")", entry->key.c_str());
    for (const std::string& code : languages) {
        ImGui::PushID(code.c_str());
        const StringTableValue* value = entry->find(code);
        bool plural = value && value->plural;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(code.c_str());
        ImGui::SameLine(Theme::dpi(90.0f));
        if (ImGui::Checkbox("Plural", &plural)) {
            changed = table;
            StringTableValue next;
            if (plural) next.setForm(PluralCategory::OTHER, value ? value->text : std::string());
            else next = StringTableValue::ofText(value ? value->form(PluralCategory::OTHER) : std::string());
            changed.find(entry->key)->set(code, next);
            stepName = std::string(plural ? "Make Plural " : "Make Text ") + entry->key + " (" + code + ")";
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A form per count (CLDR rules of %s); the count is the `count` argument, else {0}", code.c_str());
        if (plural) {
            std::vector<PluralCategory> shown = PluralRules::categories(code);
            for (int c = 0; c < PLURAL_CATEGORY_COUNT; c++) {
                if (value->has((PluralCategory)c) && std::find(shown.begin(), shown.end(), (PluralCategory)c) == shown.end()) shown.push_back((PluralCategory)c);
            }
            std::sort(shown.begin(), shown.end());
            for (PluralCategory category : shown) {
                ImGui::SameLine();
                ImGui::PushID((int)category);
                ImGui::TextUnformatted(PluralRules::name(category));
                ImGui::SameLine();
                const std::string current = value->has(category) ? value->forms[(size_t)category] : std::string();
                if (editCell("##form", current, edited, Theme::dpi(150.0f))) {
                    changed = table;
                    StringTableValue next = *value;
                    next.setForm(category, edited);
                    changed.find(entry->key)->set(code, next);
                    stepName = "Edit String Table " + entry->key + " (" + code + " " + PluralRules::name(category) + ")";
                }
                ImGui::PopID();
            }
        } else {
            ImGui::SameLine();
            if (editCell("##text", value ? value->text : std::string(), edited, -1.0f, true)) {
                changed = table;
                if (edited.empty()) changed.find(entry->key)->erase(code);
                else changed.find(entry->key)->set(code, StringTableValue::ofText(edited));
                stepName = "Edit String Table " + entry->key + " (" + code + ")";
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!stepName.empty()) commit(changed, stepName);
}

void StringTablesWindow::importFile() {
    const std::string chosen = FileDialogs::openFileDialogFiltered(editor::FileUtils::pathToUtf8(project->getProjectPath()), "CSV or XLIFF", "csv,xlf,xliff");
    if (chosen.empty()) return;
    std::string text;
    if (!ProjectChecks::readFile(editor::FileUtils::pathFromUtf8(chosen), text)) {
        notice = "cannot read " + chosen;
        noticeError = true;
        return;
    }
    StringTable changed = tables[selected].table;
    stringtables::ImportReport report;
    std::string error;
    if (!stringtables::importText(changed, text, "", project->getLocalizationSettings().effectiveDefault(), project->getLocalizationSettings().languages,
                                  report, error)) {
        notice = "Import: " + error;
        noticeError = true;
        return;
    }
    if (commit(changed, "Import Strings into " + tables[selected].name)) {
        notice = "Imported " + std::to_string(report.added) + " new key(s), " + std::to_string(report.changed) + " changed" +
                 (report.notes.empty() ? std::string(".") : "; " + report.notes.front());
        noticeError = false;
    }
}

void StringTablesWindow::exportCsv() {
    const stringtables::TableFile& table = tables[selected];
    const std::string chosen = FileDialogs::saveFileDialog(editor::FileUtils::pathToUtf8(table.path.parent_path()), table.name + ".csv", "CSV", "csv");
    if (chosen.empty()) return;
    fs::path target = editor::FileUtils::pathFromUtf8(chosen);
    if (target.extension() != ".csv") target += ".csv";
    std::string error;
    if (!stringtables::writeExport(project, {{target, stringtables::exportCsv(table.table, languages)}}, error)) {
        notice = error;
        noticeError = true;
        return;
    }
    notice = "Exported " + editor::FileUtils::pathToUtf8(target);
    noticeError = false;
}

void StringTablesWindow::exportXliff() {
    const stringtables::TableFile& table = tables[selected];
    const std::string chosen = FileDialogs::saveFileDialog(editor::FileUtils::pathToUtf8(table.path.parent_path()), table.name + ".xlf", "XLIFF", "xlf");
    if (chosen.empty() || languages.empty()) return;
    // one file per target language: <name>.<code>.xlf
    const fs::path base = editor::FileUtils::pathFromUtf8(chosen);
    const std::string source = project->getLocalizationSettings().effectiveDefault();
    std::vector<std::pair<fs::path, std::string>> files;
    for (const auto& [code, text] : stringtables::exportXliff(table.table, table.relative, source, languages)) {
        files.push_back({base.parent_path() / (base.stem().string() + "." + code + ".xlf"), text});
    }
    std::string error;
    if (!stringtables::writeExport(project, files, error)) {
        notice = error;
        noticeError = true;
        return;
    }
    notice = "Exported " + std::to_string(files.size()) + " XLIFF file(s) beside " + editor::FileUtils::pathToUtf8(base);
    noticeError = false;
}
