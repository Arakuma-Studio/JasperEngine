// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <filesystem>
#include <functional>
#include "imgui.h"

namespace jasperengine {
namespace editor {

    namespace fs = std::filesystem;

    class ProjectSaveDialog {
    private:
        bool m_isOpen = false;
        fs::path m_projectPath;
        char m_projectNameBuffer[256] = "";

        std::function<void(const std::string&, const fs::path&)> m_onSave;
        std::function<void()> m_onCancel;
        bool m_cancelRequested = false;

        // each ends the dialog: its closures are taken out before they run, none is kept
        void finishSave(const std::string& projectName, const fs::path& projectPath);
        void finishCancel();

    public:
        void open(const std::string& defaultName, 
            std::function<void(const std::string&, const fs::path&)> onSave,
            std::function<void()> onCancel = nullptr);
        void show();
        bool isOpen() const { return m_isOpen; }
        // As Cancel: onCancel runs (once)
        void close();
        // Ends the dialog without running either closure (the project it saved is closing)
        void abandon();
        // Presses Cancel on the next frame (MCP answering the dialog for an agent)
        void requestCancel() { if (m_isOpen) m_cancelRequested = true; }
    };

} // namespace editor
} // namespace jasperengine