// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window "Voxel Volume" (docs/specs/T6-voxel-volumes.md R60-R64, R77): the brush's tools and shapes, the
// error strip, the Materials tab, overlays and stats, Save. The logic lives in editor/voxel/, so tests
// reach it without ImGui; this file only draws it.

#include "ecs/Entity.h"
#include "voxel/VoxelGenerationTask.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // R61: the red strip, in the panel and the Inspector, and the log lines once
    void drawVoxelProblemStrip(Project& project, const SceneProject& sceneProject, Entity owner);

    class VoxelEditWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Voxel Volume";

        explicit VoxelEditWindow(Project* project);

        // Every frame, open or not: the brush's and the overlays' tick, a running generation
        void show();
        void open();
        void openForEntity(Entity entity, uint32_t sceneId);
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

        // Regenerate and Re-seed live here so both the panel and the Inspector run the one task
        void startRegenerate(uint32_t sceneId, Entity owner);
        void startGenerate(uint32_t sceneId, Entity owner, const VoxelGenerateRequest& request, bool recordUndo);
        bool isGenerating() const { return generation.isRunning(); }

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        uint32_t sceneId = 0;
        Entity target = NULL_ENTITY;
        std::string status;
        bool statusIsError = false;
        VoxelGenerationTask generation;
        // the Materials tab
        int replaceWith = 0;
        int removingSlot = -1;
        // Re-seed From Terrain's region (R46)
        bool reseedWhole = true;
        int reseedMin[3] = {0, 0, 0};
        int reseedMax[3] = {64, 64, 64};
        // the Stamp shape's source
        std::string stampPath;
        Entity meshSource = NULL_ENTITY;

        void updateTarget();
        void setStatus(const std::string& text, bool isError);
        void drawBrushTab(SceneProject& sceneProject);
        void drawMaterialsTab(SceneProject& sceneProject);
        void drawOverlaysTab(SceneProject& sceneProject);
        void drawVolumeTab(SceneProject& sceneProject);
        void drawInfoStrip();
        void drawSlotRow(SceneProject& sceneProject, int index);
    };

}
