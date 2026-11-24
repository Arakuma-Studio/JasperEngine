// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SavedDataWindow.h"

#include "Backend.h"
#include "App.h"
#include "Out.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "io/UserSettings.h"
#include "io/UserSettingsStore.h"
#include "window/StatusBar.h"
#include "window/dialog/EditorSettingsWindow.h"

#include "imgui.h"

#include <chrono>
#include <cstdio>
#include <ctime>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    double nowSeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string sizeText(uint64_t bytes) {
        char text[32];
        if (bytes < 1024) std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
        else if (bytes < 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f KiB", bytes / 1024.0);
        else std::snprintf(text, sizeof(text), "%.1f MiB", bytes / (1024.0 * 1024.0));
        return text;
    }

    std::string timeText(double utcSeconds) {
        if (utcSeconds <= 0) return "-";
        const std::time_t t = static_cast<std::time_t>(utcSeconds);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
        return text;
    }

    const char* statusLabel(const std::string& status) {
        if (status == "ok") return "OK";
        if (status == "recovered") return "Recovered";
        if (status == "corrupt") return "Corrupt";
        if (status == "newer_version") return "Newer Version";
        if (status == "newer_format") return "Newer Format";
        return status.c_str();
    }

    std::string titleOf(const savegame::SlotInfo& info) {
        nlohmann::json meta;
        savegame::SaveError error;
        if (info.meta.empty() || !savegame::toJson(info.meta, meta, error) || !meta.is_object()) return "";
        auto title = meta.find("title");
        return title != meta.end() && title->is_string() ? title->get<std::string>() : "";
    }

    // "(4, 0, 2)" for the JSON view's vectors, null otherwise
    bool vectorText(const nlohmann::json& value, std::string& out) {
        if (!value.is_object() || value.size() != 1) return false;
        const std::string& key = value.begin().key();
        if (key != "$vec2" && key != "$vec3" && key != "$vec4" && key != "$quat") return false;
        out = "(";
        for (size_t i = 0; i < value.begin().value().size(); i++) {
            const nlohmann::json& c = value.begin().value()[i];
            char text[32];
            std::snprintf(text, sizeof(text), "%g", c.is_number() ? c.get<double>() : 0.0);
            out += (i ? ", " : "") + std::string(text);
        }
        out += ")";
        return true;
    }

    void drawValue(const std::string& key, const nlohmann::json& value) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        std::string vector;
        const bool isVector = vectorText(value, vector);
        const bool isTable = !isVector && (value.is_object() || value.is_array()) &&
                             !(value.is_object() && value.size() == 1 && value.begin().key().rfind('$', 0) == 0 &&
                               value.begin().key().rfind("$$", 0) != 0 && value.begin().key().rfind("$bytes:", 0) != 0);
        bool open = false;
        if (isTable) {
            open = ImGui::TreeNodeEx(key.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
        } else {
            ImGui::TreeNodeEx(key.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);
        }
        ImGui::TableSetColumnIndex(1);
        std::string type, text;
        if (isVector) {
            const std::string& name = value.begin().key();
            type = name == "$vec2" ? "Vector2" : name == "$vec3" ? "Vector3" : name == "$vec4" ? "Vector4" : "Quaternion";
            text = vector;
        } else if (isTable) {
            type = "table";
            text = std::to_string(value.size()) + (value.size() == 1 ? " entry" : " entries");
        } else if (value.is_null()) {
            type = "Json.null";
        } else if (value.is_boolean()) {
            type = "boolean";
            text = value.get<bool>() ? "true" : "false";
        } else if (value.is_number_integer()) {
            type = "integer";
            text = value.dump();
        } else if (value.is_number() || (value.is_object() && value.contains("$float"))) {
            type = "float";
            text = value.is_number() ? value.dump() : value["$float"].get<std::string>();
        } else if (value.is_string()) {
            type = "string";
            const std::string& s = value.get_ref<const std::string&>();
            text = s.size() > 120 ? "\"" + s.substr(0, 120) + "...\" (" + sizeText(s.size()) + ")" : "\"" + s + "\"";
        } else {
            type = "bytes";
            text = value.dump();
        }
        ImGui::TextUnformatted(type.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(text.c_str());
        if (open) {
            if (value.is_array()) {
                for (size_t i = 0; i < value.size(); i++) drawValue("[" + std::to_string(i + 1) + "]", value[i]);
            } else {
                for (auto it = value.begin(); it != value.end(); ++it) drawValue(it.key(), it.value());
            }
            ImGui::TreePop();
        }
    }

}

SavedDataWindow::SavedDataWindow(Project* project) : project(project) {
}

void SavedDataWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
    if (open) seenChanges = UINT64_MAX;
}

bool SavedDataWindow::run(SaveSlotCmd::Action action, const std::string& slot, uint64_t generation, savegame::CorruptMode mode,
                          std::string* error) {
    auto* command = new SaveSlotCmd(action, slot, generation, mode);
    command->setNoMerge();
    CommandHistory* history = project->getProjectCommandHistory();
    // the store's message is in the Console already (the command or the history wrote it)
    if (!history->addCommand(command)) {
        std::string message = history->lastRefusal().empty() ? "The saved data could not be changed (see the Console)." : history->lastRefusal();
        StatusBar::setTransientMessage(message, 6.0);
        if (error) *error = message;
        return false;
    }
    seenChanges = UINT64_MAX;
    loadedChanges = UINT64_MAX;
    // stopped partway: what it changed is in the history (the store's message is in the Console)
    if (!command->partialFailure().empty()) {
        const std::string message = command->partialFailure() + " What changed can be undone (Edit > Undo).";
        StatusBar::setTransientMessage(message, 6.0);
        if (error) *error = message;
        return false;
    }
    return true;
}

void SavedDataWindow::select(const std::string& slot) {
    selected = slot;
    loadSelected();
}

void SavedDataWindow::refresh(bool force) {
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    const double now = nowSeconds();
    // at most 4 times a second, from the store's memory for the slots it holds
    if (!force && (now - lastRefresh < 0.25 || (store.changeCount() == seenChanges && now - lastRefresh < 1.0))) return;
    seenChanges = store.changeCount();
    lastRefresh = now;
    slots = store.list();
    // the selected slot is read again only when it may differ (once a second it read every
    // generation in full)
    if (!selected.empty() && (store.changeCount() != loadedChanges || store.slotSignature(selected) != loadedSignature)) loadSelected();
}

void SavedDataWindow::loadSelected() {
    selectedJson = nlohmann::json();
    selectedProblem.clear();
    backups.clear();
    if (selected.empty()) return;
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    loadedChanges = store.changeCount();
    loadedSignature = store.slotSignature(selected);
    savegame::SlotData data;
    // read only: viewing a slot repairs nothing (the game's own read sets a damaged file aside)
    const savegame::SaveError error = store.peekSlot(selected, data);
    if (!error.ok()) {
        selectedProblem = error.message;
    } else {
        selectedJson = nlohmann::json::object();
        nlohmann::json namespaces = nlohmann::json::object();
        for (const savegame::SaveSection& section : data.sections) {
            nlohmann::json value;
            savegame::SaveError decode;
            if (savegame::toJson(*section.payload, value, decode)) namespaces[section.name] = std::move(value);
        }
        selectedJson["namespaces"] = std::move(namespaces);
        nlohmann::json meta, requirements;
        savegame::SaveError ignored;
        if (!data.info.meta.empty() && savegame::toJson(data.info.meta, meta, ignored)) selectedJson["meta"] = meta;
        if (!data.info.requirements.empty() && savegame::toJson(data.info.requirements, requirements, ignored)) selectedJson["requires"] = requirements;
        selectedJson["version"] = data.info.version;
        selectedJson["generation"] = data.info.generation;
    }
    store.backups(selected, backups);
}

