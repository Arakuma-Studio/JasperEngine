// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelProblems.h"

#include "Out.h"
#include "Project.h"
#include "ShippingTypes.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "subsystem/VoxelSystem.h"
#include "util/FileUtils.h"
#include "voxel/VoxelGeneration.h"
#include "voxel/VoxelProjectSettings.h"

#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <tuple>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr BuildPlatform kProblemTargets[] = {BuildPlatform::Windows, BuildPlatform::Linux, BuildPlatform::MacOS,
                                          BuildPlatform::Android, BuildPlatform::IOS, BuildPlatform::Web};

    std::string problemMegabytes(int64_t bytes) {
        return std::to_string((bytes + 512 * 1024) / (1024 * 1024)) + " MB";
    }

    // (scene, entity) -> the codes loggedVoxelProblems and not gone since
    std::map<std::tuple<uint32_t, Entity>, std::set<VoxelProblemCode>>& loggedVoxelProblems() {
        static std::map<std::tuple<uint32_t, Entity>, std::set<VoxelProblemCode>> map;
        return map;
    }

}

const char* editor::voxelProblemName(VoxelProblemCode code) {
    switch (code) {
        case VoxelProblemCode::NoAsset: return "no_asset";
        case VoxelProblemCode::MissingFile: return "missing_file";
        case VoxelProblemCode::CorruptFile: return "corrupt_file";
        case VoxelProblemCode::MissingNoiseGraph: return "missing_noise_graph";
        case VoxelProblemCode::MissingTerrain: return "missing_terrain";
        case VoxelProblemCode::BaseStale: return "base_stale";
        case VoxelProblemCode::OverBudget: return "over_budget";
        case VoxelProblemCode::OversizeForTarget: return "oversize_for_target";
        case VoxelProblemCode::NoSlots: return "no_slots";
        case VoxelProblemCode::ModuleOff: return "module_off";
        case VoxelProblemCode::NonUnitScale: return "non_unit_scale";
    }
    return "";
}

