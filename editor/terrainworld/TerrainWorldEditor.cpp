// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrainworld/TerrainWorldEditor.h"

#include "Project.h"
#include "Scene.h"
#include "asset/AssetTypes.h"
#include "asset/Guid.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "texture/TextureData.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terraincore;

namespace fs = std::filesystem;

namespace {

    bool scenePlays(Project* project, Scene* scene) {
        if (!project) return false;
        for (const SceneProject& sceneProject : project->getScenes()) {
            if (sceneProject.scene != scene) continue;
            return sceneProject.playState == ScenePlayState::PLAYING || sceneProject.playState == ScenePlayState::PAUSED;
        }
        return false;
    }

    // a colour map's mean over a sparse grid of its texels, linear enough for a far overview
    bool textureMean(const Texture& texture, float out[3]) {
        if (!texture.hasData()) return false;
        Texture& readable = const_cast<Texture&>(texture);
        TextureData& data = readable.getData();
        const auto* pixels = static_cast<const uint8_t*>(data.getData());
        const int channels = data.getChannels();
        if (!pixels || data.getWidth() <= 0 || data.getHeight() <= 0 || channels < 3 || TextureData::getBytesPerChannel(data.getColorFormat()) != 1) return false;
        double sum[3] = {};
        int count = 0;
        const int step = std::max(1, std::min(data.getWidth(), data.getHeight()) / 32);
        for (int y = 0; y < data.getHeight(); y += step) {
            for (int x = 0; x < data.getWidth(); x += step) {
                const uint8_t* texel = pixels + (static_cast<size_t>(y) * data.getWidth() + x) * channels;
                for (int c = 0; c < 3; c++) sum[c] += texel[c] / 255.0;
                count++;
            }
        }
        for (int c = 0; c < 3; c++) out[c] = static_cast<float>(sum[c] / std::max(count, 1));
        return true;
    }

}

void jasperengine::editor::installTerrainWorldHost(Project* project) {
    TerrainWorldSystem::Host host;
    host.makeSource = [project](Scene*, Entity, const TerrainWorldComponent& world) -> std::shared_ptr<TerrainWorldSource> {
        if (world.worldData.empty() || !project) return nullptr;
        const fs::path folder = project->resolveAssetPath(FileUtils::pathFromUtf8(world.worldData.path));
        return std::make_shared<EditorWorldSource>(folder, project->getProjectInternalPath());
    };
    host.playing = [project](Scene* scene) { return scenePlays(project, scene); };
    host.started = [](Scene* scene, Entity entity, TerrainWorldRuntime& runtime) {
        const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        if (!terrain) return;
        runtime.setLayerColours(terrainWorldLayerColours(*terrain, scene->findComponent<MeshComponent>(entity)),
                                static_cast<int>(terrain->surfaceLayers.size()));
    };
    host.released = [](Scene*, Entity, TerrainWorldRuntime& runtime) { TerrainWorldFiles::flushWorld(runtime); };
    TerrainWorldSystem::setHost(std::move(host));
}

void jasperengine::editor::updateTerrainWorlds(double nowSeconds) {
    TerrainWorldSystem::pumpAll();
    TerrainWorldFiles::update(nowSeconds);
}

std::vector<float> jasperengine::editor::terrainWorldLayerColours(const TerrainComponent& terrain, const MeshComponent* mesh) {
    std::vector<float> colours;
    Vector4 base(1.0f, 1.0f, 1.0f, 1.0f);
    float mean[3] = {1.0f, 1.0f, 1.0f};
    if (mesh && mesh->numSubmeshes > 0) {
        base = mesh->submeshes[0].material.baseColorFactor;
        if (!textureMean(mesh->submeshes[0].material.baseColorTexture, mean)) mean[0] = mean[1] = mean[2] = 1.0f;
    }
    colours.insert(colours.end(), {base.x * mean[0], base.y * mean[1], base.z * mean[2], 1.0f});
    for (const TerrainSurfaceLayer& layer : terrain.surfaceLayers) {
        float layerMean[3] = {1.0f, 1.0f, 1.0f};
        textureMean(layer.colorTexture, layerMean);
        colours.insert(colours.end(), {layer.colorFactor.x * layerMean[0], layer.colorFactor.y * layerMean[1], layer.colorFactor.z * layerMean[2], 1.0f});
    }
    return colours;
}

bool jasperengine::editor::validateTerrainWorldSettings(const TerrainWorldSettings& s, std::string& error) {
    WorldLattice lattice;
    lattice.tileQuads = s.tileQuads;
    lattice.spacing = s.spacing;
    if (!validateLattice(lattice, &error)) return false;
    const WorldPlatformBudget budget = terrainWorldPlatformBudget();
    if (s.tileQuads > budget.maxTileQuads) {
        error = "tile_quads is at most " + std::to_string(budget.maxTileQuads) + " on this platform";
        return false;
    }
    if (!(s.maxHeight > 0.0f) || !(s.baseHeight >= 0.0f) || s.baseHeight > s.maxHeight) {
        error = "max_height must be above 0 and base_height between 0 and it";
        return false;
    }
    if (s.gridWidth < 1 || s.gridLength < 1 || s.gridWidth > budget.maxBoxTiles || s.gridLength > budget.maxBoxTiles) {
        error = "the grid is 1 to " + std::to_string(budget.maxBoxTiles) + " tiles a side";
        return false;
    }
    for (int z = s.originZ; z < s.originZ + s.gridLength; z += std::max(1, s.gridLength - 1)) {
        for (int x = s.originX; x < s.originX + s.gridWidth; x += std::max(1, s.gridWidth - 1)) {
            if (!tileWithinLimit(lattice, TileCoord(x, z))) {
                error = "tile (" + std::to_string(x) + ", " + std::to_string(z) + ") is past the world's limit of +-16384 m";
                return false;
            }
        }
    }
    return true;
}

