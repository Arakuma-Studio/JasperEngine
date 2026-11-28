// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LocalizationTablesWindow.h"

#include "Out.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "localization/Localization.h"
#include "localization/MessageFormat.h"
#include "localization/StringTableCook.h"
#include "localization/StringTableEditCmd.h"
#include "localization/StringTableInterchange.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/HtmlL10n.h"
#include "htmlui/HtmlUIInspector.h"
#include "io/AtomicFile.h"
#include "util/FileDialogs.h"
#include "util/ProjectChecks.h"
#include "util/TitleArtLanguage.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <optional>
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
    drawToolbar(set);
    // Add Entry, Add Language, search and filters
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
    const int columns = 3 + static_cast<int>(languages.size()) + (pseudoPreview ? 1 : 0) + 1;
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
        if (pseudoPreview) ImGui::TableSetupColumn("en-XA (Pseudo)");
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
            if (ImGui::Selectable(entry.key.c_str(), selectedKey == entry.key)) selectedKey = entry.key;
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
            if (pseudoPreview) {
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", Localization::pseudoLocalize(entry.text, "en-XA", set.table.sourceLanguage).c_str());
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) removeKey = entry.key;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!removeKey.empty()) apply(l10n::Json::array({{{"op", "remove"}, {"key", removeKey}}}), "Remove " + set.table.ns + "." + removeKey);
}

void LocalizationTablesWindow::openMisses() {
    setOpen(true);
    selectMisses = true;
}

void LocalizationTablesWindow::report(const std::string& title, std::vector<std::string> lines) {
    dialogTitle = title;
    dialogLines = std::move(lines);
    openDialog = true;
}

