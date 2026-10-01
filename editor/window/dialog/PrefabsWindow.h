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

    // The project's prefabs by asset GUID: where each is used (instances per scene, closed
    // scenes read from their file, the prefabs nesting it), the Missing Prefabs scenes still
    // reference, and which prefabs with no scene instance the project builds and registers
    class PrefabsWindow {
    private:
        struct PrefabEntry {
            fs::path path;
            std::string guid;
            PrefabUsage usage;
            std::string usedBy;  // the scenes listing it in their build (empty: none)
            bool included = false;
        };

        bool m_isOpen = false;
        Project* m_project = nullptr;
        std::vector<PrefabEntry> m_entries;
        std::vector<PrefabUsage> m_missing;   // referenced GUIDs no file has
        char m_filter[128] = "";

        void drawPrefabs();
        void drawUsage(const PrefabUsage& usage, bool missing);
        void applyPrefabs();

    public:
        PrefabsWindow() = default;
        ~PrefabsWindow() = default;

        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
