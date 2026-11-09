// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelVolumeOps.h"

#include "Project.h"
#include "ShippingTypes.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "command/type/VoxelEditCmd.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "subsystem/VoxelSystem.h"
#include "util/FileUtils.h"
#include "util/MeshTriangles.h"
#include "voxel/VoxelAssetFile.h"
#include "voxel/VoxelFile.h"
#include "voxel/VoxelProjectSettings.h"
#include "voxel/VoxelVoxelize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr BuildPlatform kStatsTargets[] = {BuildPlatform::Windows, BuildPlatform::Linux, BuildPlatform::MacOS,
                                          BuildPlatform::Android, BuildPlatform::IOS, BuildPlatform::Web};

    std::shared_ptr<VoxelSystem> opsSystemOf(Project* project, uint32_t sceneId) {
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        return sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<VoxelSystem>() : nullptr;
    }

    // the file's bytes for what the asset holds now: R76's editor form
    std::shared_ptr<std::vector<uint8_t>> encodeAsset(const VoxelAsset& asset) {
        voxel::SaveOptions options;
        options.mode = voxel::SaveOptions::Mode::Editor;
        options.slotCount = asset.slotCount;
        options.generatorJson = voxel::generatorToJson(asset.generator);
        options.terrainCut = asset.terrainCut;
        return std::make_shared<std::vector<uint8_t>>(voxel::saveVolumeFile(*asset.store, options));
    }

    std::string copyPath(Project& project, const std::string& path) {
        const fs::path source = FileUtils::pathFromUtf8(path);
        const fs::path folder = source.parent_path();
        const std::string stem = FileUtils::pathToUtf8(source.stem());
        for (int attempt = 1;; attempt++) {
            const std::string name = attempt == 1 ? stem + " Copy.voxelvolume" : stem + " Copy " + std::to_string(attempt) + ".voxelvolume";
            const fs::path candidate = (folder / FileUtils::pathFromUtf8(name)).lexically_normal();
            std::error_code ignored;
            if (!fs::exists(project.getAssetsPath() / candidate, ignored)) return candidate.generic_string();
        }
    }

    std::string formatStat(const char* format, double value) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), format, value);
        return buffer;
    }

    std::string statsMegabytes(int64_t bytes) {
        return formatStat("%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }

    // One undo step of a change of an asset's identity in memory: Voxel Size lives in the file's header,
    // so old and new are whole file images. The new one is dirty and saves with the scene or Save.
    class VoxelResizeCmd : public Command {
    public:
        VoxelResizeCmd(Project* project, uint32_t sceneId, std::string path, std::shared_ptr<const std::vector<uint8_t>> before,
                       std::shared_ptr<const std::vector<uint8_t>> after):
            project(project), sceneId(sceneId), path(std::move(path)), before(std::move(before)), after(std::move(after)) {}

        bool execute() override {
            install(after);
            return true;
        }
        void undo() override { install(before); }
        bool mergeWith(Command*) override { return false; }
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return "Apply to Voxel Size"; }
        const char* typeName() const override { return "VoxelResizeCmd"; }
        size_t memoryBytes() const override { return sizeof(*this) + before->size() + after->size(); }

    private:
        void install(const std::shared_ptr<const std::vector<uint8_t>>& bytes) {
            const std::shared_ptr<VoxelAsset> asset = VoxelAssets::fromBytes(path, bytes);
            asset->dirty = true;
            // every GameObject showing the file reads it again
            SceneProject* sceneProject = project->getScene(sceneId);
            if (std::shared_ptr<VoxelSystem> system = opsSystemOf(project, sceneId)) {
                auto array = sceneProject->scene->getComponentArray<VoxelVolumeComponent>();
                for (size_t i = 0; i < array->size(); i++) {
                    if (array->getComponentFromIndex(i).volumeData.path == path) system->reloadVolume(array->getEntity(i));
                }
                held = asset;
            }
            sceneProject->isModified = true;
        }

        Project* project;
        uint32_t sceneId;
        std::string path;
        std::shared_ptr<const std::vector<uint8_t>> before;
        std::shared_ptr<const std::vector<uint8_t>> after;
        // the file image stays the cached asset until the system takes it on its next update
        std::shared_ptr<VoxelAsset> held;
    };

}