void LocalizationTablesWindow::drawToolbar(l10n::StringTableSet& set) {
    const std::string global = selectedKey.empty() ? std::string() : set.table.ns + "." + selectedKey;
    ImGui::BeginDisabled(selectedKey.empty() || !set.table.find(selectedKey));
    if (ImGui::Button("Rename Key...")) {
        std::snprintf(renameTo, sizeof(renameTo), "%s", selectedKey.c_str());
        ImGui::OpenPopup("Rename Key##l10n");
    }
    ImGui::SameLine();
    if (ImGui::Button("Find References")) {
        std::vector<std::string> lines;
        for (const l10n::KeyReference& reference : l10n::findReferences(project->getProjectPath(), global)) {
            lines.push_back(reference.file + ":" + std::to_string(reference.line) + " (" + reference.kind + ")");
        }
        if (lines.empty()) lines.push_back("No HTML, scene or script names " + global + ".");
        report("References to " + global, lines);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Import...")) ImGui::OpenPopup("Import##l10n");
    ImGui::SameLine();
    if (ImGui::Button("Export...")) {
        exportLanguages.clear();
        for (const auto& entry : set.translations) exportLanguages.push_back(entry.first);
        ImGui::OpenPopup("Export##l10n");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Pseudo Preview", &pseudoPreview);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("An en-XA column: the source accented, 40%% longer and bracketed, as the pseudo-locale shows it.");

    // Rename Key: the table and its translations, one undo step; the references it leaves are listed
    if (ImGui::BeginPopup("Rename Key##l10n")) {
        ImGui::Text("Rename %s to", global.c_str());
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("##renameTo", renameTo, sizeof(renameTo));
        const bool valid = l10n::isValidKey(renameTo) && !set.table.find(renameTo);
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Rename")) {
            const std::string from = selectedKey;
            const std::string to = renameTo;
            const std::vector<l10n::KeyReference> references = l10n::findReferences(project->getProjectPath(), set.table.ns + "." + from);
            if (apply(l10n::Json::array({{{"op", "rename"}, {"key", from}, {"to", to}}}), "Rename Key " + set.table.ns + "." + from)) {
                selectedKey = to;
                std::vector<std::string> lines = {"Renamed " + set.table.ns + "." + from + " to " + set.table.ns + "." + to + " in the table and its translations."};
                if (!references.empty()) lines.push_back("Not rewritten (update them to the new key):");
                for (const l10n::KeyReference& reference : references) {
                    lines.push_back("  " + reference.file + ":" + std::to_string(reference.line) + " (" + reference.kind + ")");
                }
                report("Rename Key", lines);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (!valid && renameTo[0]) ImGui::TextDisabled("a new key of [a-z0-9_.-], not in the table");
        ImGui::EndPopup();
    }

    // Import: a CSV or XLIFF file into this table, one undo step; the notes are listed
    if (ImGui::BeginPopup("Import##l10n")) {
        ImGui::Checkbox("Add new languages", &importAddLanguages);
        if (ImGui::Button("Choose File...")) {
            const std::string chosen = FileDialogs::openFileDialogFiltered(project->getProjectPath().string(), "Strings", "csv,xlf,xliff,tsv,txt");
            if (!chosen.empty()) {
                std::string body;
                std::string error;
                StringsFile strings;
                if (!ProjectChecks::readFile(chosen, body) || !parseStrings(body, "auto", strings, error)) {
                    report("Import", {"Could not read " + chosen + (error.empty() ? std::string() : ": " + error)});
                } else {
                    const l10n::StringTableSet before = set;
                    l10n::StringTableSet after = set;
                    int changed = 0;
                    const std::vector<StringsNote> notes = l10n::importStrings(after, strings, importAddLanguages, changed);
                    std::vector<std::string> lines = {"Imported " + std::to_string(changed) + " translation(s) from " + chosen + "."};
                    for (const StringsNote& note : notes) {
                        lines.push_back("  " + note.code + " " + note.key + (note.language.empty() ? "" : " (" + note.language + ")") + ": " + note.message);
                    }
                    if (changed > 0 && l10n::commitSet(project, &before, after, "Import Strings " + set.table.ns, &error)) set = std::move(after);
                    else if (!error.empty()) lines.push_back("Not written: " + error);
                    report("Import", lines);
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Export: CSV (one file) or XLIFF (one file per language) of the chosen languages
    if (ImGui::BeginPopup("Export##l10n")) {
        ImGui::RadioButton("CSV", &exportFormat, 0);
        ImGui::SameLine();
        ImGui::RadioButton("XLIFF", &exportFormat, 1);
        for (const auto& entry : set.translations) {
            bool on = std::find(exportLanguages.begin(), exportLanguages.end(), entry.first) != exportLanguages.end();
            if (ImGui::Checkbox(entry.first.c_str(), &on)) {
                if (on) exportLanguages.push_back(entry.first);
                else exportLanguages.erase(std::remove(exportLanguages.begin(), exportLanguages.end(), entry.first), exportLanguages.end());
            }
        }
        ImGui::BeginDisabled(exportLanguages.empty());
        if (ImGui::Button("Export...")) {
            const std::string name = set.table.ns + (exportFormat == 0 ? ".csv" : ".xlf");
            const std::string chosen = FileDialogs::saveFileDialog(project->getProjectPath().string(), name, exportFormat == 0 ? "CSV" : "XLIFF",
                                                                   exportFormat == 0 ? "csv" : "xlf");
            if (!chosen.empty()) {
                const fs::path target = chosen;
                const std::vector<StringsExportDoc> docs = {l10n::exportDocOf(set, exportLanguages)};
                std::vector<std::string> lines;
                auto write = [&](const fs::path& file, const std::string& body) {
                    const io::WriteResult result = io::writeFileAtomic(file, body);
                    lines.push_back((result.ok() ? "Wrote " : "Could not write ") + file.generic_string() + (result.ok() ? "" : ": " + result.message));
                };
                if (exportFormat == 0) {
                    write(target, exportStringsCsv(docs, exportLanguages));
                } else {
                    for (const auto& [code, body] : exportStringsXliff(docs, exportLanguages)) {
                        write(target.parent_path() / (target.stem().string() + "." + code + ".xlf"), body);
                    }
                }
                report("Export", lines);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

void LocalizationTablesWindow::drawDialogs(l10n::StringTableSet&) {
    if (openDialog) {
        ImGui::OpenPopup("Localization##l10nReport");
        openDialog = false;
    }
    if (ImGui::BeginPopup("Localization##l10nReport")) {
        ImGui::TextUnformatted(dialogTitle.c_str());
        ImGui::Separator();
        for (const std::string& line : dialogLines) ImGui::TextUnformatted(line.c_str());
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

std::string LocalizationTablesWindow::extractToTable(Project* project, const fs::path& table, const std::string& documentFile,
                                                     const std::string& text, std::string& error) {
    l10n::StringTableSet before;
    if (!l10n::loadSet(project->getProjectPath(), table, before, error)) return {};
    const fs::path html = project->getAssetsPath() / documentFile;
    std::string source;
    if (!ProjectChecks::readFile(html, source)) {
        error = documentFile + " cannot be read";
        return {};
    }
    l10n::StringTableSet after = before;
    std::string edited = source;
    std::string key;
    if (!l10n::extractText(after, edited, text, key, error)) {
        error += " (" + documentFile + ")";
        return {};
    }
    // the table's files and the document as one step
    std::vector<StringTableEditCmd::FileChange> changes;
    for (const auto& [path, body] : after.files()) {
        std::string disk;
        std::optional<std::string> was;
        if (ProjectChecks::readFile(path, disk)) was = disk;
        if (was != body) changes.push_back({path, was, body});
    }
    changes.push_back({html, source, edited});
    auto* command = new StringTableEditCmd(std::move(changes), "Extract to Table " + key, {}, project);
    if (!project->getProjectCommandHistory()->addCommandNoMerge(command)) {
        error = "the edit could not be written";
        return {};
    }
    return key;
}

void LocalizationTablesWindow::drawMisses() {
    // the documents' literal text (development builds and the editor): Extract to Table keys it
    if (selected >= 0 && selected < static_cast<int>(tables.size())) {
        int row = 0;
        for (SceneProject& sceneProject : project->getScenes()) {
            if (!sceneProject.scene) continue;
            for (Entity entity : htmlui::HtmlUIInspector::documents(sceneProject.scene)) {
                const UIDocumentComponent* document = sceneProject.scene->findComponent<UIDocumentComponent>(entity);
                if (!document) continue;
                for (const auto& [where, text] : htmlui::HtmlL10n::reportOf(sceneProject.scene, entity).literal) {
                    ImGui::PushID(row++);
                    if (ImGui::SmallButton("Extract to Table")) {
                        std::string error;
                        const std::string key = extractToTable(project, tables[selected].owner, document->filename, text, error);
                        report("Extract to Table", {key.empty() ? "Not extracted: " + error : "Added " + key + " and data-l10n-id on " + where + " in " + document->filename + "."});
                        dirty = true;
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s  %s: \"%s\"", document->filename.c_str(), where.c_str(), text.c_str());
                    ImGui::PopID();
                }
            }
        }
        if (row > 0) ImGui::Separator();
    }
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
                if (selected >= 0 && selected < static_cast<int>(tables.size())) {
                    drawGrid(tables[selected]);
                    drawDialogs(tables[selected]);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Play Misses", nullptr, selectMisses ? ImGuiTabItemFlags_SetSelected : 0)) {
            selectMisses = false;
            drawMisses();
            if (selected >= 0 && selected < static_cast<int>(tables.size())) drawDialogs(tables[selected]);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
