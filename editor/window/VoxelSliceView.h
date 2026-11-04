// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Dockable "Voxel Slice" (docs/specs/T6-voxel-volumes.md R63): one plane of the selected volume, drawn on
// the CPU by renderVoxelSlice, its plane shown as a gizmo in the Scene view. A click moves the Scene
// camera's pivot there.

#include "ecs/Entity.h"
#include "texture/Texture.h"
#include "voxel/VoxelSliceRender.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class VoxelSliceView {
    public:
        static constexpr const char* WINDOW_NAME = "Voxel Slice";

        explicit VoxelSliceView(Project* project);
        ~VoxelSliceView();

        void show();
        void open();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

        // Goes back to the Scene camera's pivot from an image position; used by the click
        static bool movePivotTo(Project* project, uint32_t sceneId, Entity owner, const VoxelSliceImage& image, float pixelX, float pixelY);

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        uint32_t sceneId = 0;
        Entity target = NULL_ENTITY;
        VoxelSliceAxis axis = VoxelSliceAxis::Y;
        int position = -1;
        bool blockGrid = true;
        uint64_t drawnVersion = 0;
        const void* drawnStore = nullptr;
        VoxelSliceAxis drawnAxis = VoxelSliceAxis::Y;
        int drawnPosition = -2;
        bool drawnGrid = true;
        VoxelSliceImage image;
        Texture texture;
        std::vector<Texture> retired;
        uint64_t textureCounter = 0;

        void redrawImage(voxel::Store& store, Entity owner);
        void retireTexture();
    };

}
