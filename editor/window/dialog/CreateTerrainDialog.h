// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R58: Create Terrain (GameObject > 3D Object > Terrain, and the Terrain window's empty
// state). A preset of §6 Table 6 fills the sizes; Create makes the GameObject, its sizes, its
// height map and its collider as one undo step "Create Terrain (Name)". create_entity
// type=terrain still makes an empty terrain; both get applyNewTerrainDefaults (R91).

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor {

    class Project;

    class CreateTerrainDialog {
    public:
        enum class StartShape { Flat, FlatAtMiddle };

        struct Settings {
            std::string name = "Terrain";
            int preset = 2;             // Table 6's Small
            float size = 256.0f;
            float height = 64.0f;
            int heightmap = 512;
            int control = 512;
            int detail = 512;
            float meshResolution = 32.0f;
            int rootGrid = 2;
            int levels = 4;
            StartShape shape = StartShape::Flat;
            float baseHeight = 0.0f;    // metres, Flat
            bool collider = true;
        };

        static CreateTerrainDialog& instance();

        void open(Project* project, uint32_t sceneId, Entity parent);
        void show();
        bool opened() const { return isOpen; }

        // The undo step Create makes; the new GameObject, or NULL_ENTITY with error
        static Entity create(Project* project, uint32_t sceneId, Entity parent, const Settings& settings, std::string& error);
        // What the maps take on the CPU and GPU, and the platforms' warnings
        static std::string memoryLine(const Settings& settings);

    private:
        bool isOpen = false;
        bool justOpened = false;
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity parent = NULL_ENTITY;
        Settings settings;
        std::string error;

        void applyPreset(int index);
    };

}