const std::vector<VoxelInspectorRow>& editor::voxelInspectorRows() {
    using K = VoxelInspectorRowKind;
    static const std::vector<VoxelInspectorRow> rows = {
        {"volumeData", "Volume Data", K::Asset, "The .voxelvolume asset: dimensions, voxel size and the blocks"},
        {"quality", "Quality", K::Enum, "Auto: the platform's, or the attached terrain's"},
        {"lodLevels", "LOD Levels", K::Int, "0: from Quality, else 1 to 4"},
        {"lodRadius", "LOD Radius", K::Int, "Blocks per LOD level around the focus: 4, 6 or 8"},
        {"maxDrawDistance", "Max Draw Distance", K::Float, "0: none"},
        {"castShadows", "Cast Shadows", K::Bool, "Draws into shadow maps"},
        {"collision", "Collision", K::Bool, "Bodies collide with the surface"},
        {"collisionRadius", "Collision Radius", K::Int, "Blocks around what moves; 0: from Quality"},
        {"rescueBodies", "Rescue Bodies", K::Bool, "Bodies a dig leaves inside solid come back out"},
        {"navigationUpdate", "Navigation Update", K::Enum, "Automatic: a NavMesh Surface rebakes what an edit changed"},
        {"terrain", "Terrain", K::Entity, "The Terrain it is attached to; none: free-standing"},
        {"terrainCut", "Terrain Cut", K::Enum, "Automatic: cuts Terrain holes where the volume breaks its surface"},
        {"overlapDepth", "Overlap Depth", K::Float, "How far the volume's surface sits under the terrain's. Negative: 0.01 x Voxel Size"},
        {"cutTolerance", "Cut Tolerance", K::Float, "A surface this close to the terrain's cuts no hole. Negative: 0.25 x Voxel Size"},
        {"focus", "Focus", K::Entity, "What LOD follows; none: the active camera"},
        {"runtimeGeneration", "Runtime Generation", K::Bool, "Keep the generator in the build and evaluate it in the player, instead of shipping baked blocks"},
    };
    return rows;
}

int editor::voxelAssetUserCount(Project* project, const std::string& assetPath) {
    int users = 0;
    for (SceneProject& sceneProject : project->getScenes()) {
        if (!sceneProject.scene) continue;
        auto array = sceneProject.scene->getComponentArray<VoxelVolumeComponent>();
        for (size_t i = 0; i < array->size(); i++) {
            if (array->getComponentFromIndex(i).volumeData.path == assetPath) users++;
        }
    }
    return users;
}

bool editor::makeVoxelVolumeUnique(Project* project, uint32_t sceneId, Entity owner, std::string* error) {
    std::shared_ptr<VoxelSystem> system = opsSystemOf(project, sceneId);
    const std::shared_ptr<VoxelAsset> asset = system ? system->getAsset(owner) : nullptr;
    if (!asset || !asset->store) {
        if (error) *error = "The volume is not loaded, so there is nothing to copy.";
        return false;
    }
    const std::string path = copyPath(*project, asset->path);
    const std::shared_ptr<VoxelAsset> copy = VoxelAssets::fromBytes(path, encodeAsset(*asset));
    AssetRef ref;
    if (!saveNewVoxelAsset(*project, *copy, ref, error)) return false;
    CommandHandle::get(sceneId)->addCommandNoMerge(new PropertyCmd<AssetRef>(project, sceneId, owner, ComponentType::VoxelVolumeComponent, "volumeData", ref));
    return true;
}

