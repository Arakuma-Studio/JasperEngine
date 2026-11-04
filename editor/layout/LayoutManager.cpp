// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LayoutManager.h"

#include "AppSettings.h"
#include "Out.h"
#include "Workspace.h"
#include "util/FileDialogs.h"
#include "util/UserFiles.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "window/EditorModal.h"

#include <algorithm>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    bool sameName(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    }

    bool isDefault(const std::string& name) {
        return sameName(name, LayoutPresets::DEFAULT_NAME);
    }

    std::string trimmedName(const std::string& text) {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        return text.substr(begin, end - begin);
    }

    fs::path layoutsLock() {
        return AppSettings::getConfigDirectory() / "layouts.lock";
    }

}

void LayoutManager::setHost(Host value) {
    host = std::move(value);
}

fs::path LayoutManager::presetsDirectory() {
    return AppSettings::getConfigDirectory() / "layouts";
}

fs::path LayoutManager::globalIniFile() {
    return AppSettings::getConfigDirectory() / "editor_layout_v2.ini";
}

void LayoutManager::loadGlobalIni() {
    std::string text;
    if (userfiles::read(globalIniFile(), text) && !text.empty()) {
        ImGui::LoadIniSettingsFromMemory(text.data(), text.size());
    }
}

fs::path LayoutManager::workingLayoutFile() const {
    const fs::path projectPath = host.projectPath ? host.projectPath() : fs::path();
    if (projectPath.empty()) return {};
    return Workspace::getUserDirectory(projectPath) / "layout.yaml";
}

int LayoutManager::dockNodeCount() const {
    return ImGui::GetCurrentContext() ? ImGui::GetCurrentContext()->DockContext.Nodes.Data.Size : 0;
}

std::string LayoutManager::currentIni() const {
    size_t size = 0;
    const char* data = ImGui::SaveIniSettingsToMemory(&size);
    return LayoutPresets::filterDocumentWindows(std::string(data, size));
}

LayoutPreset LayoutManager::currentLayout(const std::string& name) const {
    LayoutPreset preset;
    preset.name = name;
    preset.uiScale = host.currentUiScale ? host.currentUiScale() : 1.0f;
    if (host.panelStates) preset.panels = host.panelStates();
    preset.ini = currentIni();
    preset.activePreset = active;
    return preset;
}

const std::vector<LayoutManager::CachedPreset>& LayoutManager::cachedPresets() const {
    const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    const fs::path dir = presetsDirectory();
    if (cacheValid && now - cacheCheckTime < 1.0) return cache;
    cacheCheckTime = now;
    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(dir, ec);
    if (cacheValid && !ec && stamp == cacheStamp) return cache;
    cacheStamp = ec ? fs::file_time_type{} : stamp;
    cache.clear();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string fileName = it->path().filename().string();
        if (fileName.size() <= std::string(LayoutPresets::FILE_SUFFIX).size() ||
            !fileName.ends_with(LayoutPresets::FILE_SUFFIX)) continue;
        std::string text;
        LayoutPreset preset;
        if (!userfiles::read(it->path(), text) || !LayoutPresets::parse(text, preset) || preset.name.empty()) {
            // a file that does not parse still lists, by its file name: applying it warns
            preset.name = fileName.substr(0, fileName.size() - std::string(LayoutPresets::FILE_SUFFIX).size());
        }
        if (isDefault(preset.name)) continue;
        cache.push_back({preset.name, it->path()});
    }
    std::sort(cache.begin(), cache.end(), [](const CachedPreset& a, const CachedPreset& b) { return a.name < b.name; });
    cacheValid = true;
    return cache;
}

std::vector<LayoutPresetInfo> LayoutManager::presets() const {
    std::vector<LayoutPresetInfo> out;
    out.push_back({LayoutPresets::DEFAULT_NAME, true, isDefault(active)});
    for (const CachedPreset& preset : cachedPresets()) out.push_back({preset.name, false, sameName(preset.name, active)});
    return out;
}

fs::path LayoutManager::presetFile(const std::string& name) const {
    for (const CachedPreset& preset : cachedPresets()) {
        if (sameName(preset.name, name)) return preset.file;
    }
    return {};
}

bool LayoutManager::loadPreset(const std::string& name, LayoutPreset& preset, std::string* error) const {
    const fs::path file = presetFile(name);
    if (file.empty()) {
        if (error) *error = "No layout named '" + name + "'.";
        return false;
    }
    std::string text;
    if (!userfiles::read(file, text)) {
        if (error) *error = "Could not read " + file.generic_string() + ".";
        return false;
    }
    std::string why;
    if (!LayoutPresets::parse(text, preset, &why)) {
        if (error) *error = file.generic_string() + " is not a layout (" + why + ").";
        return false;
    }
    preset.name = name;
    return true;
}

