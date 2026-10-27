// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Named window layouts and the project's working layout (S25 R15-R19): Window > Layouts, the
// toolbar's Layout menu, Save/Rename/Delete Layout, Save Layout to File / Load Layout from File,
// and the MCP edit_layout_presets tool all go through here.
//
// Presets live in <configDir>/layouts/<slug>.layout.yaml, for every project (Unity). Default is
// built in (App::buildDefaultLayout) and cannot be renamed or deleted. ImGui's own ini file is
// off (io.IniFilename = nullptr): the editor keeps the working layout of each project in
// <project>/.jasperengine/user/layout.yaml, written when ImGui asks to save its settings (at most
// once a second) and when the project closes, and seeded from the global editor_layout_v2.ini the
// first time a project opens without one. Without a saved project that global file is the
// working layout.
//
// A preset is applied between frames, before NewFrame (beforeFrame): ClearIniSettings and
// LoadIniSettingsFromMemory. In the next frame (inFrame, before the dock space) the panels open
// or close, the dock tree's central node is recovered, the sizes follow the UI scale and the
// open documents dock into the centre. A layout whose dock root is missing, or a file that does
// not parse, falls back to Default with a Console warning. "Dirty" compares the ini with the
// snapshot taken one frame after the preset was applied or saved.

#include "LayoutPresets.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct LayoutPresetInfo {
        std::string name;
        bool builtin = false;
        bool active = false;
    };

    class LayoutManager {
    public:
        // What the App provides
        struct Host {
            std::function<std::map<std::string, bool>()> panelStates;
            std::function<void(const std::map<std::string, bool>&)> applyPanels;
            // Default: the panels it opens and buildDockspace(true), in the frame
            std::function<void()> applyDefault;
            // After an ini was loaded, in the frame: false when the dock root is missing; else
            // the central node is recovered, sizes follow uiScale and documents dock (true)
            std::function<bool(float uiScale)> afterIniLoaded;
            std::function<float()> currentUiScale;
            // The open project's root, empty without a saved one (the global ini is used then)
            std::function<std::filesystem::path()> projectPath;
            // "Layout 'X' has unsaved changes": Save / Don't Save / Cancel
            std::function<void(const std::string& title, const std::string& message, std::function<void()> onSave,
                               std::function<void()> onDontSave)> promptUnsaved;
            std::function<void(const std::string& title, const std::string& message, std::function<void()> onYes)> confirm;
            std::function<void(const std::string& title, const std::string& message)> alert;
        };

        void setHost(Host host);
        // setup(): the global ini, as ImGui would have read it at the first frame
        void loadGlobalIni();

        // Between frames: a pending apply, and the working layout of a project that just opened
        void beforeFrame();
        // In App::show, before the dock space: the post-load steps
        void inFrame();
        // After the windows: the dirty snapshot, and the working layout saved when ImGui asks
        void endFrame();
        // The project is closing (a switch, quitting): its working layout is written now
        void saveWorkingLayoutNow();

        std::vector<LayoutPresetInfo> presets() const;
        const std::string& activePreset() const { return active; }
        bool isDirty() const;
        // The working layout file of the open project (empty without one)
        std::filesystem::path workingLayoutFile() const;
        static std::filesystem::path presetsDirectory();
        // Dock nodes alive (the spike's leak check: re-applying must not grow it)
        int dockNodeCount() const;

        // The menus: switching away from a modified user preset asks first; re-picking the
        // active one reloads it
        void requestSwitch(const std::string& name);
        // Agents and the prompt's answers: no questions. False with *error
        bool apply(const std::string& name, std::string* error = nullptr);
        // Saves the current layout as `name` (overwrites a user preset of that name)
        bool save(const std::string& name, std::string* error = nullptr, std::vector<std::filesystem::path>* written = nullptr);
        bool rename(const std::string& name, const std::string& newName, std::string* error = nullptr,
                    std::vector<std::filesystem::path>* written = nullptr);
        bool remove(const std::string& name, std::string* error = nullptr);
        // Writes one preset ("" = every user preset, as a bundle) to a file
        bool exportTo(const std::filesystem::path& file, const std::string& name, std::string* error = nullptr);
        // Adds the presets of a preset or bundle file, renamed " (2)"... when taken; their names
        bool importFrom(const std::filesystem::path& file, std::vector<std::string>* names, std::string* error = nullptr,
                        std::vector<std::filesystem::path>* written = nullptr);

        // Menu items that open a dialog
        void openSaveDialog();
        void openRenameDialog(const std::string& name);
        void confirmDelete(const std::string& name);
        void exportToFile();
        void importFromFile();
        // The dialogs (call in the frame)
        void drawDialogs();

    private:
        Host host;
        std::string active;                // "", "Default" or a user preset
        std::filesystem::path boundProject;
        bool bound = false;
        // the next apply
        bool pending = false;
        bool pendingDefault = false;
        LayoutPreset pendingPreset;
        std::string pendingName;
        bool pendingSeed = false;          // the global ini seeding a project's first layout
        bool postSeed = false;
        bool postApply = false;            // inFrame has the post-load steps to do
        bool postApplyDefault = false;
        LayoutPreset postPreset;
        int snapshotFrames = -1;           // frames left before the dirty snapshot
        std::string snapshot;
        bool writeWorkingAfterSnapshot = false;
        double lastSaveTime = -1.0;

        // dialogs
        bool saveDialogRequested = false;
        bool renameDialogRequested = false;
        bool exportDialogRequested = false;
        std::string dialogTarget;
        char nameBuffer[96] = "";
        int exportChoice = 0;

        // the user presets on disk (name, file), re-read when the folder changes (checked at
        // most once a second: the menus ask every frame)
        struct CachedPreset {
            std::string name;
            std::filesystem::path file;
        };
        mutable std::vector<CachedPreset> cache;
        mutable bool cacheValid = false;
        mutable double cacheCheckTime = -1.0;
        mutable std::filesystem::file_time_type cacheStamp{};
        const std::vector<CachedPreset>& cachedPresets() const;
        void invalidateCache() { cacheValid = false; }

        std::string currentIni() const;
        LayoutPreset currentLayout(const std::string& name) const;
        bool loadPreset(const std::string& name, LayoutPreset& preset, std::string* error) const;
        std::filesystem::path presetFile(const std::string& name) const;
        std::vector<LayoutPreset> userPresets() const;
        void schedule(const LayoutPreset& preset, const std::string& name);
        void scheduleDefault();
        bool writeWorkingLayout(std::string* error = nullptr);
        void writeGlobalIni();
        static std::filesystem::path globalIniFile();
    };

}
