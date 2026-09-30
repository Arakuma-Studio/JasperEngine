// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "imgui.h"

#include <string>
#include <filesystem>
#include <vector>

namespace jasperengine::editor {

    namespace fs = std::filesystem;

    // Chooses which prefabs with no scene instance the project builds and registers
    class PrefabsWindow {
    private:
        struct PrefabEntry {
            fs::path path;
            std::string usedBy;  // empty when no scene instantiates it
            bool included = false;
        };

        bool m_isOpen = false;
        Project* m_project = nullptr;
        std::vector<PrefabEntry> m_entries;

        void drawPrefabs();
        void applyPrefabs();

    public:
        PrefabsWindow() = default;
        ~PrefabsWindow() = default;

        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
