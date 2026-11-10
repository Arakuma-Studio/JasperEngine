// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// GameObject > 3D Object > Voxel Volume (docs/specs/T6-voxel-volumes.md R59): the dialog over
// voxel/VoxelCreate.h. Main thread; App draws it every frame.

#include "ecs/Entity.h"
#include "voxel/VoxelCreate.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor {

    class Project;

    class VoxelCreateDialog {
    public:
        static VoxelCreateDialog& instance();

        void open(Project* project, uint32_t sceneId, Entity parent);
        void draw();

    private:
        bool isOpen = false;
        bool justOpened = false;
        Project* project = nullptr;
        uint32_t sceneId = 0;
        VoxelCreateOptions options;
        std::string error;
    };

}
