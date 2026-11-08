// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R43, R50: a volume's slots as one texture array and one uniform block. New file, as a hook in
// RenderSystem.cpp would be neither small nor rare (D2).

#include "subsystem/RenderSystem.h"

#include "Log.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "render/SystemRender.h"
#include "subsystem/VoxelSystem.h"
#include "texture/TerrainDetailBuilder.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // u_fs_voxelLayers (includes/voxel_fs.glsl)
    struct fs_voxel_layers_t {
        Vector4 colorFactor[16];
        Vector4 uvTransform[16];
        Vector4 surface[16];
        Vector4 slices[16];
        Vector4 eye;
    };

    // A slot follows its Terrain Layer live (R43)
    const TerrainSurfaceLayer* slotLayer(Scene* scene, const VoxelVolumeComponent& volume, size_t index) {
        const VoxelMaterialSlot& slot = volume.materialSlots[index];
        if (slot.linkedTerrainLayer >= 0 && volume.terrain != NULL_ENTITY && scene->isEntityCreated(volume.terrain)) {
            if (const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(volume.terrain)) {
                if (static_cast<size_t>(slot.linkedTerrainLayer) < terrain->surfaceLayers.size()) {
                    return &terrain->surfaceLayers[static_cast<size_t>(slot.linkedTerrainLayer)];
                }
            }
        }
        return &slot.layer;
    }

    // R50's cap on an own array, per tier
    int maxArraySize(uint8_t tier) {
        return tier == 3 ? 512 : 1024;
    }

}

void RenderSystem::destroyVoxelDetailArrays() {
    for (auto& item : voxelDetailArrays) {
        if (!item.second.sharesTerrain) item.second.render.destroyTexture();
    }
    voxelDetailArrays.clear();
}

void RenderSystem::invalidateVoxelDetailArray(Entity owner) {
    const auto found = voxelDetailArrays.find(owner);
    if (found != voxelDetailArrays.end()) {
        if (!found->second.sharesTerrain) found->second.render.destroyTexture();
        voxelDetailArrays.erase(found);
    }
    // Rebinds on the next load of each chunk
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++) {
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (mesh.voxelOwner == owner && mesh.loaded) mesh.needReload = true;
    }
}

