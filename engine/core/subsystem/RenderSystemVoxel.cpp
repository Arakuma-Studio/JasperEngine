// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R43, R50: a volume's slots as one texture array and one uniform block. New file, as a hook in
// RenderSystem.cpp would be neither small nor rare (D2).

#include "subsystem/RenderSystem.h"

#include "Log.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/VoxelVolumeComponent.h"
#include "render/SystemRender.h"
#include "texture/MipChain.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cmath>
#include <map>

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

    std::string sourceKey(const Texture& texture) {
        const std::string path = texture.getPath(0);
        return path;
    }

    unsigned char channelOf(TextureData* source, size_t texel, int channel, unsigned char fallback) {
        if (!source || !source->getData()) return fallback;
        const int channels = source->getChannels();
        if (channel == 3 && channels < 4) return fallback;
        const auto* pixels = static_cast<const unsigned char*>(source->getData());
        return pixels[texel * static_cast<size_t>(channels) + static_cast<size_t>(std::min(channel, channels - 1))];
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

    std::vector<std::string> sliceKeys;
    std::array<int, 16> colorSlice, normalSlice, surfaceSlice;
    colorSlice.fill(-1);
    normalSlice.fill(-1);
    surfaceSlice.fill(-1);
    std::array<const TerrainSurfaceLayer*, 16> layers{};
    bool anySource = false;
    for (size_t i = 0; i < count; i++) {
        const TerrainSurfaceLayer* layer = slotLayer(scene, *volume, i);
        layers[i] = layer;
        const std::string color = sourceKey(layer->colorTexture);
        anySource = anySource || !color.empty();
        colorSlice[i] = static_cast<int>(sliceKeys.size());
        sliceKeys.push_back(color);
    }
    for (size_t i = 0; i < count; i++) {
        const TerrainSurfaceLayer* layer = layers[i];
        if (!layer->pbr) continue;
        if (!layer->normalTexture.empty()) {
            normalSlice[i] = static_cast<int>(sliceKeys.size());
            sliceKeys.push_back("normal|" + sourceKey(layer->normalTexture));
            anySource = true;
        }
        // R50: no surface slices at Low
        const bool surface = !layer->occlusionTexture.empty() || !layer->roughnessTexture.empty() || !layer->metallicTexture.empty() || !layer->heightTexture.empty();
        if (surface && tier != 3) {
            surfaceSlice[i] = static_cast<int>(sliceKeys.size());
            sliceKeys.push_back("surface|" + sourceKey(layer->occlusionTexture) + "|" + sourceKey(layer->roughnessTexture) + "|" + sourceKey(layer->metallicTexture) + "|" + sourceKey(layer->heightTexture));
            anySource = true;
        }
    }
    if (!anySource) return nullptr;

    auto found = voxelDetailArrays.find(owner);
    if (found != voxelDetailArrays.end() && !found->second.sharesTerrain && found->second.sliceKeys == sliceKeys) {
        if (found->second.failed) return nullptr;
        bound = &found->second.render;
        return &found->second;
    }
    if (found != voxelDetailArrays.end()) {
        if (!found->second.sharesTerrain) found->second.render.destroyTexture();
        voxelDetailArrays.erase(found);
    }

    // The largest source, under the tier's cap
    std::map<std::string, TextureData> sources;
    int width = 0, height = 0;
    const auto source = [&](const Texture& texture) -> TextureData* {
        const std::string key = sourceKey(texture);
        if (key.empty()) return nullptr;
        auto entry = sources.find(key);
        if (entry == sources.end()) {
            entry = sources.emplace(key, TextureData()).first;
            if (!entry->second.loadTextureFromFile(key.c_str())) return nullptr;
            entry->second.setDataOwned(true);
            width = std::max(width, entry->second.getWidth());
            height = std::max(height, entry->second.getHeight());
        }
        return entry->second.getData() ? &entry->second : nullptr;
    };
    struct Slice {
        int kind = 0;   // 0 colour, 1 normal, 2 surface
        TextureData* color = nullptr;
        TextureData* normal = nullptr;
        TextureData* occlusion = nullptr;
        TextureData* roughness = nullptr;
        TextureData* metallic = nullptr;
        TextureData* heightMap = nullptr;
    };
    std::vector<Slice> slices(sliceKeys.size());
    for (size_t i = 0; i < count; i++) {
        const TerrainSurfaceLayer* layer = layers[i];
        slices[static_cast<size_t>(colorSlice[i])].color = source(layer->colorTexture);
        if (normalSlice[i] >= 0) {
            Slice& slice = slices[static_cast<size_t>(normalSlice[i])];
            slice.kind = 1;
            slice.normal = source(layer->normalTexture);
        }
        if (surfaceSlice[i] >= 0) {
            Slice& slice = slices[static_cast<size_t>(surfaceSlice[i])];
            slice.kind = 2;
            slice.occlusion = source(layer->occlusionTexture);
            slice.roughness = source(layer->roughnessTexture);
            slice.metallic = source(layer->metallicTexture);
            slice.heightMap = source(layer->heightTexture);
        }
    }

    VoxelDetailArray& entry = voxelDetailArrays[owner];
    entry.sliceKeys = sliceKeys;
    entry.colorSlice = colorSlice;
    entry.normalSlice = normalSlice;
    entry.surfaceSlice = surfaceSlice;
    entry.sharesTerrain = false;
    entry.failed = true;
    if (width == 0) return nullptr;
    const int cap = maxArraySize(tier);
    width = std::min(width, cap);
    height = std::min(height, cap);
    for (auto& item : sources) {
        if (item.second.getData() && (item.second.getWidth() != width || item.second.getHeight() != height)) item.second.resize(width, height);
    }

    const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<std::vector<MipLevel>> sliceLevels(slices.size());
    for (size_t s = 0; s < slices.size(); s++) {
        const Slice& slice = slices[s];
        std::vector<unsigned char> pixels(texels * 4, 0xFF);
        for (size_t t = 0; t < texels; t++) {
            unsigned char* texel = &pixels[t * 4];
            if (slice.kind == 1) {
                texel[0] = channelOf(slice.normal, t, 0, 0x80);
                texel[1] = channelOf(slice.normal, t, 1, 0x80);
                texel[2] = channelOf(slice.normal, t, 2, 0xFF);
            } else if (slice.kind == 2) {
                texel[0] = channelOf(slice.occlusion, t, 0, 0xFF);
                texel[1] = channelOf(slice.roughness, t, 1, 0xFF);
                texel[2] = channelOf(slice.metallic, t, 2, 0xFF);
                texel[3] = channelOf(slice.heightMap, t, 0, 0xFF);
            } else {
                for (int c = 0; c < 4; c++) texel[c] = channelOf(slice.color, t, c, 0xFF);
            }
        }
        MipChainOptions options;
        options.maxLevels = 0;
        options.wrap = true;
        MipLevel base;
        base.width = width;
        base.height = height;
        if (slice.kind == 1) {
            options.channels = 3;
            options.type = MipPixelType::F32;
            options.normalMap = true;
            base.pixels.resize(texels * 3 * sizeof(float));
            float* v = reinterpret_cast<float*>(base.pixels.data());
            for (size_t t = 0; t < texels * 3; t++) v[t] = pixels[(t / 3) * 4 + t % 3] / 255.0f * 2.0f - 1.0f;
        } else {
            options.srgb = slice.kind == 0;
            options.alphaWeighted = slice.kind == 0;
            base.pixels = std::move(pixels);
        }
        sliceLevels[s].push_back(std::move(base));
        MipChain::build(sliceLevels[s], options);
        if (slice.kind == 1) {
            for (MipLevel& level : sliceLevels[s]) {
                const size_t n = static_cast<size_t>(level.width) * static_cast<size_t>(level.height);
                const float* v = reinterpret_cast<const float*>(level.pixels.data());
                std::vector<uint8_t> rgba(n * 4, 0xFF);
                for (size_t t = 0; t < n; t++) {
                    for (int c = 0; c < 3; c++) {
                        rgba[t * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround((v[t * 3 + static_cast<size_t>(c)] * 0.5f + 0.5f) * 255.0f)), 0, 255));
                    }
                }
                level.pixels = std::move(rgba);
            }
        }
    }
    std::vector<std::vector<uint8_t>> combined(sliceLevels[0].size());
    std::vector<TextureLevel> levels(combined.size());
    for (size_t level = 0; level < combined.size(); level++) {
        for (const std::vector<MipLevel>& slice : sliceLevels) combined[level].insert(combined[level].end(), slice[level].pixels.begin(), slice[level].pixels.end());
        levels[level].width = sliceLevels[0][level].width;
        levels[level].height = sliceLevels[0][level].height;
        levels[level].data = combined[level].data();
        levels[level].size = combined[level].size();
    }
    if (!entry.render.createTextureLevels("voxel|detail|" + std::to_string(owner), TextureType::TEXTURE_ARRAY, GpuTextureFormat::RGBA8, width, height,
            static_cast<int>(slices.size()), levels, TextureFilter::LINEAR_MIPMAP_LINEAR, TextureFilter::LINEAR, TextureWrap::REPEAT, TextureWrap::REPEAT)) {
        return nullptr;
    }
    if (Engine::isAsyncThread()) {
        auto* keep = new std::vector<std::vector<uint8_t>>(std::move(combined));
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
