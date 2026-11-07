// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The terrain's material on the GPU (T3 D1): the render system's half of what engine/core/terrain
// decides, kept out of RenderSystem.cpp, which holds only the calls.

#include "RenderSystem.h"
#include "TerrainRenderInfo.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "object/TerrainEdit.h"
#include "profiler/Profile.h"
#include "render/SystemRender.h"
#include "texture/TerrainDetailBuilder.h"
#include "terrain/TerrainLayers.h"
#include "terrain/TerrainMacroNormal.h"
#include "texture/MaterialPbr.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <chrono>

using namespace jasperengine;

// the component's cap and the terrain folder's are one (T3 R15)
static_assert(MAX_TERRAIN_LAYERS == terrain::TERRAIN_MAX_LAYERS, "TerrainComponent.h and TerrainLayers.h disagree on the layer count");

// T3 R18, R20, R29: a variant with 'Thl' reads the hole map in whichever pass this binds. The loaded
// variant may outlive the map (the last fill, until its plain variant is built), and a map that fails
// to load or is not R8 has no texels for the CPU queries: both bind white, which is solid, so the
// drawn surface never disagrees with them.
bool RenderSystem::loadTerrainHoleTexture(TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData){
    const std::pair<int, int> slotTex = shaderData.getTextureIndex(TextureShaderType::TERRAINHOLES);
    if (slotTex.first == -1){
        return true;
    }

    TextureRender* textureRender = &emptyWhite;
    if (!terrain.holeMap.empty()){
        const ResourceLoadState state = prepareTerrainHoleMap(terrain);
        if (state == ResourceLoadState::Loading){
            return false;
        }
        const bool r8 = state == ResourceLoadState::Finished && terrain.holeMap.hasData() &&
                        terrain.holeMap.getData().getChannels() == 1 &&
                        TextureData::getBytesPerChannel(terrain.holeMap.getData().getColorFormat()) == 1;
        if (r8){
            textureRender = terrain.holeMap.getRender(&emptyWhite);
            if (!textureRender || !textureRender->isCreated()){
                return false;
            }
            // T3 R30: the Holes view reads the map's classes where the passes read 0 and 255. Editor only
            if (terrain::TerrainQualitySettings::getDebugView() == terrain::TerrainDebugView::HOLES){
                TextureData& data = terrain.holeMap.getData();
                const terrain::HoleMapView view{static_cast<const uint8_t*>(data.getData()), data.getWidth(), data.getHeight()};
                const int samples = terrain.heightMap.hasData() ? std::clamp(std::min(terrain.heightMap.getData().getWidth(), terrain.heightMap.getData().getHeight()), 4, 1025) : 4;
                std::vector<uint8_t> classes;
                terrain::holeOverlayClasses(view, samples, samples, classes);
                void* faces[1] = {classes.data()};
                size_t sizes[1] = {classes.size()};
                terrainHoleOverlay.destroyTexture();
                if (terrainHoleOverlay.createTexture("terrain|holeoverlay", data.getWidth(), data.getHeight(), ColorFormat::RED, TextureType::TEXTURE_2D, 1,
                                                     faces, sizes, TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)){
                    textureRender = &terrainHoleOverlay;
                }
            }
        }else if (!terrain.holeMapWarned){
            terrain.holeMapWarned = true;
            Log::warn("Terrain hole map '%s' failed to load or is not 8-bit single channel: it draws no holes", terrain.holeMap.getPath(0).c_str());
        }
    }

    render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
    return true;
}

terrain::TerrainVariantInputs jasperengine::getTerrainVariantInputs(const TerrainComponent& terrainComponent){
    terrain::TerrainVariantInputs inputs;
    switch (terrainComponent.quality){
        case TerrainQualitySetting::HIGH:
            inputs.hasComponentQuality = true;
            inputs.componentQuality = terrain::TerrainQuality::HIGH;
            break;
        case TerrainQualitySetting::MEDIUM:
            inputs.hasComponentQuality = true;
            inputs.componentQuality = terrain::TerrainQuality::MEDIUM;
            break;
        case TerrainQualitySetting::LOW:
            inputs.hasComponentQuality = true;
            inputs.componentQuality = terrain::TerrainQuality::LOW;
            break;
        default:
            break;
    }
    inputs.limitLayers = terrainComponent.limitLayersPerPixel;
    inputs.layerCount = static_cast<int>(terrainComponent.surfaceLayers.size());
    inputs.macro = terrainComponent.macroMode != TerrainMacroMode::OFF;
    inputs.macroNormal = terrainComponent.macroNormal;
    inputs.resample = terrainComponent.resampleEnabled;
    inputs.cliff = hasTerrainCliffLayers(terrainComponent);
    inputs.heightBlend = terrainComponent.heightBlendSharpness > 0.0f;
    return inputs;
}