RenderSystem::VoxelDetailArray* RenderSystem::getVoxelDetailArray(Entity owner, TextureRender*& bound) {
    bound = nullptr;
    VoxelVolumeComponent* volume = scene->findComponent<VoxelVolumeComponent>(owner);
    if (!volume || volume->materialSlots.empty()) return nullptr;

    const size_t count = std::min<size_t>(volume->materialSlots.size(), 16);

    // All linked: the terrain's own array, so its slices (and their cost) are not doubled (R50)
    bool allLinked = volume->terrain != NULL_ENTITY && scene->isEntityCreated(volume->terrain);
    TerrainComponent* terrain = allLinked ? scene->findComponent<TerrainComponent>(volume->terrain) : nullptr;
    for (size_t i = 0; i < count && allLinked; i++) {
        allLinked = terrain && volume->materialSlots[i].linkedTerrainLayer >= 0;
    }
    if (allLinked) {
        if (TerrainDetailArray* shared = getTerrainDetailArray(volume->terrain, *terrain)) {
            VoxelDetailArray& entry = voxelDetailArrays[owner];
            entry.sharesTerrain = true;
            entry.failed = false;
            entry.colorSlice.fill(-1);
            entry.normalSlice.fill(-1);
            entry.surfaceSlice.fill(-1);
            for (size_t i = 0; i < count; i++) {
                const int layer = volume->materialSlots[i].linkedTerrainLayer;
                if (layer >= MAX_TERRAIN_LAYERS) continue;
                entry.colorSlice[i] = layer;
                entry.normalSlice[i] = shared->normalSlice[static_cast<size_t>(layer)];
                entry.surfaceSlice[i] = shared->surfaceSlice[static_cast<size_t>(layer)];
            }
            bound = &shared->render;
            return &entry;
        }
    }

    uint8_t tier = 1;
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++) {
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (mesh.voxelOwner == owner && mesh.voxelTier != 0) {
            tier = mesh.voxelTier;
            break;
        }
    }

    // R50: T3's builder over the slots' layers, so a volume's array has the terrain's formats, cooked
    // slices and height in colour alpha; no surface slices at Low
    const std::vector<TerrainSurfaceLayer> layers = voxelSlotLayers(scene, *volume);
    terrain::DetailPlanOptions planOptions;
    planOptions.surfaceSlices = tier != 3;
    planOptions.wholeBlendMaps = false;
    const terrain::DetailPlan plan = terrain::planDetailArray(layers, planOptions);
    if (!plan.anySource) return nullptr;

    auto found = voxelDetailArrays.find(owner);
    if (found != voxelDetailArrays.end() && !found->second.sharesTerrain && found->second.sliceKeys == plan.sliceKeys) {
        if (found->second.failed) return nullptr;
        bound = &found->second.render;
        return &found->second;
    }
    if (found != voxelDetailArrays.end()) {
        if (!found->second.sharesTerrain) found->second.render.destroyTexture();
        voxelDetailArrays.erase(found);
    }

    VoxelDetailArray& entry = voxelDetailArrays[owner];
    entry.sliceKeys = plan.sliceKeys;
    entry.colorSlice.fill(-1);
    entry.normalSlice.fill(-1);
    entry.surfaceSlice.fill(-1);
    for (size_t i = 0; i < layers.size(); i++) {
        entry.colorSlice[i] = static_cast<int>(i);
        entry.normalSlice[i] = plan.normalSlice[i];
        entry.surfaceSlice[i] = plan.surfaceSlice[i];
    }
    entry.sharesTerrain = false;
    entry.failed = true;

    const int cap = maxArraySize(tier);
    terrain::DetailBuildOptions options;
    options.maxSize = cap;
    terrain::DetailPixels pixels;
    if (!terrain::loadCookedDetailArray(plan, cap, pixels) && !terrain::buildDetailPixels(layers, plan, options, pixels)) return nullptr;
    std::vector<TextureLevel> levels(pixels.levels.size());
    for (size_t level = 0; level < pixels.levels.size(); level++) {
        levels[level].width = pixels.levelWidths[level];
        levels[level].height = pixels.levelHeights[level];
        levels[level].data = pixels.levels[level].data();
        levels[level].size = pixels.levels[level].size();
    }
    if (!entry.render.createTextureLevels("voxel|detail|" + std::to_string(owner), TextureType::TEXTURE_ARRAY, pixels.format, pixels.width, pixels.height,
            pixels.slices, levels, TextureFilter::LINEAR_MIPMAP_LINEAR, TextureFilter::LINEAR, TextureWrap::REPEAT, TextureWrap::REPEAT)) {
        return nullptr;
    }
    if (Engine::isAsyncThread()) {
        auto* keep = new std::vector<std::vector<uint8_t>>(std::move(pixels.levels));
        SystemRender::scheduleCleanup([](void* buffers) { delete static_cast<std::vector<std::vector<uint8_t>>*>(buffers); }, keep);
    }
    entry.failed = false;
    bound = &entry.render;
    return &entry;
}

bool RenderSystem::loadVoxelTextures(Entity owner, ObjectRender& render, ShaderData& shaderData) {
    const std::pair<int, int> slot = shaderData.getTextureIndex(TextureShaderType::VOXELDETAIL);
    if (slot.first == -1) return true;
    TextureRender* bound = nullptr;
    getVoxelDetailArray(owner, bound);
    render.addTexture(slot, ShaderStageType::FRAGMENT, (bound && bound->isCreated()) ? bound : &emptyArrayWhite);
    return true;
}

