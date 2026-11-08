// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R40, R41: Create Terrain World (GameObject > 3D Object > Terrain World) and the Terrain World
// panel: a minimap of the tiles by state with the focuses, Add Tile and Remove Tile by clicking cells,
// Rebuild Overview, Repair Seams, the streaming settings and a live info block. Drawn by the App, so
// they show whichever tab opened them.

#include "ecs/Entity.h"
#include "terrainworld/TerrainWorldEditor.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    class CreateTerrainWorldDialog {
    public:
        static CreateTerrainWorldDialog& instance();
        void open(Project* project, uint32_t sceneId, Entity parent);
        void show();
        bool opened() const { return isOpen; }

    private:
        bool isOpen = false;
        bool justOpened = false;
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity parent = NULL_ENTITY;
        TerrainWorldSettings settings;
        std::string error;
    };

    // R33, R40: Terrain inspector > Convert to Terrain World...: what the Terrain becomes, then the
    // resampling as a job; the Terrain stays, inactive
    class ConvertTerrainWorldDialog {
    public:
        static ConvertTerrainWorldDialog& instance();
        void open(Project* project, uint32_t sceneId, Entity source);
        void show();
        bool opened() const { return isOpen; }

    private:
        bool isOpen = false;
        bool justOpened = false;
        bool running = false;
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity source = NULL_ENTITY;
        std::string name;
        std::string error;
        std::function<void()> cancel;
    };

    class TerrainWorldPanel {
    public:
        static constexpr const char* WINDOW_NAME = "Terrain World";
        enum class Tool { Select, AddTile, RemoveTile };

        static TerrainWorldPanel& instance();
        void open(Project* project);
        void show();
        bool opened() const { return isOpen; }

        // R41's tooltip per state, shared with the overlay's legend
        static const char* stateHelp(int state);
        // Under a Terrain World's rows in the Inspector: why it does not start, and the panel's button
        static void drawInspectorNotes(Project* project, SceneProject* sceneProject);

    private:
        bool isOpen = false;
        Project* project = nullptr;
        Tool tool = Tool::Select;
        std::string message;
    };

}
