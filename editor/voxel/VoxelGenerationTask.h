// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Regenerate, Create's slow bases and the panel's progress bar over VoxelGeneration's three steps
// (docs/specs/T6-voxel-volumes.md R33, R59, R65, R71). One running per volume; the one undo step is
// made when it finishes.

#include "ecs/Entity.h"
#include "voxel/VoxelGeneration.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace jasperengine::editor {

    class Project;

    class VoxelGenerationTask {
    public:
        // False, leaving nothing running, with what the steps said (a code and what to do). recordUndo
        // false: it belongs to the step that made the volume, so it adds none of its own.
        bool start(Project* project, uint32_t sceneId, Entity owner, const VoxelGenerateRequest& request, bool recordUndo, std::string& error);
        // Main thread, each frame: true once it finished, and `result` says how
        bool poll(VoxelGenerateResult& result);
        void cancel() { cancelled.store(true); }
        bool isRunning() const { return running; }
        float progress() const { return progressValue.load(); }
        Entity owner() const { return target; }

        ~VoxelGenerationTask();

    private:
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity target = NULL_ENTITY;
        bool running = false;
        bool record = true;
        std::shared_ptr<VoxelGeneration> steps;
        std::thread worker;
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        std::atomic<float> progressValue{0.0f};
    };

}