bool editor::applyScaleToVoxelSize(Project* project, uint32_t sceneId, Entity owner, std::string* error) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    const Transform* transform = scene ? scene->findComponent<Transform>(owner) : nullptr;
    std::shared_ptr<VoxelSystem> system = opsSystemOf(project, sceneId);
    const std::shared_ptr<VoxelAsset> asset = system ? system->getAsset(owner) : nullptr;
    if (!transform || !asset || !asset->store) {
        if (error) *error = "The volume is not loaded.";
        return false;
    }
    const Vector3 scale = transform->scale;
    const float largest = std::max({std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)});
    if (std::fabs(scale.x - scale.y) > 1e-4f * largest || std::fabs(scale.x - scale.z) > 1e-4f * largest || !(scale.x > 0.0f)) {
        if (error) *error = "Voxel Size is one number, so only a uniform positive scale can go into it. Set the Scale's axes equal first.";
        return false;
    }

    voxel::Store& store = *asset->store;
    voxel::VolumeDesc desc = store.desc();
    desc.voxelSize *= scale.x;
    voxel::GeneratorDesc generator = asset->generator;
    generator.height *= static_cast<double>(scale.x);

    auto next = std::make_shared<VoxelAsset>();
    next->path = asset->path;
    next->generator = generator;
    next->slotCount = asset->slotCount;
    next->terrainCut = asset->terrainCut;
    next->store = std::make_shared<voxel::Store>(desc);
    VoxelAssets::resolveBase(*next);
    // samples are in voxels: only the metres they stand for change, so modified blocks carry over as they are
    for (int level = 0; level < store.levels(); level++) {
        const voxel::Int3 count = store.blockCount(level);
        for (int z = 0; z < count.z; z++) {
            for (int y = 0; y < count.y; y++) {
                for (int x = 0; x < count.x; x++) {
                    const voxel::BlockKey key{level, x, y, z};
                    if (store.slot(key).modified()) next->store->setLoadedSlot(key, store.readBlock(key), true);
                }
            }
        }
    }

    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    history->addCommandNoMerge(new VoxelResizeCmd(project, sceneId, asset->path, encodeAsset(*asset), encodeAsset(*next)));
    history->addCommandNoMerge(new PropertyCmd<Vector3>(project, sceneId, owner, ComponentType::Transform, "scale", Vector3(1.0f, 1.0f, 1.0f)));
    history->groupSince(mark, "Apply to Voxel Size");
    return true;
}

int editor::reseedVoxelVolumeFromTerrain(Project* project, uint32_t sceneId, Entity owner, const voxel::IntBox* region, std::string* error) {
    std::shared_ptr<VoxelSystem> system = opsSystemOf(project, sceneId);
    voxel::Store* store = system ? system->getStore(owner) : nullptr;
    const std::shared_ptr<VoxelAsset> asset = system ? system->getAsset(owner) : nullptr;
    if (!store || !asset) {
        if (error) *error = "The volume is not loaded.";
        return -1;
    }
    if (asset->generator.kind != "terrain" && asset->generator.otherKind != "terrain") {
        if (error) *error = "This volume's base is not a Terrain: use Regenerate to give it one.";
        return -1;
    }
    const voxel::Int3 size = store->levelSamples(0);
    voxel::IntBox box;
    box.min = voxel::Int3{0, 0, 0};
    box.max = voxel::Int3{size.x - 1, size.y - 1, size.z - 1};
    if (region) {
        const int span = voxel::BLOCK_SIZE << (store->levels() - 1);
        const auto lower = [span](int value) { return std::max(value, 0) / span * span; };
        const auto upper = [span](int value, int limit) { return std::min((std::max(value, 0) / span + 1) * span, limit) - 1; };
        box.min = voxel::Int3{lower(region->min.x), lower(region->min.y), lower(region->min.z)};
        box.max = voxel::Int3{upper(region->max.x, size.x), upper(region->max.y, size.y), upper(region->max.z, size.z)};
    }

    std::vector<voxel::SlotChange> delta;
    for (int level = 0; level < store->levels(); level++) {
        const voxel::Int3 count = store->blockCount(level);
        const int shift = voxel::BLOCK_SHIFT + level;
        for (int z = box.min.z >> shift; z <= (box.max.z >> shift) && z < count.z; z++) {
            for (int y = box.min.y >> shift; y <= (box.max.y >> shift) && y < count.y; y++) {
                for (int x = box.min.x >> shift; x <= (box.max.x >> shift) && x < count.x; x++) {
                    const voxel::BlockKey key{level, x, y, z};
                    const voxel::Slot& slot = store->slot(key);
                    if (!slot.modified()) continue;
                    voxel::Slot after;
                    after.setBase(false, true);
                    delta.push_back(voxel::SlotChange{key, slot, std::move(after)});
                }
            }
        }
    }
    const int changed = static_cast<int>(delta.size());
    if (changed == 0) return 0;
    system->applyDelta(owner, delta, false);
    CommandHandle::get(sceneId)->addCommandNoMerge(new VoxelEditCmd(project, sceneId, owner, "Re-seed From Terrain", std::move(delta)));
    asset->dirty = true;
    return changed;
}

