// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > General > Recorder (docs/specs/S27-editor-tools.md R24-R26): Unity Recorder's
// Recorder (Image Sequence / Movie), Capture (Game View), Output Resolution, Frame Rate,
// Recording Mode, Exit Play Mode, Record on Play, Output (folder and name with wildcards), Take,
// and for movies Format, Quality and Capture Audio. Its settings are the project's
// (.jasperengine/user/recorder.yaml). Start Recording / Stop Recording drive the one
// RecorderSession (as F10, the toolbar's record button and MCP record_game).

#include "recorder/RecorderSettings.h"

#include <filesystem>
#include <functional>
#include <string>

namespace jasperengine::editor {

    class RecorderWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Recorder";

        void show();
        void setOpen(bool open) { windowOpen = open; }
        bool isOpen() const { return windowOpen; }

        // the project's settings (read again when the project changes)
        void setProjectPath(const std::filesystem::path& projectPath);
        recorder::Settings& settings() { return current; }
        void saveSettings();

        // Start Recording / Stop Recording (the window's button, F10, the toolbar)
        void toggleRecording();
        // Preferences > External Tools (the "Open Preferences..." link)
        std::function<void()> openPreferences;

    private:
        bool windowOpen = false;
        std::filesystem::path projectPath;
        recorder::Settings current;
        recorder::Settings saved;
        uint64_t seenResult = 0;
        std::string message;
    };

}
