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
    // why the file named cannot be this scene's ("" = it can): the project's own rules
    std::function<std::string(const fs::path&)> m_pathProblem;
    bool m_cancelRequested = false;

    // each ends the dialog: its closures are taken out before they run (they may open it again)
    // and none is kept
    void finishSave(const fs::path& fullPath);
    void finishCancel();

public:
    void open(const fs::path& projectPath, const fs::path& initialDirectory, const std::string& defaultName,
        std::function<void(const fs::path&)> onSave,
        std::function<void()> onCancel = nullptr,
        std::function<std::string(const fs::path&)> pathProblem = nullptr);
    void show();
    bool isOpen() const { return m_isOpen; }
    // As Cancel: onCancel runs (once)
    void close();
    // Ends the dialog without running either closure (the project it saved into is closing)
    void abandon();
    // Presses Cancel on the next frame (MCP answering the dialog for an agent)
    void requestCancel() { if (m_isOpen) m_cancelRequested = true; }

    // The file a typed name gives inside `folder` (".scene" appended unless it ends so)
    static std::string withSceneExtension(const std::string& name);
    // Why `name` cannot be saved in `folder` of the project ("" = it can): empty, a path (/, \, ..,
    // a drive), or a name Windows refuses (projects move between systems), then the project's
    // own rule for the file (another scene's, outside the project, a folder)
    static std::string nameProblem(const fs::path& projectPath, const fs::path& folder, const std::string& name,
        const std::function<std::string(const fs::path&)>& pathProblem);
};

} // namespace editor
} // namespace jasperengine