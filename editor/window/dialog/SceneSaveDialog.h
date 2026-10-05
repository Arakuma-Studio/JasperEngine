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

class SceneSaveDialog {
private:
    void displayDirectoryTree(const fs::path& currentPath);

    bool m_isOpen = false;
    fs::path m_projectPath;
    std::string m_selectedPath;
    char m_fileNameBuffer[256] = "";

    std::function<void(const fs::path&)> m_onSave;
    std::function<void()> m_onCancel;
    bool m_cancelRequested = false;

public:
    void open(const fs::path& projectPath, const fs::path& initialDirectory, const std::string& defaultName,
        std::function<void(const fs::path&)> onSave,
        std::function<void()> onCancel = nullptr);
    void show();
    bool isOpen() const { return m_isOpen; }
    void close() { m_isOpen = false; }
    // Presses Cancel on the next frame (MCP answering the dialog for an agent)
    void requestCancel() { if (m_isOpen) m_cancelRequested = true; }
};

} // namespace editor
} // namespace jasperengine