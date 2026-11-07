// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelCreate.h"

#include "Project.h"
#include "ShippingTypes.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/TerrainComponent.h"
#include "component/VoxelVolumeComponent.h"
#include "subsystem/VoxelSystem.h"
#include "util/FileUtils.h"
#include "voxel/VoxelAssetFile.h"
#include "voxel/VoxelProjectSettings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr int64_t CREATE_BLOCK_PAYLOAD = voxel::BLOCK_SAMPLES * (sizeof(int16_t) + sizeof(uint16_t));
    constexpr int64_t CREATE_SLOT_BYTES = 8;
    constexpr BuildPlatform kCreateTargets[] = {BuildPlatform::Windows, BuildPlatform::Linux, BuildPlatform::MacOS,
                                          BuildPlatform::Android, BuildPlatform::IOS, BuildPlatform::Web};

    int roundUpToBlock(int value) {
        const int positive = std::max(value, voxel::BLOCK_SIZE);
        return (positive + voxel::BLOCK_SIZE - 1) / voxel::BLOCK_SIZE * voxel::BLOCK_SIZE;
    }

    // the share of a level's blocks a base leaves non-uniform; a plain ground's is two layers of them
    // The blocks a base leaves non-uniform: a ground surface (two block layers), a share of all
    // blocks, or both for what is carved into a ground
    struct NonUniformShape {
        bool surface = false;
        double share = 0.0;
    };

    NonUniformShape nonUniformShape(VoxelCreateBase base) {
        switch (base) {
            case VoxelCreateBase::Empty: return {false, 0.0};
            case VoxelCreateBase::SolidGround:
            case VoxelCreateBase::FromTerrain: return {true, 0.0};
            case VoxelCreateBase::Caves: return {true, 0.35};
            case VoxelCreateBase::Overhangs: return {true, 0.25};
            case VoxelCreateBase::CaveEntrance: return {true, 0.20};
            case VoxelCreateBase::NoiseGraph: return {false, 0.30};
            case VoxelCreateBase::FromMesh: return {false, 0.15};
        }
        return {};
    }

    std::string joinedTargetNames(const std::vector<std::string>& names) {
        std::string text;
        for (const std::string& name : names) {
            if (!text.empty()) text += ", ";
            text += name;
        }
        return text;
    }

    // Beside the scene's file, under the assets; a scene outside them (or not saved yet) keeps its
    // volumes in voxels/
    std::string newAssetPath(Project& project, const SceneProject& sceneProject, const std::string& name) {
        fs::path folder = "voxels";
        if (!sceneProject.filepath.empty()) {
            const fs::path scene = sceneProject.filepath.is_absolute() ? sceneProject.filepath : project.getProjectPath() / sceneProject.filepath;
            const fs::path parent = scene.parent_path().lexically_normal();
            const fs::path relative = parent.lexically_relative(project.getAssetsPath().lexically_normal());
            if (!relative.empty() && *relative.begin() != "..") folder = relative;
        }
        std::string stem;
        for (const char c : name) stem += (c == '/' || c == '\\' || c == ':') ? '_' : c;
        if (stem.empty()) stem = "Voxel Volume";
        for (int attempt = 0;; attempt++) {
            const std::string file = attempt == 0 ? stem + ".voxelvolume" : stem + " " + std::to_string(attempt) + ".voxelvolume";
            const fs::path candidate = (folder / file).lexically_normal();
            std::error_code ignored;
            if (!fs::exists(project.getAssetsPath() / candidate, ignored)) return candidate.generic_string();
        }
    }

}

const char* editor::voxelCreatePresetLabel(VoxelCreatePreset preset) {
    switch (preset) {
        case VoxelCreatePreset::Small: return "Small (64 x 64 x 64)";
        case VoxelCreatePreset::Medium: return "Medium (128 x 64 x 128)";
        case VoxelCreatePreset::Large: return "Large (256 x 128 x 256)";
        case VoxelCreatePreset::Custom: return "Custom";
    }
    return "";
}

const char* editor::voxelCreateBaseLabel(VoxelCreateBase base) {
    switch (base) {
        case VoxelCreateBase::Empty: return "Empty";
        case VoxelCreateBase::SolidGround: return "Solid Ground";
        case VoxelCreateBase::FromTerrain: return "From Terrain";
        case VoxelCreateBase::Caves: return "Caves";
        case VoxelCreateBase::Overhangs: return "Overhangs";
        case VoxelCreateBase::CaveEntrance: return "Cave Entrance";
        case VoxelCreateBase::NoiseGraph: return "Noise Graph...";
        case VoxelCreateBase::FromMesh: return "From Mesh...";
    }
    return "";
}

voxel::Int3 editor::voxelCreatePresetSize(VoxelCreatePreset preset) {
    switch (preset) {
        case VoxelCreatePreset::Small: return {64, 64, 64};
        case VoxelCreatePreset::Medium: return {128, 64, 128};
        case VoxelCreatePreset::Large: return {256, 128, 256};
        case VoxelCreatePreset::Custom: break;
    }
    return {};
}