terrain::TerrainQualityResolved RenderSystem::resolveTerrainTier(const MeshShaderEnvironment& environment, const terrain::TerrainVariantInputs& inputs){
    terrain::TerrainQualityRequest request;
    request.hasOverride = environment.terrainForced;
    request.globalOverride = environment.terrainForcedTier;
    request.hasComponent = inputs.hasComponentQuality;
    request.component = inputs.componentQuality;
    request.project = environment.terrainTier;
    request.shippedMask = environment.terrainShippedMask;
    return terrain::resolveTerrainQuality(request);
}

// T3 D2: the one rule of the defines a terrain's variant takes. Medium and Low never define the hex or
// stochastic paths; a feature a tier does not draw has no define there.
uint64_t RenderSystem::getTerrainDefineBits(const MeshShaderEnvironment& environment, const terrain::TerrainVariantInputs& inputs, bool surfacePath, bool unlit, bool hasNormals){
    // A terrain with no layer has nothing for a tier, the cap or a feature to act on but Macro Variation's
    // tint of the base colour, so it draws the base material alone: llvmpipe's GL path (Mesa 23.2.1) dies
    // in its fragment JIT on every tier and feature variant of a terrain without layers (spec Deviations)
    if (inputs.layerCount == 0 && environment.terrainDebugView == 0){
        return 0;
    }
    const terrain::TerrainQuality tier = resolveTerrainTier(environment, inputs).quality;
    const terrain::TerrainTierInfo& info = terrain::terrainTierInfo(tier);
    const auto bit = [](const char* define){ return MaterialPbr::getDefineBit(define); };
    uint64_t bits = 0;

    if (tier == terrain::TerrainQuality::MEDIUM) bits |= bit("TERRAIN_QUALITY_MEDIUM");
    if (tier == terrain::TerrainQuality::LOW) bits |= bit("TERRAIN_QUALITY_LOW");
    // A cap that keeps every layer is no cap: the same image without the selection. And llvmpipe's GL path
    // (Mesa 23.2) dies on an illegal instruction in the colour-only High variant with the cap, which every
    // new terrain (no layers, R53's limit on) would draw (spec Deviations)
    if (tier == terrain::TerrainQuality::HIGH && inputs.limitLayers && inputs.layerCount > info.limitedLayersPerPixel) bits |= bit("TERRAIN_LAYER_LIMIT");
    if (info.trimBlendMaps){
        const int maps = terrain::terrainBlendMapCount(inputs.layerCount);
        if (maps <= 1) bits |= bit("TERRAIN_BLENDMAPS_1");
        else if (maps == 2) bits |= bit("TERRAIN_BLENDMAPS_2");
    }
    if (inputs.layerCount > TERRAIN_SHADER_BASE_LAYERS) bits |= bit("TERRAIN_EXTENDED_LAYERS");
    if (surfacePath){
        if (inputs.hex && info.hexAndStochastic){
            bits |= bit(MaterialPbr::TERRAIN_HEX_DEFINE);
            if (inputs.hexStochastic) bits |= bit(MaterialPbr::TERRAIN_HEX_STOCHASTIC_DEFINE);
        }
        if (inputs.macro) bits |= bit("TERRAIN_MACRO");
        if (inputs.macroNormal && info.macroNormal && !unlit && hasNormals) bits |= bit("TERRAIN_MACRO_NORMAL");
        if (inputs.resample && info.distanceResampling) bits |= bit("TERRAIN_RESAMPLE");
        if (inputs.cliff) bits |= bit("TERRAIN_CLIFF");
        if (inputs.heightBlend) bits |= bit("TERRAIN_HEIGHT_BLEND");
        if (environment.terrainDebugView != 0) bits |= bit("TERRAIN_DEBUG_VIEW");
    }
    return bits;
}