std::vector<VoxelProblem> editor::voxelProblems(Project& project, const SceneProject& sceneProject, Entity owner, bool moduleOn) {
    std::vector<VoxelProblem> problems;
    const auto add = [&](VoxelProblemCode code, std::string what, std::string action) {
        problems.push_back(VoxelProblem{code, std::move(what), std::move(action)});
    };
    Scene* scene = sceneProject.scene;
    if (!scene || !scene->isEntityCreated(owner)) return problems;
    const VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
    if (!component) return problems;

    if (!moduleOn) {
        add(VoxelProblemCode::ModuleOff, "This build has no voxel module, so the volume does nothing.",
            "Use a build with JASPERENGINE_VOXEL on; the volume's data stays as it is.");
        return problems;
    }

    const std::string name = scene->getEntityName(owner);
    if (const Transform* transform = scene->findComponent<Transform>(owner)) {
        const Vector3 scale = transform->scale;
        if (std::fabs(scale.x - 1.0f) > 1e-4f || std::fabs(scale.y - 1.0f) > 1e-4f || std::fabs(scale.z - 1.0f) > 1e-4f) {
            add(VoxelProblemCode::NonUnitScale, "A Voxel Volume takes no scale; it is drawn at (1, 1, 1).",
                "Use Apply to Voxel Size, or set the Transform's Scale back to 1.");
        }
    }

    const std::string& path = component->volumeData.path;
    if (path.empty()) {
        add(VoxelProblemCode::NoAsset, "No .voxelvolume is assigned.", "Drop one on Volume Data, or use GameObject > 3D Object > Voxel Volume.");
        return problems;
    }

    std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
    const std::shared_ptr<VoxelAsset> asset = system ? system->getAsset(owner) : nullptr;
    const fs::path file = (project.getAssetsPath() / FileUtils::pathFromUtf8(path)).lexically_normal();
    std::error_code ignored;
    if (!fs::exists(file, ignored) && (!asset || !asset->store)) {
        add(VoxelProblemCode::MissingFile, "'" + path + "' is not on disk.", "Restore the file, or assign another .voxelvolume.");
        return problems;
    }
    if (asset && (asset->error != voxel::FileError::None || !asset->store)) {
        add(VoxelProblemCode::CorruptFile,
            "'" + path + "' did not load (" + voxel::fileErrorCode(asset->error) + (asset->errorMessage.empty() ? "" : ": ") + asset->errorMessage + ").",
            "The file stays untouched and is not overwritten. Restore it from a backup or version control, or use a newer editor if it was made by one.");
        return problems;
    }
    if (!asset || !asset->store) return problems;

    const voxel::GeneratorDesc& generator = asset->generator;
    if (generator.kind == "noise_graph" || generator.otherKind == "noise_graph") {
        const std::string& graph = generator.kind == "noise_graph" ? generator.sourcePath : generator.otherPath;
        if (graph.empty() || !fs::exists(project.getAssetsPath() / FileUtils::pathFromUtf8(graph), ignored)) {
            add(VoxelProblemCode::MissingNoiseGraph, "The Noise Graph of this volume's base" + (graph.empty() ? std::string() : " ('" + graph + "')") + " is missing.",
                "Restore the graph, or give the volume another base with Regenerate. Modified blocks stay.");
        }
    }
    const bool usesTerrain = generator.kind == "terrain" || component->terrain != NULL_ENTITY;
    if (usesTerrain && (component->terrain == NULL_ENTITY || !scene->isEntityCreated(component->terrain) ||
                        !scene->findComponent<TerrainComponent>(component->terrain))) {
        add(VoxelProblemCode::MissingTerrain, "The Terrain this volume is attached to is missing.", "Pick a Terrain in the Terrain field, or clear it to leave the volume free-standing.");
    }
    // Unless a source being gone already says why: the source changed since the stored hash (V10), or
    // this session could not build the base from the file's description
    const bool sourceGone = !problems.empty() && (problems.back().code == VoxelProblemCode::MissingNoiseGraph || problems.back().code == VoxelProblemCode::MissingTerrain);
    if (!sourceGone && (isVoxelBaseStale(scene, owner) || (!asset->baseResolved && generator.kind != "empty"))) {
        add(VoxelProblemCode::BaseStale, "The base of this volume (" + generator.kind + ") is not up to date with its source.",
            "Regenerate rebuilds it and keeps modified blocks. Until then untouched blocks read as they were.");
    }

    const VoxelProjectSettings& settings = project.getVoxelSettings();
    const int64_t memory = asset->store->memory().total();
    std::string overTargets;
    std::string overBudgetText;
    for (BuildPlatform target : kProblemTargets) {
        const VoxelLimits limits = lowerVoxelLimits(voxelTargetLimits(target), settings);
        if (memory > limits.cpuBudget) {
            if (!overTargets.empty()) overTargets += ", ";
            overTargets += buildPlatformName(target);
            overBudgetText = problemMegabytes(limits.cpuBudget);
        }
    }
    if (!overTargets.empty()) {
        add(VoxelProblemCode::OverBudget, "This volume holds " + problemMegabytes(memory) + ", over the budget on " + overTargets + " (" + overBudgetText + " at the lowest).",
            "Flatten or discard modified blocks, make it smaller, or raise the budget in Project Settings > Voxels.");
    }

    if (!settings.ignoreDimensionLimits) {
        for (BuildPlatform target : kProblemTargets) {
            const std::string problem = voxelDimensionProblem(name, asset->store->desc().dimensions, target, settings);
            if (!problem.empty()) {
                add(VoxelProblemCode::OversizeForTarget, problem, "Make the volume smaller, or allow it with Ignore Dimension Limits in Project Settings > Voxels.");
                break;
            }
        }
    }

    if (component->materialSlots.empty()) {
        add(VoxelProblemCode::NoSlots, "This volume has no material slots, so it draws in the default colour.", "Add a slot in the Voxel Volume panel's Materials tab.");
    }
    return problems;
}

int editor::logVoxelProblems(const SceneProject& sceneProject, Entity owner, const std::vector<VoxelProblem>& problems) {
    std::set<VoxelProblemCode>& seen = loggedVoxelProblems()[std::make_tuple(sceneProject.id, owner)];
    std::set<VoxelProblemCode> now;
    int written = 0;
    const std::string name = sceneProject.scene ? sceneProject.scene->getEntityName(owner) : std::string();
    for (const VoxelProblem& problem : problems) {
        now.insert(problem.code);
        if (seen.count(problem.code)) continue;
        Out::error("Voxel Volume '%s' [%s]: %s %s", name.c_str(), voxelProblemName(problem.code), problem.what.c_str(), problem.action.c_str());
        written++;
    }
    seen = std::move(now);
    return written;
}

void editor::resetVoxelProblemLog() {
    loggedVoxelProblems().clear();
}