voxel::Int3 editor::voxelCreateDimensions(const VoxelCreateOptions& options) {
    const voxel::Int3 source = options.preset == VoxelCreatePreset::Custom ? options.dimensions : voxelCreatePresetSize(options.preset);
    return voxel::Int3{roundUpToBlock(source.x), roundUpToBlock(source.y), roundUpToBlock(source.z)};
}

voxel::GeneratorDesc editor::voxelCreateGenerator(const VoxelCreateOptions& options) {
    const voxel::Int3 size = voxelCreateDimensions(options);
    voxel::GeneratorDesc generator;
    generator.seed = options.seed;
    switch (options.base) {
        case VoxelCreateBase::Empty:
            generator.kind = "empty";
            break;
        case VoxelCreateBase::SolidGround:
            generator.kind = "solid_ground";
            generator.height = static_cast<double>(size.y) * static_cast<double>(options.voxelSize) * 0.5;
            break;
        case VoxelCreateBase::FromTerrain:
            generator.kind = "terrain";
            break;
        case VoxelCreateBase::Caves:
            generator.kind = "preset";
            generator.preset = "caves";
            break;
        case VoxelCreateBase::Overhangs:
            generator.kind = "preset";
            generator.preset = "overhangs";
            break;
        case VoxelCreateBase::CaveEntrance:
            generator.kind = "preset";
            generator.preset = "cave_entrance";
            break;
        case VoxelCreateBase::NoiseGraph:
            generator.kind = "noise_graph";
            generator.sourcePath = options.graphPath;
            break;
        case VoxelCreateBase::FromMesh:
            generator.kind = "mesh";
            break;
    }
    return generator;
}

bool editor::voxelCreateNeedsGeneration(const VoxelCreateOptions& options) {
    switch (options.base) {
        case VoxelCreateBase::Empty:
        case VoxelCreateBase::SolidGround:
            return false;
        // the terrain's too: only the steps store the hash that says when it has gone stale (V10)
        case VoxelCreateBase::FromTerrain:
        case VoxelCreateBase::Caves:
        case VoxelCreateBase::Overhangs:
        case VoxelCreateBase::CaveEntrance:
        case VoxelCreateBase::NoiseGraph:
        case VoxelCreateBase::FromMesh:
            return true;
    }
    return false;
}

VoxelGenerateRequest editor::voxelCreateRequest(const VoxelCreateOptions& options) {
    VoxelGenerateRequest request;
    request.seed = options.seed;
    request.terrain = options.terrain;
    request.keepEdits = false;
    switch (options.base) {
        case VoxelCreateBase::FromTerrain: request.source = "terrain"; break;
        case VoxelCreateBase::NoiseGraph:
            request.source = "noise_graph";
            request.graphPath = options.graphPath;
            break;
        case VoxelCreateBase::FromMesh:
            request.source = "mesh";
            request.mesh = options.mesh;
            break;
        case VoxelCreateBase::Empty: request.source = "preset"; request.preset = "empty"; break;
        case VoxelCreateBase::SolidGround: request.source = "preset"; request.preset = "solid_ground"; break;
        case VoxelCreateBase::Caves: request.source = "preset"; request.preset = "caves"; break;
        case VoxelCreateBase::Overhangs: request.source = "preset"; request.preset = "overhangs"; break;
        case VoxelCreateBase::CaveEntrance: request.source = "preset"; request.preset = "cave_entrance"; break;
    }
    return request;
}

VoxelCreateEstimate editor::estimateVoxelCreate(const VoxelCreateOptions& options, const VoxelProjectSettings& settings) {
    VoxelCreateEstimate estimate;
    const voxel::Int3 size = voxelCreateDimensions(options);
    const int levels = voxel::levelsFor(size, voxel::MAX_LEVELS);
    const NonUniformShape shape = nonUniformShape(options.base);
    for (int level = 0; level < levels; level++) {
        const int span = voxel::BLOCK_SIZE << level;
        const int64_t blocksX = (size.x + span - 1) / span;
        const int64_t blocksY = (size.y + span - 1) / span;
        const int64_t blocksZ = (size.z + span - 1) / span;
        const int64_t total = blocksX * blocksY * blocksZ;
        estimate.bytes += total * CREATE_SLOT_BYTES;
        const int64_t surface = shape.surface ? blocksX * std::min<int64_t>(blocksY, 2) * blocksZ : 0;
        const int64_t nonUniform = std::min(total, surface + static_cast<int64_t>(std::llround(static_cast<double>(total) * shape.share)));
        estimate.bytes += nonUniform * CREATE_BLOCK_PAYLOAD;
    }
    for (BuildPlatform target : kCreateTargets) {
        const VoxelLimits limits = lowerVoxelLimits(voxelTargetLimits(target), settings);
        if (estimate.bytes > limits.cpuBudget) estimate.overBudgetTargets.push_back(buildPlatformName(target));
        if (!voxelDimensionProblem(options.name, size, target, settings).empty()) estimate.oversizeTargets.push_back(buildPlatformName(target));
    }
    if (!estimate.overBudgetTargets.empty()) {
        estimate.warning = "Over the memory budget on " + joinedTargetNames(estimate.overBudgetTargets) + ".";
    }
    if (!estimate.oversizeTargets.empty()) {
        if (!estimate.warning.empty()) estimate.warning += " ";
        estimate.warning += "Too large for " + joinedTargetNames(estimate.oversizeTargets) + ".";
    }
    return estimate;
}

