// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "input/InputActionAsset.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace jasperengine::editor {

    class Project;

    // Window > Input Actions (double-click an .inputactions asset): Unity's three panes, Action
    // Maps | Actions | Properties. Maps, actions, bindings and composites are added, renamed,
    // duplicated, deleted and dragged into another order; a binding's path comes from the path
    // picker or Listen (the next key, mouse button or gamepad control). A group filter shows one
    // control scheme. Every edit is one named undo step (InputActionsEditCmd) written atomically,
    // applied live during Play. The Debug tab (Play) shows each action's value, phase and
    // consumer, and the frame's raw edges of its controls.
    class InputActionsWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Input Actions";

        explicit InputActionsWindow(Project* project);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return focused; }

        // opens the asset (an absolute path) and the window
        void openFile(const std::filesystem::path& file);
        const std::filesystem::path& getFile() const { return file; }
        // the project closed: nothing stays open
        void reset();

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        bool focused = false;

        std::filesystem::path file;
        std::string text;                    // the file as last read
        InputActionAssetDesc asset;
        std::string loadError;
        uint64_t revision = UINT64_MAX;      // InputActionsEditCmd's when last read
        std::filesystem::file_time_type fileTime;
        double nextFileCheck = 0.0;

        // selection: a map, then an action of it ("Map/Action") or a binding (id)
        std::string selectedMap;
        std::string selectedAction;
        std::string selectedBinding;
        std::string groupFilter;             // "" all
        std::string status;
        int tab = 0;                         // 0 asset, 1 debug

        // inline renames
        std::string renamingMap;
        std::string renamingAction;
        char renameBuffer[128] = {};
        bool renameFocus = false;

        // Listen: the binding waiting for a control
        std::string listeningBinding;
        int listenFrames = 0;
        bool padButtonsBefore[16][15] = {};
        float padAxesBefore[16][6] = {};

        char pathFilter[64] = {};
        char virtualName[64] = {};

        void reload();
        void checkFile();
        // one edit as one undo step; false (and status) when it does not apply
        bool commit(const InputJson& op);

        void drawToolbar();
        void drawMaps();
        void drawActions();
        void drawProperties();
        void drawMapProperties(const InputActionMapDesc& map);
        void drawActionProperties(const InputActionDesc& action);
        void drawBindingProperties(const InputBindingDesc& binding, bool isPart);
        void drawPathField(const InputBindingDesc& binding);
        void drawPathPicker(const std::string& bindingId);
        void drawModifiers(const char* label, const char* key, const std::string& bindingId, int mods);
        void drawDebug();
        void updateListen();
        void finishListen(const std::string& path);
        void createAsset();
        bool visibleInFilter(const InputBindingDesc& binding) const;
    };

}