void SavedDataWindow::show() {
    if (!windowOpen) return;
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##SavedDataTabs")) {
        if (ImGui::BeginTabItem("Save Slots")) {
            drawSlots();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("PlayerPrefs")) {
            drawPrefs();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void SavedDataWindow::drawSlots() {
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    bool force = false;
    if (ImGui::Button("Refresh")) force = true;
    ImGui::SameLine();
    if (ImGui::Button("Open Folder")) {
        std::error_code ec;
        std::filesystem::create_directories(store.folder(), ec);
        EditorSettingsWindow::openInFileManager(store.folder());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(slots.empty());
    if (ImGui::Button("Clear All Saves...")) {
        Backend::getApp().registerConfirmAlert("Clear All Saves",
            "Delete every SaveGame slot of this project (" + std::to_string(slots.size()) + ")?\n" + store.folder().string() +
            "\nEdit > Undo brings them back.",
            [this]() { run(SaveSlotCmd::Action::ClearAll, ""); });
    }
    ImGui::EndDisabled();
    refresh(force);

    const float listHeight = selected.empty() ? 0.0f : ImGui::GetContentRegionAvail().y * 0.4f;
    if (ImGui::BeginTable("##SaveSlots", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_Resizable, ImVec2(0.0f, listHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Slot");
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Saved");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Backups");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Title");
        ImGui::TableHeadersRow();
        for (const savegame::SlotInfo& info : slots) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // a dot: written in this Play session
            const std::string label = (info.writtenThisSession ? "\xE2\x97\x8F " : "") + info.slot;
            if (ImGui::Selectable(label.c_str(), selected == info.slot, ImGuiSelectableFlags_SpanAllColumns)) {
                select(info.slot);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", info.version);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(timeText(info.savedAt).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(sizeText(info.size).c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", info.generations > 0 ? info.generations - 1 : 0);
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(statusLabel(info.status));
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(titleOf(info).c_str());
        }
        ImGui::EndTable();
    }
    if (slots.empty()) ImGui::TextDisabled("No save slots. A game saves with SaveGame.write(\"slot1\", data).");
    drawSelected();
}

void SavedDataWindow::drawSelected() {
    if (selected.empty()) return;
    ImGui::Separator();
    ImGui::Text("Slot \"%s\"", selected.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy as JSON")) ImGui::SetClipboardText(selectedJson.dump(2).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Delete Slot")) {
        const std::string slot = selected;
        if (run(SaveSlotCmd::Action::Delete, slot)) selected.clear();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Testing")) ImGui::OpenPopup("##SaveTesting");
    if (ImGui::BeginPopup("##SaveTesting")) {
        ImGui::TextDisabled("Corrupt Newest Generation");
        if (ImGui::MenuItem("Truncate")) run(SaveSlotCmd::Action::Corrupt, selected, 0, savegame::CorruptMode::Truncate);
        if (ImGui::MenuItem("Flip a Byte")) run(SaveSlotCmd::Action::Corrupt, selected, 0, savegame::CorruptMode::Flip);
        if (ImGui::MenuItem("Overwrite Header")) run(SaveSlotCmd::Action::Corrupt, selected, 0, savegame::CorruptMode::Header);
        ImGui::EndPopup();
    }
    if (!selectedProblem.empty()) ImGui::TextWrapped("%s", selectedProblem.c_str());

    if (ImGui::BeginChild("##SaveSlotDetail", ImVec2(0, 0))) {
        if (selectedJson.contains("namespaces") &&
            ImGui::BeginTable("##SaveTree", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();
            for (auto it = selectedJson["namespaces"].begin(); it != selectedJson["namespaces"].end(); ++it) drawValue(it.key(), it.value());
            ImGui::EndTable();
        }
        if (selectedJson.contains("meta")) ImGui::TextWrapped("Metadata: %s", selectedJson["meta"].dump().c_str());
        if (selectedJson.contains("requires")) ImGui::TextWrapped("Requires: %s", selectedJson["requires"].dump().c_str());

        ImGui::SeparatorText("Backups");
        if (ImGui::BeginTable("##SaveBackups", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Generation");
            ImGui::TableSetupColumn("Saved");
            ImGui::TableSetupColumn("Version");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const savegame::BackupInfo& backup : backups) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%llu", static_cast<unsigned long long>(backup.generation));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(timeText(backup.savedAt).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", backup.version);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(sizeText(backup.size).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(backup.valid ? "Valid" : "Corrupt");
                if (!backup.valid && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", backup.problem.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::PushID(static_cast<int>(backup.generation));
                if (ImGui::SmallButton("Restore Backup")) {
                    std::string error;
                    run(SaveSlotCmd::Action::Restore, selected, backup.generation, savegame::CorruptMode::Flip, &error);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void SavedDataWindow::drawPrefs() {
    if (ImGui::Button("Clear User Settings...")) {
        const size_t count = UserSettingsStore::instance().entries().size();
        Backend::getApp().registerConfirmAlert("Clear User Settings",
            "Delete every UserSettings / PlayerPrefs key of this project (" + std::to_string(count) + ")?\n" +
            UserSettingsStore::instance().getFilePath() + "\nThis cannot be undone.",
            []() { UserSettings::deleteAll(); UserSettings::save(); Out::info("Cleared the project's User Settings (PlayerPrefs)"); });
    }
    const std::map<std::string, std::string> entries = UserSettingsStore::instance().entries();
    if (ImGui::BeginTable("##PlayerPrefs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& entry : entries) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(entry.first.c_str());
            ImGui::TableSetColumnIndex(1);
            const std::string shown = entry.second.size() > 200 ? entry.second.substr(0, 200) + "..." : entry.second;
            ImGui::TextUnformatted(shown.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(entry.first.c_str());
            if (ImGui::SmallButton("Delete Key")) {
                UserSettings::deleteKey(entry.first.c_str());
                UserSettings::save();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (entries.empty()) ImGui::TextDisabled("No PlayerPrefs keys.");
}