// Read here rather than through Texture, which would upload a 2D image per layer on top of the array.
// Rebuilt when the set of source files changes, when a file of it does (invalidateTerrainDetailArray:
// the editor's file pick-up), and when the tier changes its largest size.
RenderSystem::TerrainDetailArray* RenderSystem::getTerrainDetailArray(Entity entity, TerrainComponent& terrainComponent){
    const terrain::DetailPlan plan = terrain::planDetailArray(terrainComponent);
    if (!plan.anySource){
        destroyTerrainDetailArray(entity);
        return NULL;
    }

    // One sampler covers every slice, so the first assigned layer sets it for all of them
    const size_t layerCount = std::min(terrainComponent.surfaceLayers.size(), (size_t)MAX_TERRAIN_LAYERS);
    const Texture* sampled = &terrainComponent.surfaceLayers[0].colorTexture;
    for (size_t i = 0; i < layerCount; i++){
        if (!terrainComponent.surfaceLayers[i].colorTexture.empty()){
            sampled = &terrainComponent.surfaceLayers[i].colorTexture;
            break;
        }
    }

    // Tiled surface maps alias badly at distance without mipmaps. A color-only terrain
    // keeps whatever filter its layer textures ask for, so its look is unchanged.
    TextureFilter minFilter = sampled->getMinFilter();
    if (minFilter == TextureFilter::LINEAR && hasTerrainSurfaceLayers(terrainComponent)){
        minFilter = TextureFilter::LINEAR_MIPMAP_LINEAR;
    }

    const MeshShaderEnvironment environment = getMeshShaderEnvironment();
    const int maxSize = terrain::terrainTierInfo(resolveTerrainTier(environment, getTerrainVariantInputs(terrainComponent)).quality).detailArraySize;

    auto it = terrainDetailArrays.find(entity);
    if (it != terrainDetailArrays.end()){
        const TerrainDetailArray& cached = it->second;
        if (cached.sliceKeys == plan.sliceKeys && cached.minFilter == minFilter && cached.magFilter == sampled->getMagFilter() &&
            cached.wrapU == sampled->getWrapU() && cached.wrapV == sampled->getWrapV() && cached.maxSize == maxSize){
            // A set that could not be built is remembered, not retried every frame
            return cached.failed ? NULL : &it->second;
        }
        destroyTerrainDetailArray(entity);
    }

    auto remember = [&](bool failed) -> TerrainDetailArray& {
        TerrainDetailArray& entry = terrainDetailArrays[entity];
        entry.sliceKeys = plan.sliceKeys;
        entry.colorSlices = plan.colorSlices;
        entry.normalSlice = plan.normalSlice;
        entry.surfaceSlice = plan.surfaceSlice;
        entry.minFilter = minFilter;
        entry.magFilter = sampled->getMagFilter();
        entry.wrapU = sampled->getWrapU();
        entry.wrapV = sampled->getWrapV();
        entry.maxSize = maxSize;
        entry.failed = failed;
        return entry;
    };

    terrain::DetailBuildOptions options;
    options.mipmapped = minFilter == TextureFilter::LINEAR_MIPMAP_LINEAR || minFilter == TextureFilter::LINEAR_MIPMAP_NEAREST ||
                        minFilter == TextureFilter::NEAREST_MIPMAP_LINEAR || minFilter == TextureFilter::NEAREST_MIPMAP_NEAREST;
    options.maxSize = maxSize;
    terrain::DetailPixels pixels;
    if (!terrain::loadCookedDetailArray(plan, maxSize, pixels) && !terrain::buildDetailPixels(terrainComponent, plan, options, pixels)){
        remember(true);
        return NULL;
    }

    std::vector<TextureLevel> levels(pixels.levels.size());
    for (size_t level = 0; level < pixels.levels.size(); level++){
        levels[level].width = pixels.levelWidths[level];
        levels[level].height = pixels.levelHeights[level];
        levels[level].data = pixels.levels[level].data();
        levels[level].size = pixels.levels[level].size();
    }

    TerrainDetailArray& detail = remember(false);
    detail.format = pixels.format;
    detail.bytes = 0;
    for (const std::vector<uint8_t>& level : pixels.levels){
        detail.bytes += level.size();
    }
    if (!detail.render.createTextureLevels("terrain|detail|" + std::to_string(entity), TextureType::TEXTURE_ARRAY,
            pixels.format, pixels.width, pixels.height, pixels.slices, levels,
            minFilter, sampled->getMagFilter(), sampled->getWrapU(), sampled->getWrapV())){
        detail.failed = true;
        return NULL;
    }
    // sokol copied the levels, or the queue holds them until it runs
    if (Engine::isAsyncThread()){
        auto* keep = new std::vector<std::vector<uint8_t>>(std::move(pixels.levels));
        SystemRender::scheduleCleanup([](void* buffers){ delete static_cast<std::vector<std::vector<uint8_t>>*>(buffers); }, keep);
    }

    return &detail;
}