std::string editor::voxelCreateProblem(const VoxelCreateOptions& options) {
    if (!(options.voxelSize > 0.0f) || !std::isfinite(options.voxelSize)) return "Voxel Size must be above zero.";
    if (options.base == VoxelCreateBase::FromTerrain && options.terrain == NULL_ENTITY) return "From Terrain needs a Terrain.";
    if (options.base == VoxelCreateBase::CaveEntrance && options.terrain == NULL_ENTITY) return "Cave Entrance digs from a Terrain: choose one.";
    if (options.base == VoxelCreateBase::NoiseGraph && options.graphPath.empty()) return "Choose a Noise Graph.";
    if (options.base == VoxelCreateBase::FromMesh && options.mesh == NULL_ENTITY) return "Choose a GameObject with a Mesh.";
    const voxel::Int3 size = voxelCreateDimensions(options);
    const int largest = std::max({size.x, size.y, size.z});
    if (largest > voxelTargetLimits(BuildPlatform::Linux).maxDimension) return "A dimension is over the largest a target takes (1024 voxels).";
    return {};
}

Entity editor::createVoxelVolume(Project* project, uint32_t sceneId, const VoxelCreateOptions& options, std::string* error) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene) {
        if (error) *error = "No scene is open.";
        return NULL_ENTITY;
    }
    const std::string problem = voxelCreateProblem(options);
    if (!problem.empty()) {
        if (error) *error = problem;
        return NULL_ENTITY;
    }

    voxel::VolumeDesc desc;
    desc.dimensions = voxelCreateDimensions(options);
    desc.voxelSize = options.voxelSize;
    desc.levels = voxel::levelsFor(desc.dimensions, voxel::MAX_LEVELS);
    desc.bounds = voxel::BoundsMode::Closed;

    // One slot to start with, or the terrain's layers in order, each following its layer (R43)
    std::vector<VoxelMaterialSlot> slots;
    Scene* scene = sceneProject->scene;
    const TerrainComponent* terrain = options.terrain != NULL_ENTITY ? scene->findComponent<TerrainComponent>(options.terrain) : nullptr;
    if ((options.base == VoxelCreateBase::FromTerrain || options.base == VoxelCreateBase::CaveEntrance) && terrain) {
        for (size_t i = 0; i < terrain->surfaceLayers.size() && i < static_cast<size_t>(voxel::MAX_SLOTS); i++) {
            VoxelMaterialSlot slot;
            slot.name = "Layer " + std::to_string(i);
            slot.linkedTerrainLayer = static_cast<int>(i);
            slots.push_back(std::move(slot));
        }
    }
    if (slots.empty()) {
        VoxelMaterialSlot slot;
        slot.name = "Default";
        slots.push_back(std::move(slot));
    }

    const std::string path = newAssetPath(*project, *sceneProject, options.name);
    const std::shared_ptr<VoxelAsset> asset = VoxelAssets::create(path, desc, voxelCreateGenerator(options), static_cast<int>(slots.size()));
    std::error_code ignored;
    fs::create_directories((project->getAssetsPath() / path).parent_path(), ignored);
    AssetRef ref;
    if (!saveNewVoxelAsset(*project, *asset, ref, error)) return NULL_ENTITY;

    auto* create = new CreateEntityCmd(project, sceneId, options.name, EntityCreationType::VOXEL_VOLUME, options.parent);
    create->addProperty<Vector3>(ComponentType::Transform, "position", options.position);
    create->addProperty<AssetRef>(ComponentType::VoxelVolumeComponent, "volumeData", ref);
    create->addProperty<std::vector<VoxelMaterialSlot>>(ComponentType::VoxelVolumeComponent, "materialSlots", slots);
    if (terrain) {
        create->addProperty<Entity>(ComponentType::VoxelVolumeComponent, "terrain", options.terrain);
        create->addProperty<int>(ComponentType::VoxelVolumeComponent, "terrainCut", static_cast<int>(VoxelTerrainCut::Automatic));
    }
    CommandHistory* history = CommandHandle::get(sceneId);
    if (!history->addCommandNoMerge(create)) {
        if (error) *error = "The GameObject could not be created.";
        return NULL_ENTITY;
    }
    return create->getEntity();
}