bool editor::saveVoxelVolume(Project* project, uint32_t sceneId, Entity owner, std::string* error) {
    std::shared_ptr<VoxelSystem> system = opsSystemOf(project, sceneId);
    const std::shared_ptr<VoxelAsset> asset = system ? system->getAsset(owner) : nullptr;
    if (!asset) {
        if (error) *error = "The volume is not loaded.";
        return false;
    }
    return saveVoxelAsset(*asset, error);
}

std::vector<VoxelStatsRow> editor::voxelStatsRows(const VoxelStats& stats, const VoxelProjectSettings& settings) {
    std::vector<VoxelStatsRow> rows;
    const auto add = [&rows](const char* label, std::string value) { rows.push_back(VoxelStatsRow{label, std::move(value)}); };
    const voxel::Store::BlockCounts& blocks = stats.blocks;
    // hot: what occupies block memory now, neither uniform nor waiting in the file or the generator
    const int64_t hot = blocks.total - blocks.uniform - blocks.base - blocks.cold;
    add("Blocks Total", std::to_string(blocks.total));
    add("Uniform", std::to_string(blocks.uniform));
    add("Non-uniform", std::to_string(blocks.nonUniform));
    add("Hot", std::to_string(hot));
    add("Cold", std::to_string(blocks.cold));
    add("Modified", std::to_string(blocks.modified));
    add("Memory Hot", statsMegabytes(stats.memory.hotBytes));
    add("Memory Cold", statsMegabytes(stats.memory.coldBytes));
    add("Memory Tables", statsMegabytes(stats.memory.tableBytes));
    add("Memory Meshes", statsMegabytes(stats.meshBytes));
    for (BuildPlatform target : kStatsTargets) {
        const VoxelLimits limits = lowerVoxelLimits(voxelTargetLimits(target), settings);
        const std::string used = statsMegabytes(stats.memory.total()) + " of " + statsMegabytes(limits.cpuBudget) + ", meshes " + statsMegabytes(stats.meshBytes) + " of " + statsMegabytes(limits.meshBudget);
        rows.push_back(VoxelStatsRow{std::string("Budget ") + buildPlatformName(target), used});
    }
    add("Triangles", std::to_string(stats.triangles));
    add("Chunks", std::to_string(stats.chunks));
    add("Draws", std::to_string(stats.draws));
    add("Jobs Pending", std::to_string(stats.jobsPending));
    add("Collision Bodies", std::to_string(stats.collisionBodies));
    add("Last Mesh", formatStat("%.2f ms", stats.lastMeshMs));
    add("Last Latency", stats.lastLatencyFrames < 0 ? std::string("-") : std::to_string(stats.lastLatencyFrames) + " frames");
    return rows;
}

std::shared_ptr<const voxel::StampField> editor::voxelStampFieldFromAsset(const std::string& assetPath) {
    const std::shared_ptr<VoxelAsset> asset = VoxelAssets::acquire(assetPath);
    if (!asset || !asset->store) return nullptr;
    voxel::Store& store = *asset->store;
    const voxel::Int3 samples = store.levelSamples(0);
    auto field = std::make_shared<voxel::StampField>();
    field->size = samples;
    field->voxelSize = store.desc().voxelSize;
    field->sdf.resize(static_cast<size_t>(samples.x) * samples.y * samples.z);
    size_t i = 0;
    for (int z = 0; z < samples.z; z++) {
        for (int y = 0; y < samples.y; y++) {
            for (int x = 0; x < samples.x; x++) field->sdf[i++] = store.sdfAt(0, x, y, z);
        }
    }
    field->originOffset[0] = samples.x * field->voxelSize * 0.5;
    field->originOffset[1] = samples.y * field->voxelSize * 0.5;
    field->originOffset[2] = samples.z * field->voxelSize * 0.5;
    return field;
}

std::shared_ptr<const voxel::StampField> editor::voxelMeshField(Scene* scene, Entity entity, double voxelSize) {
    MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (!mesh) return nullptr;
    std::vector<Vector3> vertices;
    std::vector<uint32_t> indices;
    MeshTriangles::appendMeshTriangles(mesh, Matrix4(), vertices, indices);
    std::vector<float> positions;
    positions.reserve(vertices.size() * 3);
    for (const Vector3& v : vertices) {
        positions.push_back(v.x);
        positions.push_back(v.y);
        positions.push_back(v.z);
    }
    voxel::VoxelizeOptions options;
    options.voxelSize = voxelSize;
    return voxel::voxelizeMesh(positions, indices, options);
}
