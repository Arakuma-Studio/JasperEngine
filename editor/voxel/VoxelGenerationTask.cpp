// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelGenerationTask.h"

#include "Project.h"
#include "command/Command.h"
#include "command/CommandHandle.h"
#include "subsystem/VoxelSystem.h"

#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // R71: one undo step, the old base description and the block references it replaced
    class VoxelGenerateCmd : public Command {
    public:
        VoxelGenerateCmd(Project* project, uint32_t sceneId, Entity owner, VoxelGenerateUndo state):
            project(project), sceneId(sceneId), owner(owner), state(std::move(state)) {}

        bool execute() override {
            // the steps have applied it: the first execute only records
            if (first) {
                first = false;
                return true;
            }
            VoxelGeneration::applyUndo(sceneOf(), owner, state, false);
            return true;
        }
        void undo() override { VoxelGeneration::applyUndo(sceneOf(), owner, state, true); }
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return "Regenerate Voxel Volume"; }
        const char* typeName() const override { return "VoxelGenerateCmd"; }
        size_t memoryBytes() const override { return sizeof(*this) + state.slots.size() * sizeof(voxel::SlotChange); }

    private:
        Scene* sceneOf() const {
            SceneProject* sceneProject = project->getScene(sceneId);
            return sceneProject ? sceneProject->scene : nullptr;
        }

        Project* project;
        uint32_t sceneId;
        Entity owner;
        VoxelGenerateUndo state;
        bool first = true;
    };

}

VoxelGenerationTask::~VoxelGenerationTask() {
    cancelled.store(true);
    if (worker.joinable()) worker.join();
}

bool VoxelGenerationTask::start(Project* targetProject, uint32_t targetScene, Entity owner, const VoxelGenerateRequest& request, bool recordUndo, std::string& error) {
    if (running) {
        error = "A generation is already running on this volume.";
        return false;
    }
    SceneProject* sceneProject = targetProject ? targetProject->getScene(targetScene) : nullptr;
    if (!sceneProject || !sceneProject->scene) {
        error = "No scene is open.";
        return false;
    }
    VoxelGenerateResult result;
    steps = VoxelGeneration::prepare(sceneProject->scene, owner, request, result);
    if (!steps || !result.ok) {
        error = result.message.empty() ? result.error : result.message;
        steps.reset();
        return false;
    }
    project = targetProject;
    sceneId = targetScene;
    target = owner;
    running = true;
    record = recordUndo;
    done.store(false);
    cancelled.store(false);
    progressValue.store(0.0f);
    worker = std::thread([this]() {
        steps->run([this]() { return cancelled.load(); }, [this](float value) { progressValue.store(value); });
        done.store(true);
    });
    return true;
}

bool VoxelGenerationTask::poll(VoxelGenerateResult& result) {
    if (!running || !done.load()) return false;
    worker.join();
    running = false;
    SceneProject* sceneProject = project->getScene(sceneId);
    VoxelGenerateUndo undo;
    if (sceneProject && sceneProject->scene) {
        result = steps->finish(sceneProject->scene, record ? &undo : nullptr);
        if (record && result.ok && !result.estimate && !cancelled.load()) {
            CommandHandle::get(sceneId)->addCommandNoMerge(new VoxelGenerateCmd(project, sceneId, target, std::move(undo)));
        }
    } else {
        result = VoxelGenerateResult{};
        result.error = "not_loaded";
    }
    steps.reset();
    return true;
}