void RenderSystem::applyVoxelLayersUniform(ObjectRender& render, int slot, Entity owner, const Vector3& eye) {
    if (slot == -1) return;
    const VoxelVolumeComponent* volume = scene->findComponent<VoxelVolumeComponent>(owner);
    if (!volume) return;
    const auto found = voxelDetailArrays.find(owner);
    const VoxelDetailArray* detail = found != voxelDetailArrays.end() && !found->second.failed ? &found->second : nullptr;

    // R42: an attached volume's flat projection is the terrain's, with its origin, size and detail tiling
    float metresPerTile = 0.0f;
    float originX = 0.0f, originZ = 0.0f;
    if (volume->terrain != NULL_ENTITY && scene->isEntityCreated(volume->terrain)) {
        const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(volume->terrain);
        const Transform* terrainTransform = scene->findComponent<Transform>(volume->terrain);
        const Transform* volumeTransform = scene->findComponent<Transform>(owner);
        if (terrain && terrainTransform && volumeTransform && terrain->textureDetailTiles > 0) {
            metresPerTile = terrain->terrainSize / static_cast<float>(terrain->textureDetailTiles);
            originX = volumeTransform->worldPosition.x - terrainTransform->worldPosition.x + terrain->terrainSize * 0.5f;
            originZ = volumeTransform->worldPosition.z - terrainTransform->worldPosition.z + terrain->terrainSize * 0.5f;
        }
    }

    static const TerrainSurfaceLayer emptyLayer;
    fs_voxel_layers_t layers;
    for (size_t i = 0; i < 16; i++) {
        const bool present = i < volume->materialSlots.size();
        const TerrainSurfaceLayer& layer = present ? *slotLayer(scene, *volume, i) : emptyLayer;
        layers.colorFactor[i] = Vector4(layer.colorFactor.x, layer.colorFactor.y, layer.colorFactor.z, layer.pbr ? 1.0f : 0.0f);
        const float scaleX = metresPerTile > 0.0f ? layer.uvScale.x / metresPerTile : layer.uvScale.x;
        const float scaleY = metresPerTile > 0.0f ? layer.uvScale.y / metresPerTile : layer.uvScale.y;
        layers.uvTransform[i] = Vector4(scaleX, scaleY, originX * scaleX + layer.uvOffset.x, originZ * scaleY + layer.uvOffset.y);
        layers.surface[i] = Vector4(layer.roughnessFactor, layer.metallicFactor, layer.normalStrength, layer.occlusionStrength);
        // a colour layer leaves roughness and metallic to the terrain's material (terrain_fs.glsl's
        // getTerrainColor): a slot following one takes them from there too
        if (present && !layer.pbr && &layer != &volume->materialSlots[i].layer) {
            if (const MeshComponent* terrainMesh = scene->findComponent<MeshComponent>(volume->terrain)) {
                const Material& material = terrainMesh->submeshes[0].material;
                layers.surface[i].x = material.roughnessFactor;
                layers.surface[i].y = material.metallicFactor;
            }
        }
        layers.slices[i] = Vector4(detail ? static_cast<float>(std::max(detail->colorSlice[i], 0)) : 0.0f, detail ? static_cast<float>(detail->normalSlice[i]) : -1.0f,
                                   detail ? static_cast<float>(detail->surfaceSlice[i]) : -1.0f, 0.0f);
    }
    // the far tile break fades by distance to the eye, as the terrain's does: the clip w an orthographic
    // camera gives is 1 everywhere, which left the voxel ground sharper than the terrain around it
    const Transform* volumeTransform = scene->findComponent<Transform>(owner);
    const Vector3 localEye = volumeTransform ? volumeTransform->modelMatrix.inverse() * eye : eye;
    layers.eye = Vector4(localEye.x, localEye.y, localEye.z, 0.0f);
    render.applyUniformBlock(slot, sizeof(fs_voxel_layers_t), &layers);
}