// Factors change without rebuilding the array, so the block is filled at draw time. T3 R33: the arrays
// hold 9 entries or 12 and the features' parameters end the block, in the order of terrain_fs.glsl, so
// a variant uploads exactly the block it declares.
void RenderSystem::applyTerrainLayersUniform(ObjectRender& render, int slot, Entity entity, TerrainComponent& terrainComponent, uint64_t litDefineBits, bool extendedBlock){
    if (slot == -1){
        return;
    }

    auto it = terrainDetailArrays.find(entity);
    const TerrainDetailArray* detail = (it != terrainDetailArrays.end() && !it->second.failed) ? &it->second : NULL;

    static const TerrainSurfaceLayer emptyLayer;

    const int entries = extendedBlock ? MAX_TERRAIN_LAYERS : TERRAIN_SHADER_BASE_LAYERS;
    Vector4 block[5 * MAX_TERRAIN_LAYERS + 3 + 1 + 3 + 1 + 1 + 1];
    for (int i = 0; i < entries; i++){
        const bool present = i < (int)terrainComponent.surfaceLayers.size();
        const TerrainSurfaceLayer& layer = present ? terrainComponent.surfaceLayers[i] : emptyLayer;
        const bool pbr = present && layer.pbr;

        block[i] = Vector4(layer.colorFactor.x, layer.colorFactor.y, layer.colorFactor.z, pbr ? 1.0f : 0.0f);
        block[entries + i] = Vector4(layer.uvScale.x, layer.uvScale.y, layer.uvOffset.x, layer.uvOffset.y);
        block[entries * 2 + i] = Vector4(layer.roughnessFactor, layer.metallicFactor, layer.normalStrength, layer.occlusionStrength);
        block[entries * 3 + i] = Vector4((float)i,
                                         detail ? (float)detail->normalSlice[i] : -1.0f,
                                         detail ? (float)detail->surfaceSlice[i] : -1.0f,
                                         // the hex rotation of a hex tiled layer, < 0 for the others (TERRAIN_HEX_TILING)
                                         (present && layer.hexTiling) ? std::clamp(layer.hexRotation, 0.0f, 1.0f) : -1.0f);
        // The user values as they are (includes/stochastic.glsl maps them), a cell size of 0
        // turning the layer's stochastic branch off
        block[entries * 4 + i] = Vector4((present && layer.stochasticTiling) ? layer.stochasticCellSize : 0.0f,
                                         layer.stochasticBlendSharpness, layer.stochasticContrast, layer.stochasticRotation);
    }

    int size = entries * 5;
    if (litDefineBits & MaterialPbr::getDefineBit("TERRAIN_MACRO")){
        const float scale = terrainComponent.macroScale > 0.0f ? terrainComponent.macroScale : 1.0f;
        block[size++] = Vector4(1.0f / scale, terrainComponent.macroStrength, terrainComponent.macroSlopeFade,
                                terrainComponent.macroMode == TerrainMacroMode::BASE_MAP ? 1.0f : 0.0f);
        block[size++] = Vector4(terrainComponent.macroTintA.x, terrainComponent.macroTintA.y, terrainComponent.macroTintA.z, terrainComponent.terrainSize);
        block[size++] = Vector4(terrainComponent.macroTintB.x, terrainComponent.macroTintB.y, terrainComponent.macroTintB.z, 0.0f);
    }
    if (litDefineBits & MaterialPbr::getDefineBit("TERRAIN_RESAMPLE")){
        // metres to the detail tiles v_terrainEyeTiles counts in
        const float tiles = terrainComponent.terrainSize > 0.0f ? terrainComponent.textureDetailTiles / terrainComponent.terrainSize : 0.0f;
        const float nearTiles = terrainComponent.resampleNear * tiles;
        block[size++] = Vector4(nearTiles, std::max(terrainComponent.resampleFar * tiles, nearTiles + 0.0001f), terrainComponent.resampleReduction, 0.0f);
    }
    if (litDefineBits & MaterialPbr::getDefineBit("TERRAIN_CLIFF")){
        for (int v = 0; v < 3; v++){
            float flags[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int c = 0; c < 4; c++){
                const size_t layer = (size_t)(v * 4 + c);
                flags[c] = layer < terrainComponent.surfaceLayers.size() && terrainComponent.surfaceLayers[layer].cliffProjection == TerrainCliffProjection::FLAT_ONLY ? 1.0f : 0.0f;
            }
            block[size++] = Vector4(flags[0], flags[1], flags[2], flags[3]);
        }
    }
    if (litDefineBits & MaterialPbr::getDefineBit("TERRAIN_HEIGHT_BLEND")){
        block[size++] = Vector4(terrainComponent.heightBlendSharpness, 0.0f, 0.0f, 0.0f);
    }
    if (litDefineBits & MaterialPbr::getDefineBit("TERRAIN_MACRO_NORMAL")){
        auto macro = terrainMacroNormals.find(entity);
        block[size++] = Vector4(macro != terrainMacroNormals.end() && macro->second.render.isCreated() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (litDefineBits & MaterialPbr::getDefineBit("TERRAIN_DEBUG_VIEW")){
        block[size++] = Vector4((float)terrain::TerrainQualitySettings::getDebugView(), 0.0f, 0.0f, 0.0f);
    }

    render.applyUniformBlock(slot, sizeof(Vector4) * (size_t)size, block);
}

// Low's Macro Variation is the vertex stage's (T3 R9): its block, in u_vs_terrainMacro
void RenderSystem::applyTerrainMacroVertexUniform(ObjectRender& render, int slot, const TerrainComponent& terrainComponent){
    if (slot == -1){
        return;
    }
    const float scale = terrainComponent.macroScale > 0.0f ? terrainComponent.macroScale : 1.0f;
    Vector4 block[3] = {
        Vector4(1.0f / scale, terrainComponent.macroStrength, terrainComponent.macroSlopeFade, 0.0f),
        Vector4(terrainComponent.macroTintA.x, terrainComponent.macroTintA.y, terrainComponent.macroTintA.z, 0.0f),
        Vector4(terrainComponent.macroTintB.x, terrainComponent.macroTintB.y, terrainComponent.macroTintB.z, 0.0f),
    };
    render.applyUniformBlock(slot, sizeof(block), block);
}

// T3 R14: the terrain-wide normal map, baked from the height texels off the main thread where there are
// workers, and by rows inside the frame's slice on the single-thread web build. Until it is ready its
// strength in the block is 0 and vertex normals draw. A dirty rectangle bakes its rows again, uploaded at
// most 4 times a second.
void RenderSystem::updateTerrainMacroNormal(Entity entity, TerrainComponent& terrainComponent, const MeshComponent& mesh){
    const bool wanted = mesh.loaded && mesh.numSubmeshes > 0 && (mesh.submeshes[0].litDefineBits & MaterialPbr::getDefineBit("TERRAIN_MACRO_NORMAL")) != 0;
    auto it = terrainMacroNormals.find(entity);
    if (!wanted){
        if (it != terrainMacroNormals.end()){
            it->second.render.destroyTexture();
            terrainMacroNormals.erase(it);
        }
        return;
    }
    if (terrainComponent.heightMap.empty() || terrainComponent.heightMap.isFramebuffer() || !terrainComponent.heightMap.hasData()){
        return;
    }
    TextureData& heights = terrainComponent.heightMap.getData();
    if (!heights.getData()){
        return;
    }

    TerrainMacroNormalMap& map = terrainMacroNormals[entity];
    const uint64_t heightRevision = terrainComponent.heightMap.getRender(&emptyWhite) ? terrainComponent.heightMap.getRender(&emptyWhite)->getRevision() : 0;
    const double now = Engine::getSystemTime();

    if (!map.work && (!map.render.isCreated() || (map.bakedRevision != heightRevision && now - map.lastUpload >= 0.25))){
        // the job's own copy: the map may change on the main thread while it runs
        auto work = std::make_shared<TerrainMacroNormalWork>();
        work->pixels.assign(static_cast<const uint8_t*>(heights.getData()),
                            static_cast<const uint8_t*>(heights.getData()) + (size_t)heights.getWidth() * heights.getHeight() * heights.getChannels() * TextureData::getBytesPerChannel(heights.getColorFormat()));
        work->source.width = heights.getWidth();
        work->source.height = heights.getHeight();
        work->source.channels = heights.getChannels();
        work->source.bytesPerChannel = TextureData::getBytesPerChannel(heights.getColorFormat());
        work->source.terrainSize = terrainComponent.terrainSize;
        work->source.maxHeight = terrainComponent.maxHeight;
        work->revision = heightRevision;
        work->source.pixels = work->pixels.data();
        terrain::MacroNormalLevel top;
        top.width = terrain::macroNormalSize(work->source.width);
        top.height = terrain::macroNormalSize(work->source.height);
        top.rg.resize((size_t)top.width * top.height * 2);
        work->levels.push_back(std::move(top));
        map.work = work;
        if (jobs::hasSystem() && jobs::system().getWorkerCount() > 0){
            TerrainMacroNormalWork* raw = work.get();
            work->task = jobs::submit([raw](){
                terrain::MacroNormalLevel& level = raw->levels[0];
                terrain::bakeMacroNormalRows(raw->source, level.width, level.height, 0, level.height, level.rg.data());
                terrain::buildMacroNormalMips(raw->levels);
                raw->done.store(true, std::memory_order_release);
            }, {jobs::Lane::Background, jobs::GateClass::Default, "terrain-macro-normal"});
            work->threaded = true;
        }
    }

    if (map.work){
        TerrainMacroNormalWork& work = *map.work;
        if (!work.threaded){
            // S06's slice: as many rows as 4 ms holds this frame
            const auto start = std::chrono::steady_clock::now();
            terrain::MacroNormalLevel& level = work.levels[0];
            while (work.nextRow < level.height && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(4)){
                const int end = std::min(work.nextRow + 16, level.height);
                terrain::bakeMacroNormalRows(work.source, level.width, level.height, work.nextRow, end, level.rg.data());
                work.nextRow = end;
            }
            if (work.nextRow >= level.height){
                terrain::buildMacroNormalMips(work.levels);
                work.done.store(true, std::memory_order_release);
            }
        }
        if (work.done.load(std::memory_order_acquire)){
            std::vector<TextureLevel> levels(work.levels.size());
            for (size_t l = 0; l < work.levels.size(); l++){
                levels[l].width = work.levels[l].width;
                levels[l].height = work.levels[l].height;
                levels[l].data = work.levels[l].rg.data();
                levels[l].size = work.levels[l].rg.size();
            }
            map.render.destroyTexture();
            map.render.createTextureLevels("terrain|macronormal|" + std::to_string(entity), TextureType::TEXTURE_2D, GpuTextureFormat::RG8,
                                           work.levels[0].width, work.levels[0].height, 1, levels,
                                           TextureFilter::LINEAR_MIPMAP_LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE);
            map.bakedRevision = work.revision;
            map.lastUpload = now;
            map.work.reset();
            terrainComponent.needUpdateTexture = true;
        }
    }
}

void RenderSystem::destroyTerrainMacroNormal(Entity entity){
    auto it = terrainMacroNormals.find(entity);
    if (it != terrainMacroNormals.end()){
        it->second.render.destroyTexture();
        terrainMacroNormals.erase(it);
    }
}

// T3 R17: terrain outside the view casts (Auto: the tier's); RenderSystem::shadowTerrainCut selects it
bool RenderSystem::terrainDrawsOffscreenShadows(const TerrainComponent& terrainComponent) const{
    switch (terrainComponent.offscreenShadows){
        case TerrainOffscreenShadows::ON: return true;
        case TerrainOffscreenShadows::OFF: return false;
        default: break;
    }
    const MeshShaderEnvironment environment = getMeshShaderEnvironment();
    return terrain::terrainTierInfo(resolveTerrainTier(environment, getTerrainVariantInputs(terrainComponent)).quality).offscreenShadowsAuto;
}

bool RenderSystem::getTerrainDetailInfo(Entity entity, GpuTextureFormat& format, int& slices, uint64_t& bytes) const{
    auto it = terrainDetailArrays.find(entity);
    if (it == terrainDetailArrays.end() || it->second.failed){
        return false;
    }
    format = it->second.format;
    slices = static_cast<int>(it->second.sliceKeys.size());
    bytes = it->second.bytes;
    return true;
}

bool jasperengine::getTerrainDetailArrayInfo(Scene* scene, Entity entity, GpuTextureFormat& format, int& slices, uint64_t& bytes){
    std::shared_ptr<RenderSystem> render = scene ? scene->getSystem<RenderSystem>() : nullptr;
    return render && render->getTerrainDetailInfo(entity, format, slices, bytes);
}

bool RenderSystem::isTerrainMacroNormalReady(Entity entity){
    auto it = terrainMacroNormals.find(entity);
    return it != terrainMacroNormals.end() && it->second.render.isCreated();
}