std::vector<LayoutPreset> LayoutManager::userPresets() const {
    std::vector<LayoutPreset> out;
    for (const CachedPreset& cached : cachedPresets()) {
        LayoutPreset preset;
        if (loadPreset(cached.name, preset, nullptr)) out.push_back(std::move(preset));
    }
    return out;
}

bool LayoutManager::isDirty() const {
    if (active.empty() || snapshotFrames >= 0 || pending || postApply) return false;
    return currentIni() != snapshot;
}

void LayoutManager::schedule(const LayoutPreset& preset, const std::string& name) {
    pending = true;
    pendingSeed = false;
    pendingDefault = false;
    pendingPreset = preset;
    pendingName = name;
}

void LayoutManager::scheduleDefault() {
    pending = true;
    pendingSeed = false;
    pendingDefault = true;
    pendingPreset = {};
    pendingName = LayoutPresets::DEFAULT_NAME;
}

void LayoutManager::beforeFrame() {
    // a project opened (or the editor started on one): its working layout
    const fs::path projectPath = host.projectPath ? host.projectPath() : fs::path();
    if (!bound || projectPath != boundProject) {
        bound = true;
        boundProject = projectPath;
        if (!projectPath.empty()) {
            const fs::path file = workingLayoutFile();
            std::string text;
            std::error_code ec;
            if (fs::exists(file, ec) && userfiles::read(file, text)) {
                LayoutPreset working;
                std::string why;
                if (LayoutPresets::parse(text, working, &why)) {
                    schedule(working, working.activePreset);
                } else {
                    Out::warning("Layout: %s could not be read (%s); the Default layout is used.", file.generic_string().c_str(), why.c_str());
                    scheduleDefault();
                }
            } else {
                // the first time: the global layout (Default without one), written for the
                // project one frame later
                std::string global;
                if (userfiles::read(globalIniFile(), global) && !global.empty()) {
                    LayoutPreset seeded = currentLayout("");
                    seeded.ini = LayoutPresets::filterDocumentWindows(global);
                    schedule(seeded, active);
                    pendingSeed = true;
                } else {
                    scheduleDefault();
                }
                writeWorkingAfterSnapshot = true;
            }
        }
    }

    if (!pending) return;
    pending = false;
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context) return;
    if (pendingDefault) {
        postApply = true;
        postApplyDefault = true;
        active = pendingName;
        return;
    }
    ImGui::ClearIniSettings();
    ImGui::LoadIniSettingsFromMemory(pendingPreset.ini.data(), pendingPreset.ini.size());
    postApply = true;
    postApplyDefault = false;
    postSeed = pendingSeed;
    pendingSeed = false;
    postPreset = pendingPreset;
    active = pendingName;
}

void LayoutManager::inFrame() {
    if (!postApply) return;
    postApply = false;
    if (postApplyDefault) {
        if (host.applyDefault) host.applyDefault();
    } else {
        if (host.applyPanels && !postPreset.panels.empty()) host.applyPanels(postPreset.panels);
        if (host.afterIniLoaded && !host.afterIniLoaded(postPreset.uiScale)) {
            // the editor's own global file without a dock tree yet (a first start) is no news
            if (!postSeed) Out::warning("Layout '%s' has no dock layout for this editor window; the Default layout is used.",
                         (postPreset.name.empty() ? std::string("working layout") : postPreset.name).c_str());
            active = LayoutPresets::DEFAULT_NAME;
            if (host.applyDefault) host.applyDefault();
        }
    }
    // one frame later the windows sit where the layout put them
    snapshotFrames = 1;
}