uint64_t jasperengine::editor::predictTerrainWorldBytes(const TerrainWorldSettings& s) {
    WorldLattice lattice;
    lattice.tileQuads = s.tileQuads;
    lattice.spacing = s.spacing;
    const uint64_t perTile = static_cast<uint64_t>(lattice.heightSide()) * lattice.heightSide() * 2;
    const uint64_t tiles = static_cast<uint64_t>(std::max(0, s.gridWidth)) * static_cast<uint64_t>(std::max(0, s.gridLength));
    return perTile * std::min<uint64_t>(tiles, static_cast<uint64_t>(terrainWorldPlatformBudget().poolSize));
}

Entity jasperengine::editor::createTerrainWorld(Project* project, uint32_t sceneId, Entity parent, const TerrainWorldSettings& s, std::string& error) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene) {
        error = "The scene is not open.";
        return NULL_ENTITY;
    }
    if (!validateTerrainWorldSettings(s, error)) return NULL_ENTITY;
    const std::string name = s.name.empty() ? std::string("Terrain World") : s.name;
    std::string assetPath = s.assetPath.empty() ? name + ".terrainworld" : s.assetPath;
    if (!AssetTypes::isFolderAsset(fs::path(assetPath).filename().string())) assetPath += ".terrainworld";
    const fs::path folder = project->resolveAssetPath(FileUtils::pathFromUtf8(assetPath));

    WorldManifest manifest;
    manifest.lattice.tileQuads = s.tileQuads;
    manifest.lattice.spacing = s.spacing;
    manifest.maps.maxHeight = s.maxHeight;
    manifest.maps.baseHeight = static_cast<uint16_t>(std::lround(std::clamp(s.baseHeight / s.maxHeight, 0.0f, 1.0f) * 65535.0f));
    std::vector<TileCoord> tiles;
    for (int z = s.originZ; z < s.originZ + s.gridLength; z++) {
        for (int x = s.originX; x < s.originX + s.gridWidth; x++) tiles.emplace_back(x, z);
    }
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t start = history->mark();
    auto fail = [&](const std::string& why) {
        while (history->position() > start && history->canUndo() && history->undo()) {}
        error = why;
        return NULL_ENTITY;
    };
    CreateEntityCmd* createCmd = new CreateEntityCmd(project, sceneId, name, EntityCreationType::TERRAIN, parent);
    if (!history->addCommandNoMerge(createCmd)) return fail("The Terrain World could not be made.");
    const Entity entity = createCmd->getEntity();
    // the tiles carry the macro colour the runtime derives from the new Terrain's layers
    Scene* scene = sceneProject->scene;
    const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
    const std::vector<float> colours = terrain ? terrainWorldLayerColours(*terrain, scene->findComponent<MeshComponent>(entity)) : std::vector<float>();
    if (!createWorldFolder(folder, manifest, tiles, error, colours, terrain ? static_cast<int>(terrain->surfaceLayers.size()) : 0)) return fail(error);
    FileUtils::reportWritten(folder);
    const AssetRef ref(assetPath, Guid::parse(manifest.guid));
    if (!history->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, ComponentType::TerrainWorldComponent)) ||
        !history->addCommandNoMerge(new PropertyCmd<AssetRef>(project, sceneId, entity, ComponentType::TerrainWorldComponent, "worldData", ref))) {
        return fail("The Terrain World component could not be added.");
    }
    // R20: a static Rigidbody with no collider: the world pages its own
    if (!history->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, ComponentType::RigidbodyComponent))) {
        return fail("The Terrain World's Rigidbody could not be added.");
    }
    history->groupSince(start, "Create Terrain World (" + name + ")");
    project->setSelectedEntity(sceneId, entity);
    return entity;
}

TerrainWorldAutoAddPause::TerrainWorldAutoAddPause(TerrainComponent& terrain): runtime(terrain.world) {
    if (!runtime || !runtime->started()) {
        runtime = nullptr;
        return;
    }
    was = runtime->store().options().autoAddTiles;
    runtime->store().options().autoAddTiles = false;
}

TerrainWorldAutoAddPause::~TerrainWorldAutoAddPause() {
    if (runtime) runtime->store().options().autoAddTiles = was;
}

TerrainWorldRuntime* jasperengine::editor::terrainWorldRuntimeOf(Scene* scene, Entity entity, std::string& error) {
    const std::string name = "\"" + scene->getEntityName(entity) + "\"";
    if (!scene->findComponent<TerrainWorldComponent>(entity)) {
        error = scene->findComponent<TerrainComponent>(entity) ? name + " is a plain Terrain, not a Terrain World: make one with edit_terrain_world {create}."
                                                               : name + " is not a Terrain World: pass one as entity, or make one with edit_terrain_world {create}.";
        return nullptr;
    }
    std::shared_ptr<TerrainWorldSystem> system = scene->findSystem<TerrainWorldSystem>();
    if (!system) {
        error = "This editor was built without Terrain Worlds.";
        return nullptr;
    }
    std::string why;
    TerrainWorldRuntime* runtime = system->ensureRuntime(entity, &why);
    if (!runtime || !runtime->started()) {
        error = "The Terrain World " + name + " did not start: " + (why.empty() ? std::string("its data did not load") : why) +
                ". Check its World Data asset.";
        return nullptr;
    }
    return runtime;
}