void LayoutManager::endFrame() {
    if (snapshotFrames >= 0) {
        if (snapshotFrames-- == 0) {
            snapshot = currentIni();
            if (writeWorkingAfterSnapshot) {
                writeWorkingAfterSnapshot = false;
                writeWorkingLayout();
            }
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantSaveIniSettings) return;
    const double now = ImGui::GetTime();
    if (lastSaveTime >= 0.0 && now - lastSaveTime < 1.0) return;
    lastSaveTime = now;
    io.WantSaveIniSettings = false;
    if (workingLayoutFile().empty()) writeGlobalIni();
    else writeWorkingLayout();
}

void LayoutManager::saveWorkingLayoutNow() {
    if (!bound || pending || postApply) return;
    if (boundProject.empty()) writeGlobalIni();
    else writeWorkingLayout();
}

bool LayoutManager::writeWorkingLayout(std::string* error) {
    if (boundProject.empty() || !host.projectPath || host.projectPath() != boundProject) return false;
    const fs::path file = workingLayoutFile();
    std::string why;
    if (!userfiles::write(file, LayoutPresets::serialize(currentLayout(active.empty() ? std::string("Working Layout") : active), true), &why)) {
        Out::warning("Layout: %s", why.c_str());
        if (error) *error = why;
        return false;
    }
    return true;
}

void LayoutManager::writeGlobalIni() {
    size_t size = 0;
    const char* data = ImGui::SaveIniSettingsToMemory(&size);
    std::string why;
    if (!userfiles::write(globalIniFile(), std::string(data, size), &why)) Out::warning("Layout: %s", why.c_str());
}

void LayoutManager::requestSwitch(const std::string& name) {
    const bool leavingDirty = !active.empty() && isDirty() && !sameName(name, active);
    if (!leavingDirty || !host.promptUnsaved) {
        std::string error;
        if (!apply(name, &error) && host.alert) host.alert("Layout", error);
        return;
    }
    const std::string current = active;
    auto switchTo = [this, name]() {
        std::string error;
        if (!apply(name, &error) && host.alert) host.alert("Layout", error);
    };
    host.promptUnsaved("Layout '" + current + "' has unsaved changes",
        isDefault(current) ? "Default cannot be overwritten: Save keeps the changes as a new layout."
                           : "Save the window layout as '" + current + "' before switching?",
        [this, current, switchTo]() {
            if (isDefault(current)) {
                // Save As...: the name dialog, then the switch is up to the user
                openSaveDialog();
                return;
            }
            std::string error;
            if (!save(current, &error) && host.alert) {
                host.alert("Layout", error);
                return;
            }
            switchTo();
        },
        switchTo);
}

bool LayoutManager::apply(const std::string& name, std::string* error) {
    if (isDefault(name)) {
        scheduleDefault();
        return true;
    }
    LayoutPreset preset;
    std::string why;
    if (!loadPreset(name, preset, &why)) {
        if (presetFile(name).empty()) {
            if (error) *error = why;
            return false;
        }
        // listed but unreadable: Default, with the reason (R17)
        Out::warning("Layout '%s': %s The Default layout is used.", name.c_str(), why.c_str());
        scheduleDefault();
        return true;
    }
    schedule(preset, preset.name);
    return true;
}

bool LayoutManager::save(const std::string& rawName, std::string* error, std::vector<fs::path>* written) {
    const std::string name = trimmedName(rawName);
    std::string why;
    if (!LayoutPresets::isValidName(name, &why)) {
        if (error) *error = why;
        return false;
    }
    fs::path file = presetFile(name);
    std::string stored = name;
    if (file.empty()) {
        std::vector<std::string> existing;
        for (const CachedPreset& preset : cachedPresets()) existing.push_back(preset.file.filename().string());
        std::string fileName = LayoutPresets::slug(name) + LayoutPresets::FILE_SUFFIX;
        for (int n = 2; std::find(existing.begin(), existing.end(), fileName) != existing.end(); n++) {
            fileName = LayoutPresets::slug(name) + "-" + std::to_string(n) + LayoutPresets::FILE_SUFFIX;
        }
        file = presetsDirectory() / fileName;
    } else {
        // the existing preset keeps its spelling
        for (const CachedPreset& preset : cachedPresets()) {
            if (sameName(preset.name, name)) stored = preset.name;
        }
    }
    LayoutPreset preset = currentLayout(stored);
    if (!userfiles::write(file, LayoutPresets::serialize(preset, false), &why, layoutsLock())) {
        if (error) *error = why;
        return false;
    }
    if (written) written->push_back(file);
    invalidateCache();
    active = stored;
    snapshot = preset.ini;
    snapshotFrames = -1;
    writeWorkingLayout();
    return true;
}

bool LayoutManager::rename(const std::string& name, const std::string& rawNewName, std::string* error, std::vector<fs::path>* written) {
    const std::string newName = trimmedName(rawNewName);
    if (isDefault(name)) {
        if (error) *error = "Default is the built-in layout: it cannot be renamed.";
        return false;
    }
    std::string why;
    if (!LayoutPresets::isValidName(newName, &why)) {
        if (error) *error = why;
        return false;
    }
    LayoutPreset preset;
    if (!loadPreset(name, preset, error)) return false;
    if (!sameName(name, newName) && !presetFile(newName).empty()) {
        if (error) *error = "A layout named '" + newName + "' exists.";
        return false;
    }
    const fs::path oldFile = presetFile(name);
    preset.name = newName;
    const fs::path file = presetsDirectory() / (LayoutPresets::slug(newName) + LayoutPresets::FILE_SUFFIX);
    if (!userfiles::write(file, LayoutPresets::serialize(preset, false), &why, layoutsLock())) {
        if (error) *error = why;
        return false;
    }
    if (file != oldFile && !userfiles::remove(oldFile, &why, layoutsLock())) {
        if (error) *error = why;
        return false;
    }
    if (written) written->push_back(file);
    invalidateCache();
    if (sameName(active, name)) active = newName;
    return true;
}

bool LayoutManager::remove(const std::string& name, std::string* error) {
    if (isDefault(name)) {
        if (error) *error = "Default is the built-in layout: it cannot be deleted.";
        return false;
    }
    const fs::path file = presetFile(name);
    if (file.empty()) {
        if (error) *error = "No layout named '" + name + "'.";
        return false;
    }
    std::string why;
    if (!userfiles::remove(file, &why, layoutsLock())) {
        if (error) *error = why;
        return false;
    }
    invalidateCache();
    // the windows stay as they are; no preset is checked any more
    if (sameName(active, name)) active.clear();
    return true;
}

bool LayoutManager::exportTo(const fs::path& file, const std::string& name, std::string* error) {
    std::string text;
    if (name.empty()) {
        const std::vector<LayoutPreset> all = userPresets();
        if (all.empty()) {
            if (error) *error = "There are no saved layouts to export.";
            return false;
        }
        text = LayoutPresets::serializeBundle(all);
    } else if (isDefault(name)) {
        text = LayoutPresets::serialize(currentLayout(LayoutPresets::DEFAULT_NAME), false);
    } else {
        LayoutPreset preset;
        if (!loadPreset(name, preset, error)) return false;
        text = LayoutPresets::serialize(preset, false);
    }
    std::string why;
    if (!userfiles::write(file, text, &why)) {
        if (error) *error = why;
        return false;
    }
    return true;
}

bool LayoutManager::importFrom(const fs::path& file, std::vector<std::string>* names, std::string* error, std::vector<fs::path>* written) {
    std::string text;
    if (!userfiles::read(file, text)) {
        if (error) *error = "Could not read " + file.generic_string() + ".";
        return false;
    }
    std::vector<LayoutPreset> imported;
    std::string why;
    if (!LayoutPresets::parseFile(text, imported, &why)) {
        if (error) *error = file.generic_string() + " is not a layout file (" + why + ").";
        return false;
    }
    std::vector<std::string> existing{LayoutPresets::DEFAULT_NAME};
    for (const CachedPreset& preset : cachedPresets()) existing.push_back(preset.name);
    for (LayoutPreset& preset : imported) {
        preset.name = LayoutPresets::uniqueName(preset.name.empty() ? file.stem().stem().string() : preset.name, existing);
        existing.push_back(preset.name);
        std::string fileName = LayoutPresets::slug(preset.name) + LayoutPresets::FILE_SUFFIX;
        std::error_code ec;
        for (int n = 2; fs::exists(presetsDirectory() / fileName, ec); n++) {
            fileName = LayoutPresets::slug(preset.name) + "-" + std::to_string(n) + LayoutPresets::FILE_SUFFIX;
        }
        const fs::path target = presetsDirectory() / fileName;
        if (!userfiles::write(target, LayoutPresets::serialize(preset, false), &why, layoutsLock())) {
            if (error) *error = why;
            return false;
        }
        if (written) written->push_back(target);
        if (names) names->push_back(preset.name);
        invalidateCache();
    }
    return true;
}

void LayoutManager::openSaveDialog() {
    saveDialogRequested = true;
    const std::string suggestion = active.empty() || isDefault(active) ? std::string("New Layout") : active;
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", suggestion.c_str());
}

void LayoutManager::openRenameDialog(const std::string& name) {
    renameDialogRequested = true;
    dialogTarget = name;
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", name.c_str());
}

void LayoutManager::confirmDelete(const std::string& name) {
    if (!host.confirm) return;
    host.confirm("Delete Layout", "Delete the layout '" + name + "'? The windows stay as they are.", [this, name]() {
        std::string error;
        if (!remove(name, &error) && host.alert) host.alert("Delete Layout", error);
    });
}

void LayoutManager::exportToFile() {
    exportDialogRequested = true;
    exportChoice = 0;
}

void LayoutManager::importFromFile() {
    const std::string path = FileDialogs::openFileDialogFiltered(presetsDirectory().string(), "Layouts", "yaml");
    if (path.empty()) return;
    std::vector<std::string> names;
    std::string error;
    if (!importFrom(path, &names, &error)) {
        if (host.alert) host.alert("Load Layout from File", error);
        return;
    }
    std::string list;
    for (const std::string& name : names) list += (list.empty() ? "" : ", ") + name;
    Out::info("Loaded layout%s: %s", names.size() == 1 ? "" : "s", list.c_str());
}

void LayoutManager::drawDialogs() {
    const char* savePopup = "Save Layout##LayoutSaveModal";
    const char* renamePopup = "Rename Layout##LayoutRenameModal";
    const char* exportPopup = "Save Layout to File##LayoutExportModal";
    if (saveDialogRequested) {
        saveDialogRequested = false;
        ImGui::OpenPopup(savePopup);
    }
    if (renameDialogRequested) {
        renameDialogRequested = false;
        ImGui::OpenPopup(renamePopup);
    }
    if (exportDialogRequested) {
        exportDialogRequested = false;
        ImGui::OpenPopup(exportPopup);
    }

    auto nameDialog = [this](const char* popup, const char* prompt, const char* button, const std::function<bool(const std::string&, std::string&)>& accept) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!EditorModal::begin(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
        ImGui::TextUnformatted(prompt);
        EditorModal::message(prompt);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        const bool enter = EditorModal::inputText("##LayoutName", "name", nameBuffer, sizeof(nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        std::string why;
        const bool valid = LayoutPresets::isValidName(nameBuffer, &why);
        if (!valid) ImGui::TextDisabled("%s", why.c_str());
        EditorModal::error(valid ? std::string() : why);
        ImGui::Separator();
        ImGui::BeginDisabled(!valid);
        const bool pressed = EditorModal::button(button, "ok", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = EditorModal::button("Cancel", "cancel", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if ((pressed || enter) && valid) {
            std::string error;
            if (accept(nameBuffer, error)) {
                ImGui::CloseCurrentPopup();
            } else if (host.alert) {
                ImGui::CloseCurrentPopup();
                host.alert("Layout", error);
            }
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    };
    nameDialog(savePopup, "Save the window layout as:", "Save", [this](const std::string& name, std::string& error) {
        return save(name, &error);
    });
    nameDialog(renamePopup, ("Rename '" + dialogTarget + "' to:").c_str(), "Rename", [this](const std::string& name, std::string& error) {
        return rename(dialogTarget, name, &error);
    });

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (EditorModal::begin(exportPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const std::vector<LayoutPresetInfo> list = presets();
        ImGui::TextUnformatted("Layout to save:");
        EditorModal::message("Layout to save; ok asks where (a file picker).");
        std::vector<std::string> exportOptions;
        for (const LayoutPresetInfo& info : list) exportOptions.push_back(info.name);
        if (list.size() > 1) exportOptions.push_back("Every saved layout");
        EditorModal::enumField("layout", exportChoice, exportOptions);
        for (int i = 0; i < static_cast<int>(list.size()); i++) {
            ImGui::RadioButton(list[i].name.c_str(), &exportChoice, i);
        }
        const bool anyUser = list.size() > 1;
        ImGui::BeginDisabled(!anyUser);
        ImGui::RadioButton("Every saved layout (one bundle file)", &exportChoice, static_cast<int>(list.size()));
        ImGui::EndDisabled();
        if (!anyUser) ImGui::SetItemTooltip("Save a layout first (Window > Layouts > Save Layout...)");
        ImGui::Separator();
        const bool pressed = EditorModal::button("Save...", "ok", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f));
        ImGui::SameLine();
        const bool cancel = EditorModal::button("Cancel", "cancel", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (pressed) {
            const bool all = exportChoice >= static_cast<int>(list.size());
            const std::string name = all ? std::string() : list[std::clamp(exportChoice, 0, static_cast<int>(list.size()) - 1)].name;
            const std::string defaultName = (all ? std::string("layouts") : LayoutPresets::slug(name)) + LayoutPresets::FILE_SUFFIX;
            ImGui::CloseCurrentPopup();
            const std::string path = FileDialogs::saveFileDialog(presetsDirectory().string(), defaultName, "Layouts", "yaml");
            if (!path.empty()) {
                std::string error;
                if (!exportTo(path, name, &error) && host.alert) host.alert("Save Layout to File", error);
                else Out::info("Saved %s to %s", all ? "every layout" : ("layout '" + name + "'").c_str(), path.c_str());
            }
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
