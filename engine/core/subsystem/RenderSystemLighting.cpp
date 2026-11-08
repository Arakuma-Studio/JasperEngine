// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S57's lighting features in the renderer (docs/specs/S57-forge-lighting.md): the screen-tile
// light lists (Light Mode Tiled), capsule shadows, contact shadows and weighted blended OIT.
// Hooked from RenderSystem.cpp's update, load and draw paths; each costs nothing while off.

#include "RenderSystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "jobs/JobSystem.h"
#include "pool/ShaderPool.h"
#include "render/BlueNoise64.h"
#include "render/ContactShadows.h"
#include "render/OrderIndependentTransparency.h"
#include "render/SystemRender.h"
#include "shader/ShaderManifest.h"
#include "texture/MaterialPbr.h"
#include "profiler/Profile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {
    uint64_t lightingBit(ShaderPool::LightingFeature feature){
        return ShaderPool::getLightingProperty(feature);
    }

    float lightingLuminance(const Vector4& colorIntensity){
        return (0.2126f * colorIntensity.x + 0.7152f * colorIntensity.y + 0.0722f * colorIntensity.z) * colorIntensity.w;
    }

    // the target texels a viewport starts at in gl_FragCoord, along y: GL's from the bottom
    // (sokol's viewport origin), the other backends' from the top
    float fragCoordRowStart(const Rect& viewport, unsigned int targetHeight){
        return Engine::isOpenGL() ? viewport.getY() : (float)targetHeight - viewport.getY() - viewport.getHeight();
    }

    // the frames a feature's target is kept unused before it is freed
    constexpr uint64_t TARGET_IDLE_DRAWS = 120;
}

//-----What the variants take-----

void RenderSystem::updateLightingFeatures(){
    // the last frame's OIT draws, for the stats
    lighting.oitDraws = lighting.oitDrawsCounting;
    lighting.oitDrawsCounting = 0;
    lighting.tier = LightingTiers::getTier();
    LightingTiers::tileGrid(lighting.tier, lighting.tileColumns, lighting.tileRows);
    lighting.contactDivisor = LightingTiers::contactShadowDivisor(lighting.tier);

    // by the Light components (enabled or not, active or not): a light switched on and off
    // does not reload every variant
    auto lights = scene->getComponentArray<LightComponent>();
    bool localLights = false;
    bool shadowedSun = false;
    bool contactLight = false;
    for (size_t i = 0; i < lights->size(); i++){
        const LightComponent& light = lights->getComponentFromIndex(i);
        if (light.type == LightType::DIRECTIONAL){
            shadowedSun = shadowedSun || light.shadows;
        }else{
            localLights = true;
        }
        if (light.shadows && light.contactShadows && light.type != LightType::POINT)
            contactLight = true;
    }

    // Tiled draws point and spot lights per tile: a scene of suns keeps the object lists
    const bool tiled = hasLights && localLights && LightingTiers::usesTiledLights();
    const bool capsules = hasLights && hasShadows && shadowedSun && scene->isCapsuleShadowsEnabled() &&
                          scene->getComponentArray<CapsuleShadowCasterComponent>()->size() > 0;
    const bool contact = hasLights && hasShadows && contactLight && lighting.contactDivisor > 0;

    // a camera asking for Order Independent where the device cannot draws Sorted (warned once)
    bool oitRequested = false;
    auto cameras = scene->getComponentArray<CameraComponent>();
    for (size_t i = 0; i < cameras->size(); i++){
        const CameraComponent& camera = cameras->getComponentFromIndex(i);
        if (camera.transparencyMode == TransparencyMode::ORDER_INDEPENDENT)
            oitRequested = true;
    }
    const bool oitSupported = OrderIndependentTransparency::isSupported(msaaPipelineSamples);
    lighting.oitFallback = oitRequested && !oitSupported;
    if (lighting.oitFallback && !lighting.oitFallbackWarned){
        lighting.oitFallbackWarned = true;
        Log::warn("Transparency Mode Order Independent needs renderable and blendable RGBA16F and R16F targets%s, which this device "
                  "does not have: its cameras draw Sorted", msaaPipelineSamples > 1 ? " with MSAA" : "");
    }
    const bool oit = oitRequested && oitSupported;

    if (tiled != lighting.tiledLights || capsules != lighting.capsuleShadows || contact != lighting.contactShadows || oit != lighting.oit){
        lighting.tiledLights = tiled;
        lighting.capsuleShadows = capsules;
        lighting.contactShadows = contact;
        lighting.oit = oit;
        needReloadMeshVariants();
    }
    releaseLightingFeatures(false);
    requestLightingPasses();
}

void RenderSystem::requestLightingPasses(){
    // asked again each update until created: a program the runtime compiler builds in the
    // background is created by the get() after it is done (ShaderPool::get), not by its handle
    if (lighting.contactShadows && !(lighting.contactShader && lighting.contactShader->isCreated()))
        lighting.contactShader = ShaderPool::get(ShaderType::LIGHTING_PASS, 0);
    if (lighting.oit && !(lighting.resolveShader && lighting.resolveShader->isCreated()))
        lighting.resolveShader = ShaderPool::get(ShaderType::LIGHTING_PASS, 1);
}

bool RenderSystem::lightingPassesLoading() const{
    // a program that cannot build (not in the build, or failed) is settled: its pass draws nothing
    auto pending = [](const std::shared_ptr<ShaderRender>& shader, uint64_t properties){
        return shader && !shader->isCreated() && !ShaderPool::isShaderBuildFailed(ShaderType::LIGHTING_PASS, properties, 0);
    };
    return (lighting.contactShadows && pending(lighting.contactShader, 0)) || (lighting.oit && pending(lighting.resolveShader, 1));
}

RenderSystem::LightingStats RenderSystem::getLightingStats() const{
    LightingStats stats;
    stats.lightMode = LightingTiers::getLightMode();
    stats.tier = lighting.tier;
    stats.tiledLights = lighting.tiledLights;
    stats.tileColumns = lighting.tileColumns;
    stats.tileRows = lighting.tileRows;
    stats.tileStatsValid = lighting.tiledLights && lighting.tileStatsValid;
    stats.tiles = lighting.tileStats;
    stats.capsuleShadows = lighting.capsuleShadows;
    stats.capsuleCasters = lighting.capsuleCasters;
    stats.capsules = (int)lighting.capsules.size();
    stats.contactShadows = lighting.contactShadows;
    stats.contactWidth = lighting.contactShadows ? lighting.contactWidth : 0;
    stats.contactHeight = lighting.contactShadows ? lighting.contactHeight : 0;
    stats.oit = lighting.oit;
    stats.oitFallback = lighting.oitFallback;
    stats.oitDraws = lighting.oitDraws;
    return stats;
}

void RenderSystem::releaseLightingFeatures(bool all){
    auto releaseViewTargets = [&](RenderView& view){
        if (!view.lightingTargets)
            return;
        LightingViewTargets& targets = *view.lightingTargets;
        if ((all || !lighting.contactShadows || targets.contactUsed + TARGET_IDLE_DRAWS < drawCount) && targets.contactMask.isCreated()){
            targets.contactMask.destroyFramebuffer();
            targets.contactWidth = 0;
            targets.contactHeight = 0;
        }
        if ((all || !lighting.oit || targets.oitUsed + TARGET_IDLE_DRAWS < drawCount) && targets.oit.isCreated()){
            targets.oit.destroyFramebuffer();
            targets.oitWidth = 0;
            targets.oitHeight = 0;
        }
        if (!targets.contactMask.isCreated() && !targets.oit.isCreated())
            view.lightingTargets.reset();
    };
    releaseViewTargets(primaryView);
#ifdef JASPERENGINE_EDITOR
    for (auto& entry : secondaryViews)
        releaseViewTargets(*entry.second);
#endif

    if (all || !lighting.tiledLights){
        primaryView.shadows.tiledLights.reset();
#ifdef JASPERENGINE_EDITOR
        for (auto& entry : secondaryViews)
            entry.second->shadows.tiledLights.reset();
#endif
        for (auto& entry : cameraShadows)
            entry.second->tiledLights.reset();
        lighting.emptyTiles.destroyTexture();
        lighting.tileStatsValid = false;
    }
    if ((all || !lighting.contactShadows) && lighting.contactShader){
        if (lighting.contactLoaded)
            lighting.contactRender.destroy();
        lighting.contactLoaded = false;
        lighting.contactShader.reset();
        ShaderPool::remove(ShaderType::LIGHTING_PASS, 0);
        lighting.noiseTexture.destroyTexture();
    }
    if ((all || !lighting.oit) && lighting.resolveShader){
        if (lighting.resolveLoaded)
            lighting.resolveRender.destroy();
        lighting.resolveLoaded = false;
        lighting.resolveShader.reset();
        ShaderPool::remove(ShaderType::LIGHTING_PASS, 1);
    }
    if (all){
        lighting.capsules.clear();
        lighting.capsuleLight = -1;
        lighting.contactLight = -1;
    }
}

//-----Capsule shadows, Capsules Only and the occlusion lights-----

void RenderSystem::updateLightingOcclusion(const Transform& mainCameraTransform){
    auto casters = scene->getComponentArray<CapsuleShadowCasterComponent>();
    auto transforms = scene->getComponentArray<Transform>();

    // Capsules Only: the meshes of such a caster's GameObject and its children leave the shadow
    // maps (the shadow cache sees them go once, then never again while they move)
    if (capsuleShadowsOnlyMarked || casters->size() > 0){
        auto meshes = scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++)
            meshes->getComponentFromIndex(i).capsuleShadowsOnly = false;
        capsuleShadowsOnlyMarked = false;
        for (size_t c = 0; c < casters->size(); c++){
            const CapsuleShadowCasterComponent& caster = casters->getComponentFromIndex(c);
            const Entity entity = casters->getEntity(c);
            if (!caster.enabled || caster.castShadows != CapsuleCastShadows::CAPSULES_ONLY || !scene->isEntityActiveInHierarchy(entity))
                continue;
            if (!scene->findComponent<Transform>(entity))
                continue;
            const size_t first = transforms->getIndex(entity);
            const size_t last = scene->findBranchLastIndex(entity);
            for (size_t k = first; k <= last && k < transforms->size(); k++){
                if (MeshComponent* mesh = scene->findComponent<MeshComponent>(transforms->getEntity(k))){
                    mesh->capsuleShadowsOnly = true;
                    capsuleShadowsOnlyMarked = true;
                }
            }
        }
    }

    // the lights taking capsule and contact shadows: the brightest shadowed directional light,
    // and for contact shadows it when it asks for them, else the first shadowed spot asking
    lighting.capsuleLight = -1;
    lighting.contactLight = -1;
    if (lighting.capsuleShadows || lighting.contactShadows){
        int sun = -1;
        float brightest = 0.0f;
        int contactSpot = -1;
        for (int i = 0; i < (int)sceneLights.size() && i < (int)sceneLightEntities.size(); i++){
            const SceneLight& light = sceneLights[i];
            const LightComponent* component = scene->findComponent<LightComponent>(sceneLightEntities[i]);
            if (!light.active || !component || !component->shadows)
                continue;
            if (light.position_type.w == 0.0f){
                const float brightness = lightingLuminance(light.color_intensity);
                if (sun < 0 || brightness > brightest){
                    sun = i;
                    brightest = brightness;
                }
            }else if (light.position_type.w == 2.0f && component->contactShadows && contactSpot < 0){
                contactSpot = i;
            }
        }
        if (lighting.capsuleShadows)
            lighting.capsuleLight = sun;
        if (lighting.contactShadows){
            const LightComponent* sunLight = (sun >= 0) ? scene->findComponent<LightComponent>(sceneLightEntities[sun]) : nullptr;
            lighting.contactLight = (sunLight && sunLight->contactShadows) ? sun : contactSpot;
        }
    }

    // the capsules of the casters nearest the main camera
    lighting.capsules.clear();
    lighting.capsuleCasters = 0;
    if (!lighting.capsuleShadows || lighting.capsuleLight < 0)
        return;
    const Vector3 direction(sceneLights[lighting.capsuleLight].direction_range.x, sceneLights[lighting.capsuleLight].direction_range.y,
                            sceneLights[lighting.capsuleLight].direction_range.z);
    lighting.capsuleTowardsLight = (direction.length() > 1e-6f) ? -direction.normalized() : Vector3(0.0f, 1.0f, 0.0f);

    struct Caster{
        size_t index;
        float distance;
    };
    Caster nearest[CapsuleShadows::MAX_CASTERS];
    int nearestCount = 0;
    for (size_t c = 0; c < casters->size(); c++){
        CapsuleShadowCasterComponent& caster = casters->getComponentFromIndex(c);
        const Entity entity = casters->getEntity(c);
        if (!caster.enabled || caster.capsules.empty() || !scene->isEntityActiveInHierarchy(entity))
            continue;
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            continue;
        const float distance = (transform->worldPosition - mainCameraTransform.worldPosition).length();
        if (nearestCount == CapsuleShadows::MAX_CASTERS){
            if (distance >= nearest[nearestCount - 1].distance)
                continue;
            nearestCount--;
        }
        int slot = nearestCount++;
        while (slot > 0 && nearest[slot - 1].distance > distance){
            nearest[slot] = nearest[slot - 1];
            slot--;
        }
        nearest[slot] = {c, distance};
    }

    for (int n = 0; n < nearestCount; n++){
        CapsuleShadowCasterComponent& caster = casters->getComponentFromIndex(nearest[n].index);
        const Entity entity = casters->getEntity(nearest[n].index);
        const size_t count = std::min(caster.capsules.size(), (size_t)CapsuleShadowCasterComponent::MAX_CAPSULES);

        // the bones by name under the GameObject, found again when the names change
        uint64_t namesHash = 1469598103934665603ull ^ (uint64_t)count ^ ((uint64_t)entity << 32);
        for (size_t k = 0; k < count; k++){
            for (const char ch : caster.capsules[k].bone)
                namesHash = (namesHash ^ (uint8_t)ch) * 1099511628211ull;
            namesHash = (namesHash ^ 0xFF) * 1099511628211ull;
        }
        bool resolved = caster.resolvedHash == namesHash && caster.boneEntities.size() == count;
        for (size_t k = 0; resolved && k < count; k++){
            resolved = caster.boneEntities[k] == NULL_ENTITY || scene->isEntityCreated(caster.boneEntities[k]);
        }
        if (!resolved){
            caster.boneEntities.assign(count, NULL_ENTITY);
            const size_t first = transforms->getIndex(entity);
            const size_t last = scene->findBranchLastIndex(entity);
            for (size_t k = 0; k < count; k++){
                const std::string& bone = caster.capsules[k].bone;
                if (bone.empty()){
                    caster.boneEntities[k] = entity;
                    continue;
                }
                for (size_t t = first; t <= last && t < transforms->size(); t++){
                    const Entity candidate = transforms->getEntity(t);
                    if (scene->getEntityName(candidate) == bone){
                        caster.boneEntities[k] = candidate;
                        break;
                    }
                }
            }
            caster.resolvedHash = namesHash;
        }

        bool any = false;
        for (size_t k = 0; k < count && (int)lighting.capsules.size() < CapsuleShadows::MAX_CAPSULES; k++){
            const Transform* bone = (caster.boneEntities[k] != NULL_ENTITY) ? scene->findComponent<Transform>(caster.boneEntities[k]) : nullptr;
            if (!bone)
                continue;
            const CapsuleShadows::World world = CapsuleShadows::toWorld(caster.capsules[k], bone->modelMatrix);
            if (world.radius <= 0.0f)
                continue;
            lighting.capsules.push_back(world);
            any = true;
        }
        if (any)
            lighting.capsuleCasters++;
    }
}

int RenderSystem::lightOcclusionBits(int sceneLight) const{
    int bits = 0;
    if (sceneLight == lighting.capsuleLight && sceneLight >= 0)
        bits |= 1;
    if (sceneLight == lighting.contactLight && sceneLight >= 0)
        bits |= 2;
    return bits;
}

void RenderSystem::fillLightOcclusion(const Rect& colorViewport, unsigned int targetHeight, bool flipped){
    const float width = std::max(colorViewport.getWidth(), 1.0f);
    const float height = std::max(colorViewport.getHeight(), 1.0f);
    lightOcclusionBlock.contactViewport = Vector4(colorViewport.getX(), fragCoordRowStart(colorViewport, targetHeight), 1.0f / width, 1.0f / height);
    // the mask is in the depth pre-pass's orientation: rows the other way under a flipped pass
    lightOcclusionBlock.contactParams = Vector4(flipped ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    lightOcclusionBlock.capsuleParams = Vector4(0.0f, scene->getCapsuleShadowIntensity(), scene->getCapsuleShadowSoftness(), 0.0f);
}

//-----Per draw-----

void RenderSystem::loadLightingFeatureSlots(Submesh& submesh, ShaderData& shaderData){
    submesh.slotFSLightOcclusion = shaderData.getUniformBlockIndexByName("u_fs_lightOcclusion");
    shaderData.getUniformBlockMembers("u_fs_lightOcclusion", submesh.sizeFSLightOcclusion);
    submesh.sizeFSLightOcclusion = std::min(submesh.sizeFSLightOcclusion, (unsigned int)sizeof(light_occlusion_t));
    submesh.slotTiledLights = shaderData.getTextureIndexByName("u_tiledLights");
    submesh.slotContactShadows = shaderData.getTextureIndexByName("u_contactShadowTexture");
    // the lights' records form (includes/lights.glsl): a float texture until the frame's is bound
    submesh.slotLightRecords = shaderData.getTextureIndexByName("u_lightRecords");
    submesh.lightDataTail = resolveLightDataTail(shaderData);
    if (submesh.slotLightRecords.first != -1 && ensureLightRecords(1))
        submesh.render.addTexture(submesh.slotLightRecords, ShaderStageType::FRAGMENT, &lightRecords);
    // placeholders until a draw binds the pass's own
    if (submesh.slotTiledLights.first != -1)
        submesh.render.addTexture(submesh.slotTiledLights, ShaderStageType::FRAGMENT, &emptyBlack);
    if (submesh.slotContactShadows.first != -1)
        submesh.render.addTexture(submesh.slotContactShadows, ShaderStageType::FRAGMENT, &emptyWhite);
}

LightDataTail RenderSystem::resolveLightDataTail(ShaderData& shaderData){
    LightDataTail tail;
    unsigned int size = 0;
    const std::vector<ShaderUniform>* members = shaderData.getUniformBlockMembers("u_fs_objectLights", size);
    if (!members)
        return tail;
    for (const ShaderUniform& member : *members){
        if (member.name == "lightDataBases")
            tail.bases = (int16_t)member.offset;
        else if (member.name == "contactViewport")
            tail.occlusion = (int16_t)member.offset;
        else if (member.name == "lights2DAmbient")
            tail.lights2D = (int16_t)member.offset;
    }
    return tail;
}

void RenderSystem::writeLightDataTail(const LightDataTail& tail, const MeshComponent& mesh, uint64_t properties, unsigned char* block, size_t size) const{
    auto put = [&](int offset, const Vector4* values, size_t count){
        if (offset >= 0 && (size_t)offset + count * sizeof(Vector4) <= size)
            std::memcpy(block + offset, values, count * sizeof(Vector4));
    };
    if (tail.bases >= 0){
        const Vector4 bases((float)lightDataCapsuleBase, (float)lightData2DBase, 0.0f, 0.0f);
        put(tail.bases, &bases, 1);
    }
    if (tail.occlusion >= 0){
        // the capsules that can shadow this object, from its bounds along the light, a bit each
        // in the frame's order (the block form lists the same ones in that order)
        uint32_t reach = 0;
        if (properties & lightingBit(ShaderPool::LightingFeature::CAPSULE_SHADOWS)){
            const float softness = lightOcclusionBlock.capsuleParams.z;
            for (size_t c = 0; c < lighting.capsules.size() && c < 32; c++){
                if (CapsuleShadows::reaches(lighting.capsules[c], lighting.capsuleTowardsLight, softness, mesh.worldAABB))
                    reach |= 1u << c;
            }
        }
        const Vector4 occlusion[3] = {
            lightOcclusionBlock.contactViewport,
            lightOcclusionBlock.contactParams,
            Vector4((float)(reach & 0xFFFFu), lightOcclusionBlock.capsuleParams.y, lightOcclusionBlock.capsuleParams.z, (float)(reach >> 16))
        };
        put(tail.occlusion, occlusion, 3);
    }
    if (tail.lights2D >= 0){
        const Vector4 lights2D[2] = {fs_lighting2d.ambient, fs_lighting2d.atlasInfo};
        put(tail.lights2D, lights2D, 2);
    }
}

void RenderSystem::applyObjectLights(ObjectRender& render, int slotVS, unsigned int sizeVS, int slotFS, unsigned int sizeFS,
                                     std::pair<int, int> slotLightRecords, const LightDataTail& tail, const MeshComponent& mesh,
                                     uint64_t properties, uint32_t& builtLightBits){
    const bool tiled = (properties & lightingBit(ShaderPool::LightingFeature::TILED_LIGHTS)) != 0;
    // the records form (includes/lights.glsl, LIGHTS_FS_RECORDS; the Tiled form takes precedence)
    const bool records = !tiled && slotLightRecords.first != -1;
    // bit 31 of the masked bits (the post colour's, never in them) tells the Tiled form, bit 1
    // (UV1's, never in them) a 'Dlo' variant's list (orderObjectLights: directional only) and
    // bit 0 (unlit's, never in a lit draw's) the records form
    const uint32_t lightBits = static_cast<uint32_t>(properties & (shaderPropertyBit(3) | shaderPropertyBit(28) | shaderPropertyBit(29) | shaderPropertyBit(30))) |
                               (tiled ? 0x80000000u : 0u) | ((properties & ShaderPool::getDirectionalOnlyProperty()) ? 0x2u : 0u) |
                               (records ? 0x1u : 0u);
    if (lightBits != builtLightBits){
        if (records){
            // the vertex stage's block (mixed and vertex lighting) keeps the block form
            if (slotVS != -1)
                buildObjectLights(mesh, properties, objectLightsBlock);
            buildObjectLightRecords(mesh, properties, lightRecordsBlock);
        }else if (tiled){
            // the vertex stage's block holds the object's vertex lights alone (its pixel lights
            // are the tiles'); the fragment stage's the SH ambient and the pass's grid
            buildObjectLights(mesh, properties & ~shaderPropertyBit(3), objectLightsBlock);
            tiledObjectLightsBlock.counts = Vector4(0.0f, objectLightsBlock.data[0].y, 0.0f, 0.0f);
            const uint32_t layerBit = mesh.lights.layerBit;
            int layer = 0;
            while (layer < 31 && layerBit && !(layerBit & (1u << layer)))
                layer++;
            tiledObjectLightsBlock.counts.z = (float)layer;
            const TiledLightsView* tiles = ps().tiledLights.get();
            tiledObjectLightsBlock.counts.w = (tiles && tiles->masks) ? 1.0f : 0.0f;
            // the lights over the cap are the tiles' now: the scene ambient alone
            for (int c = 0; c < 3; c++)
                tiledObjectLightsBlock.sh[c] = Vector4(0.0f, 0.0f, 0.0f, lightListAmbient[c]);
            if (tiles && tiles->uploaded){
                tiledObjectLightsBlock.tileGrid = tiles->tileGrid;
                tiledObjectLightsBlock.tileCount = tiles->tileCount;
            }else{
                // the empty lists' one tile
                tiledObjectLightsBlock.tileGrid = Vector4(0.0f, 0.0f, 65536.0f, 65536.0f);
                tiledObjectLightsBlock.tileCount = Vector4(1.0f, 1.0f, 0.0f, 0.0f);
            }
        }else{
            buildObjectLights(mesh, properties, objectLightsBlock);
        }
        builtLightBits = lightBits;
    }
    render.applyUniformBlock(slotVS, sizeVS, &objectLightsBlock);
    if (tail.bases >= 0 || tail.occlusion >= 0 || tail.lights2D >= 0){
        // the records or Tiled form with the frame's capsules and 2D lights after it
        unsigned char* block = reinterpret_cast<unsigned char*>(lightBlockScratch);
        const size_t size = std::min((size_t)sizeFS, sizeof(lightBlockScratch));
        std::memcpy(block, records ? (const void*)&lightRecordsBlock : (const void*)&tiledObjectLightsBlock,
                    std::min(size, records ? sizeof(object_light_records_t) : sizeof(tiled_object_lights_t)));
        writeLightDataTail(tail, mesh, properties, block, size);
        render.applyUniformBlock(slotFS, (unsigned int)size, block);
    }else if (records){
        render.applyUniformBlock(slotFS, std::min(sizeFS, (unsigned int)sizeof(object_light_records_t)), &lightRecordsBlock);
    }else{
        render.applyUniformBlock(slotFS, sizeFS, tiled ? (void*)&tiledObjectLightsBlock : (void*)&objectLightsBlock);
    }
    if (slotLightRecords.first != -1 && lightRecords.isCreated())
        render.addTexture(slotLightRecords, ShaderStageType::FRAGMENT, &lightRecords);
}

void RenderSystem::applyLightingFeatures(Submesh& submesh, ObjectRender& render, const MeshComponent& mesh){
    const uint64_t properties = submesh.shaderProperties;

    if ((properties & lightingBit(ShaderPool::LightingFeature::TILED_LIGHTS)) && submesh.slotTiledLights.first != -1){
        const TiledLightsView* tiles = ps().tiledLights.get();
        TextureRender* texture = (tiles && tiles->uploaded) ? (TextureRender*)&tiles->texture : nullptr;
        if (!texture){
            // a pass that built no lists (a path drawing meshes outside the camera passes):
            // every tile empty
            if (!lighting.emptyTiles.isCreated()){
                lighting.emptyTiles.createDynamicTexture("tiled lights (empty)", TiledLights::TEXTURE_WIDTH, TiledLights::textureRows(TiledLights::Grid{1, 1, 2, 2}));
                const std::vector<float> zeros((size_t)TiledLights::TEXTURE_WIDTH * (size_t)TiledLights::textureRows(TiledLights::Grid{1, 1, 2, 2}) * 4, 0.0f);
                lighting.emptyTiles.updateTexture(zeros.data(), zeros.size() * sizeof(float));
            }
            texture = &lighting.emptyTiles;
        }
        render.addTexture(submesh.slotTiledLights, ShaderStageType::FRAGMENT, texture);
    }

    const bool capsules = (properties & lightingBit(ShaderPool::LightingFeature::CAPSULE_SHADOWS)) != 0;
    const bool contact = (properties & lightingBit(ShaderPool::LightingFeature::CONTACT_SHADOWS)) != 0;
    if ((capsules || contact) && submesh.slotFSLightOcclusion != -1){
        if (capsules){
            // the capsules that can shadow this object, from its bounds along the light
            const float softness = lightOcclusionBlock.capsuleParams.z;
            int count = 0;
            for (const CapsuleShadows::World& capsule : lighting.capsules){
                if (!CapsuleShadows::reaches(capsule, lighting.capsuleTowardsLight, softness, mesh.worldAABB))
                    continue;
                lightOcclusionBlock.capsuleA[count] = Vector4(capsule.a, capsule.radius);
                lightOcclusionBlock.capsuleB[count] = Vector4(capsule.b, 0.0f);
                count++;
            }
            lightOcclusionBlock.capsuleParams.x = (float)count;
        }
        render.applyUniformBlock(submesh.slotFSLightOcclusion, submesh.sizeFSLightOcclusion, &lightOcclusionBlock);
    }
    if (contact && submesh.slotContactShadows.first != -1){
        render.addTexture(submesh.slotContactShadows, ShaderStageType::FRAGMENT, passContactMask ? passContactMask : &emptyWhite);
    }

    if (passSubmeshQueue == SubmeshQueue::OIT && submesh.oit && submesh.oit->slotFSOit != -1){
        render.applyUniformBlock(submesh.oit->slotFSOit, sizeof(Vector4), &lighting.oitParams);
    }
}

void RenderSystem::writePbrHairBlock(const Submesh& submesh, unsigned char* block, size_t size) const{
    if (submesh.pbrHairOffset < 0)
        return;
    float hair[MaterialPbr::HAIR_UNIFORM_FLOATS];
    if ((size_t)submesh.pbrHairOffset + sizeof(hair) > size)
        return;
    MaterialPbr::getHairUniformBlock(submesh.material.pbr, hair);
    std::memcpy(block + submesh.pbrHairOffset, hair, sizeof(hair));
}

//-----Screen-tile light lists-----

void RenderSystem::beginTiledLights(CameraShadows& target, const CameraComponent& camera, const Transform& cameraTransform,
                                    const Rect& viewport, unsigned int targetHeight, bool flipped){
    if (!target.tiledLights)
        target.tiledLights = std::make_unique<TiledLightsView>();
    TiledLightsView& tiles = *target.tiledLights;
    if (tiles.building){
        tiles.job.wait();
        tiles.building = false;
    }

    const int width = std::max(1, (int)viewport.getWidth());
    const int height = std::max(1, (int)viewport.getHeight());
    TiledLights::View& view = tiles.view;
    view.viewMatrix = camera.viewMatrix;
    view.projectionMatrix = camera.projectionMatrix;
    view.perspective = camera.type == CameraType::CAMERA_PERSPECTIVE;
    // the near and far the projection was built with (a perspective near of 0 is moved off it)
    const Vector2 nearFar = getCameraNearFar(camera);
    view.nearClip = nearFar.x;
    view.farClip = nearFar.y;
    view.cameraPosition = cameraTransform.worldPosition;
    view.width = width;
    view.height = height;
    // gl_FragCoord.y grows towards the image's top on GL drawing unflipped, and on the other
    // backends drawing flipped (a cube face)
    view.fragYUp = Engine::isOpenGL() != flipped;
    view.grid = TiledLights::makeGrid(width, height, lighting.tileColumns, lighting.tileRows);
    tiles.tileGrid = Vector4(viewport.getX(), fragCoordRowStart(viewport, targetHeight), (float)view.grid.tileWidth, (float)view.grid.tileHeight);
    tiles.tileCount = Vector4((float)view.grid.columns, (float)view.grid.rows, 0.0f, 0.0f);

    // the lights as the tiles list them: the per-object block's form, this pass's cascades
    tiles.lights.clear();
    tiles.masks = false;
    for (int i = 0; i < (int)sceneLights.size(); i++){
        const SceneLight& light = sceneLights[i];
        if (!light.active)
            continue;
        TiledLights::Light entry;
        entry.record[0] = light.packed[0];
        entry.record[1] = light.packed[1];
        entry.record[2] = light.packed[2];
        entry.record[3] = light.packed[3];
        const int tile = (light.spotMaskTile >= 0) ? light.spotMaskTile : 7;
        const int shadowSlot = (int)light.cone_shadow.z;
        const int cascades = passLightCascades(light);
        entry.record[3].w = (float)((std::max(shadowSlot + 1, 0) & 63) | ((cascades & 7) << 6) | ((tile & 7) << 9) | (lightOcclusionBits(i) << 12));
        const bool vertexShaded = light.shading == LightShading::VERTEX;
        entry.record[4] = Vector4((float)(light.cullingMask & 0xFFFFu), (float)(light.cullingMask >> 16), vertexShaded ? 1.0f : 0.0f, 0.0f);
        tiles.masks = tiles.masks || light.cullingMask != LAYER_MASK_EVERYTHING || vertexShaded;
        entry.position = Vector3(light.position_type.x, light.position_type.y, light.position_type.z);
        entry.range = light.direction_range.w;
        entry.global = light.position_type.w == 0.0f || light.direction_range.w <= 0.0f;
        entry.sceneIndex = i;
        tiles.lights.push_back(entry);
    }

    TiledLightsView* view_ = &tiles;
    tiles.job = jobs::submit([view_](){
        view_->builder.build(view_->view, view_->lights, view_->texels, view_->stats);
    }, {jobs::Lane::High, jobs::GateClass::Default, "tiled-lights"});
    tiles.building = true;
}

void RenderSystem::finishTiledLights(CameraShadows& target){
    if (!target.tiledLights)
        return;
    TiledLightsView& tiles = *target.tiledLights;
    if (tiles.building){
        tiles.job.wait();
        tiles.building = false;
    }
    if (tiles.texels.empty())
        return;

    const int rows = TiledLights::textureRows(tiles.view.grid);
    if (!tiles.texture.isCreated() || rows != tiles.textureRows){
        tiles.texture.destroyTexture();
        JE_GPU_ALLOC_SCOPE(Uniforms, "Tiled lights");
        if (!tiles.texture.createDynamicTexture("tiled lights", TiledLights::TEXTURE_WIDTH, rows)){
            tiles.uploaded = false;
            return;
        }
        tiles.textureRows = rows;
        tiles.uploadedFrame = 0;
    }
    // once a frame (sokol): a second pass of the camera in a frame draws with the first's lists
    const uint64_t frame = SystemRender::getFrameCount();
    if (tiles.uploadedFrame != frame || !tiles.uploaded){
        tiles.uploaded = tiles.texture.updateTexture(tiles.texels.data(), tiles.texels.size() * sizeof(float));
        tiles.uploadedFrame = frame;
    }
    if (&target == &rv().shadows){
        lighting.tileStats = tiles.stats;
        lighting.tileStatsValid = true;
    }
}

//-----Contact shadows-----

TextureRender* RenderSystem::renderContactShadows(CameraComponent& camera, TextureRender* depth, unsigned int width, unsigned int height){
    const int divisor = lighting.contactDivisor;
    if (divisor <= 0 || lighting.contactLight < 0 || lighting.contactLight >= (int)sceneLightEntities.size() || !depth)
        return nullptr;
    const Entity lightEntity = sceneLightEntities[lighting.contactLight];
    const LightComponent* light = scene->findComponent<LightComponent>(lightEntity);
    const Transform* lightTransform = scene->findComponent<Transform>(lightEntity);
    if (!light || !lightTransform)
        return nullptr;

    if (!lighting.contactLoaded){
        if (!lighting.contactShader)
            lighting.contactShader = ShaderPool::get(ShaderType::LIGHTING_PASS, 0);
        if (!lighting.contactShader || !lighting.contactShader->isCreated())
            return nullptr;
        if (!lighting.noiseTexture.isCreated()){
            void* data[1] = {(void*)blueNoise64};
            size_t size[1] = {sizeof(blueNoise64)};
            lighting.noiseTexture.createTexture("contact shadows|bluenoise", BLUE_NOISE_64_SIZE, BLUE_NOISE_64_SIZE, ColorFormat::RED, TextureType::TEXTURE_2D, 1,
                                                data, size, TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);
        }
        ObjectRender& render = lighting.contactRender;
        render.beginLoad(PrimitiveType::TRIANGLES);
        render.setShader(lighting.contactShader.get());
        ShaderData& data = lighting.contactShader->shaderData;
        render.addTexture(data.getTextureIndexByName("u_depthTexture"), ShaderStageType::FRAGMENT, &emptyWhite);
        render.addTexture(data.getTextureIndexByName("u_noiseTexture"), ShaderStageType::FRAGMENT, &lighting.noiseTexture);
        // a pass of the frame: created at once, never skipped (spec S32 §6.4)
        render.setImmediatePipelines(true);
        if (!render.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)){
            Log::error("Contact shadows: could not load their pass");
            return nullptr;
        }
        lighting.contactSlot = data.getUniformBlockIndexByName("u_fs_contactShadows");
        lighting.contactLoaded = true;
    }

    if (!rv().lightingTargets)
        rv().lightingTargets = std::make_unique<LightingViewTargets>();
    LightingViewTargets& targets = *rv().lightingTargets;
    const unsigned int maskWidth = std::max(1u, (width + (unsigned int)divisor - 1) / (unsigned int)divisor);
    const unsigned int maskHeight = std::max(1u, (height + (unsigned int)divisor - 1) / (unsigned int)divisor);
    if (!targets.contactMask.isCreated() || targets.contactWidth != maskWidth || targets.contactHeight != maskHeight){
        targets.contactMask.destroyFramebuffer();
        FramebufferDesc desc;
        desc.width = (int)maskWidth;
        desc.height = (int)maskHeight;
        desc.color = ColorFormat::RED;
        desc.depth = DepthStencilFormat::NONE;
        desc.minFilter = TextureFilter::LINEAR;
        desc.magFilter = TextureFilter::LINEAR;
        desc.label = "contact shadows";
        JE_GPU_ALLOC_SCOPE(ShadowMaps, "Contact shadows");
        if (!targets.contactMask.createFramebuffer(desc))
            return nullptr;
        targets.contactWidth = maskWidth;
        targets.contactHeight = maskHeight;
    }
    targets.contactUsed = drawCount;

    // the light in the depth pre-pass's view (its logical matrices)
    fs_contact_shadows_t block;
    block.projection = camera.projectionMatrix;
    block.invProjection = camera.projectionMatrix.inverse();
    Vector4 lightProjection;
    if (light->type == LightType::DIRECTIONAL){
        const Vector3 towards = -light->worldDirection.normalized();
        lightProjection = camera.viewProjectionMatrix * Vector4(towards, 0.0f);
        const Vector4 view = camera.viewMatrix * Vector4(towards, 0.0f);
        block.lightView = Vector4(view.x, view.y, view.z, 0.0f);
    }else{
        lightProjection = camera.viewProjectionMatrix * Vector4(lightTransform->worldPosition, 1.0f);
        const Vector4 view = camera.viewMatrix * Vector4(lightTransform->worldPosition, 1.0f);
        block.lightView = Vector4(view.x, view.y, view.z, 1.0f);
    }
    block.lightCoordinate = ContactShadows::lightCoordinate(lightProjection);
    const float depthWidth = (float)std::max(depth->getWidth(), 1);
    const float depthHeight = (float)std::max(depth->getHeight(), 1);
    block.depthSize = Vector4(depthWidth, depthHeight, 1.0f / depthWidth, 1.0f / depthHeight);
    const int samples = std::clamp(light->contactShadowSampleCount, ContactShadows::MIN_SAMPLES, ContactShadows::MAX_SAMPLES);
    int hard = 0;
    int fade = 0;
    ContactShadows::sampleGroups(samples, hard, fade);
    block.params = Vector4(std::max(light->contactShadowLength, 0.0f), std::max(light->contactShadowThickness, 0.0f), (float)samples, (float)hard);
    // Bend's starting values: a 2 % bilinear threshold and a contrast of 4
    block.params2 = Vector4((float)fade, 0.02f, 4.0f, 1.0f / (float)BLUE_NOISE_64_SIZE);

    CameraRender& pass = targets.contactPass;
    pass.setClearColor(Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    pass.setLabel("contact shadows");
    pass.startRenderPass(&targets.contactMask);
    PipelineState state;
    state.target.color = ColorFormat::RED;
    state.target.depth = DepthStencilFormat::NONE;
    state.blend = BlendMode::NONE;
    state.depthCompare = CompareFunc::ALWAYS;
    if (lighting.contactRender.beginDraw(state)){
        ShaderData& data = lighting.contactShader->shaderData;
        lighting.contactRender.addTexture(data.getTextureIndexByName("u_depthTexture"), ShaderStageType::FRAGMENT, depth);
        lighting.contactRender.addTexture(data.getTextureIndexByName("u_noiseTexture"), ShaderStageType::FRAGMENT, &lighting.noiseTexture);
        lighting.contactRender.applyUniformBlock(lighting.contactSlot, sizeof(block), &block);
        lighting.contactRender.draw(0, 3, 1);
    }
    pass.endRenderPass();
    lighting.contactWidth = (int)maskWidth;
    lighting.contactHeight = (int)maskHeight;
    return &targets.contactMask.getColorTexture();
}

//-----Weighted blended OIT-----

bool RenderSystem::usesOit(const CameraComponent& camera, int msaaSamples) const{
    // until its resolve program exists the camera draws Sorted (the accumulation alone shows nothing)
    return lighting.oit && lighting.resolveShader && lighting.resolveShader->isCreated() &&
           camera.transparencyMode == TransparencyMode::ORDER_INDEPENDENT && camera.depthTest &&
           !camera.invertCulling && OrderIndependentTransparency::isSupported(msaaSamples);
}

bool RenderSystem::drawsOit(const Submesh& submesh) const{
    return submesh.oit && submesh.oit->loaded;
}

bool RenderSystem::loadSubmeshOit(MeshComponent& mesh, unsigned int submeshIndex, uint64_t colorProperties, uint16_t customShaderId,
                                  const RenderState& colorState, bool transparentQueue){
    Submesh& submesh = mesh.submeshes[submeshIndex];
    // the built-in Lit (its lobe variants too) and library shaders write g_finalColor, which the
    // OIT variant accumulates; forks keep Sorted. Only straight alpha blending: additive and
    // multiply stay sorted, as do a transmissive submesh (it reads the scene) and an extra pass
    bool shaderTakesOit = customShaderId == 0;
    if (!shaderTakesOit){
        const std::string reference = ShaderPool::getCustomShaderName(customShaderId);
        shaderTakesOit = ShaderReference::isLibrary(reference) ||
            ShaderReference::getBase(reference) == ShaderReference::BUILTIN_LIT;
    }
    const bool transmissive = (submesh.litDefineBits & MaterialPbr::getDefineBit(MaterialPbr::TRANSMISSION_DEFINE)) != 0;
    const bool eligible = lighting.oit && transparentQueue && shaderTakesOit && colorState.blend == BlendMode::ALPHA &&
                          colorState.colorWriteMask != 0 && !transmissive && !hasExtraPass(submesh, false) &&
                          !(colorProperties & shaderPropertyBit(17)); // 'Ter'
    if (!eligible){
        releaseSubmeshOit(submesh);
        return true;
    }

    const uint64_t properties = colorProperties | lightingBit(ShaderPool::LightingFeature::OIT);
    if (!submesh.oit || submesh.oit.use_count() > 1)
        submesh.oit = std::make_shared<SubmeshOit>();
    SubmeshOit& oit = *submesh.oit;
    if (oit.loaded)
        oit.render.destroy();
    oit.loaded = false;
    if (oit.shader && (oit.shaderProperties != properties || oit.customShaderId != customShaderId)){
        oit.shader.reset();
        ShaderPool::remove(ShaderType::MESH, oit.shaderProperties, oit.customShaderId);
    }
    oit.shaderProperties = properties;
    oit.customShaderId = customShaderId;
    oit.failed = false;
    if (!oit.shader)
        oit.shader = ShaderPool::get(ShaderType::MESH, properties, customShaderId);
    finishSubmeshOit(submesh);
    (void)mesh;
    return true;
}

void RenderSystem::finishSubmeshOit(Submesh& submesh){
    SubmeshOit& oit = *submesh.oit;
    if (oit.loaded || oit.failed || !submesh.shader)
        return;
    if (!oit.shader || !oit.shader->isCreated()){
        // still building: the submesh draws Sorted until it is built (spec S32); the get()
        // after the runtime compiler is done creates it
        oit.shader = ShaderPool::get(ShaderType::MESH, oit.shaderProperties, oit.customShaderId);
        if (oit.shader->isCreated())
            return finishSubmeshOit(submesh);
        oit.failed = ShaderPool::isShaderBuildFailed(ShaderType::MESH, oit.shaderProperties, oit.customShaderId);
        if (oit.failed)
            Log::warn("The Order Independent variant of a submesh failed to build: it draws Sorted");
        return;
    }

    // the accumulation draws apply the colour variant's slots: they must be the OIT variant's
    const ShaderData& colorData = submesh.shader->shaderData;
    const ShaderData& oitData = oit.shader->shaderData;
    bool same = colorData.stages.size() == oitData.stages.size();
    for (size_t s = 0; same && s < colorData.stages.size(); s++){
        const ShaderStage& colorStage = colorData.stages[s];
        const ShaderStage& oitStage = oitData.stages[s];
        for (const ShaderUniformBlock& block : colorStage.uniformblocks){
            bool found = false;
            for (const ShaderUniformBlock& other : oitStage.uniformblocks)
                found = found || (other.name == block.name && other.slot == block.slot);
            same = same && found;
        }
        for (const ShaderTexture& texture : colorStage.textures){
            bool found = false;
            for (const ShaderTexture& other : oitStage.textures)
                found = found || (other.name == texture.name && other.slot == texture.slot);
            same = same && found;
        }
    }
    if (!same || !oit.render.loadAlike(submesh.render, oit.shader.get())){
        Log::error("The Order Independent variant of a submesh does not bind as its colour variant does: it draws Sorted");
        oit.failed = true;
        return;
    }
    oit.slotFSOit = oit.shader->shaderData.getUniformBlockIndexByName("u_fs_oit");
    oit.loaded = true;
}

void RenderSystem::releaseSubmeshOit(Submesh& submesh){
    if (!submesh.oit)
        return;
    SubmeshOit& oit = *submesh.oit;
    if (submesh.oit.use_count() == 1){
        if (oit.loaded)
            oit.render.destroy();
        if (oit.shader){
            oit.shader.reset();
            ShaderPool::remove(ShaderType::MESH, oit.shaderProperties, oit.customShaderId);
        }
    }
    submesh.oit.reset();
}

bool RenderSystem::hasOitDraws(const std::vector<TransparentRenderData>& renders){
    bool any = false;
    for (const TransparentRenderData& renderData : renders){
        if (renderData.type != TransparentRenderType::MESH)
            continue;
        for (unsigned int i = 0; i < renderData.mesh->numSubmeshes; i++){
            Submesh& submesh = renderData.mesh->submeshes[i];
            // an OIT variant built since the load
            if (submesh.oit && !submesh.oit->loaded)
                finishSubmeshOit(submesh);
            any = any || drawsOit(submesh);
        }
    }
    return any;
}

bool RenderSystem::renderOitAccumulation(CameraComponent& camera, Transform& cameraTransform, FramebufferRender* colorTarget, int msaaSamples,
                                         const Rect& viewport, bool viewportApplied, const std::vector<TransparentRenderData>& renders,
                                         int terrainView, int instanceView, const LodView* lodView){
    if (!rv().lightingTargets)
        rv().lightingTargets = std::make_unique<LightingViewTargets>();
    LightingViewTargets& targets = *rv().lightingTargets;

    // the targets match the colour target and share its depth (the multisampled one's under MSAA)
    FramebufferRender* depthSource = (msaaSamples > 1) ? &rv().msaaFramebuffer.getRender() : colorTarget;
    const unsigned int width = (unsigned int)colorTarget->getColorTexture().getWidth();
    const unsigned int height = (unsigned int)colorTarget->getColorTexture().getHeight();
    const uint32_t depthView = depthSource->backend.getDepthAttachmentView().id;
    if (!targets.oit.isCreated() || targets.oitWidth != width || targets.oitHeight != height || targets.oitSamples != msaaSamples ||
            targets.oitDepthView != depthView){
        targets.oit.destroyFramebuffer();
        FramebufferDesc desc;
        desc.width = (int)width;
        desc.height = (int)height;
        desc.color = ColorFormat::RGBA16F;
        desc.colorCount = 2;
        desc.moreColors[0] = ColorFormat::R16F;
        desc.depth = DepthStencilFormat::DEPTH_STENCIL;
        desc.sampleCount = msaaSamples;
        desc.shareDepthStencil = depthSource;
        desc.minFilter = TextureFilter::NEAREST;
        desc.magFilter = TextureFilter::NEAREST;
        desc.label = "oit accumulation";
        JE_GPU_ALLOC_SCOPE(RenderTargets, "OIT accumulation");
        if (!targets.oit.createFramebuffer(desc))
            return false;
        targets.oitWidth = width;
        targets.oitHeight = height;
        targets.oitSamples = msaaSamples;
        targets.oitDepthView = depthView;
    }
    targets.oitUsed = drawCount;

    CameraRender& pass = targets.oitPass;
    pass.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    pass.setColorActions(1, LoadAction::CLEAR, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    // the opaque depth tests the layers; the colour pass resumes on it
    pass.setDepthStencilActions(LoadAction::LOAD, StoreAction::STORE);
    pass.setLabel("oit accumulation");
    pass.startRenderPass(&targets.oit);
    if (viewportApplied)
        pass.applyViewport(viewport);

    lighting.oitParams = Vector4(std::max(camera.oitWeightScale, 1e-4f), 0.0f, 0.0f, 0.0f);
    const PipelineType accumulationPip = (msaaSamples > 1) ? PIP_MSAA : PIP_RTT;
    for (const TransparentRenderData& renderData : renders){
        if (renderData.type != TransparentRenderType::MESH)
            continue;
        bool any = false;
        for (unsigned int i = 0; i < renderData.mesh->numSubmeshes && !any; i++)
            any = drawsOit(renderData.mesh->submeshes[i]);
        if (!any)
            continue;
        passSubmeshQueue = SubmeshQueue::OIT;
        drawMesh(renderData.entity, *renderData.mesh, *renderData.transform, camera, cameraTransform, accumulationPip, renderData.instmesh,
                 renderData.terrain, renderData.tilemap, terrainView, instanceView, lodView);
        lighting.oitDrawsCounting++;
    }
    passSubmeshQueue = SubmeshQueue::ALL;
    pass.endRenderPass();
    return true;
}

void RenderSystem::drawOitResolve(PipelineType colorPip, int msaaSamples){
    if (!rv().lightingTargets || !rv().lightingTargets->oit.isCreated())
        return;
    if (!lighting.resolveLoaded){
        if (!lighting.resolveShader)
            lighting.resolveShader = ShaderPool::get(ShaderType::LIGHTING_PASS, 1);
        if (!lighting.resolveShader || !lighting.resolveShader->isCreated())
            return;
        ObjectRender& render = lighting.resolveRender;
        render.beginLoad(PrimitiveType::TRIANGLES);
        render.setShader(lighting.resolveShader.get());
        ShaderData& data = lighting.resolveShader->shaderData;
        render.addTexture(data.getTextureIndexByName("u_oitAccumTexture"), ShaderStageType::FRAGMENT, &emptyBlack);
        render.addTexture(data.getTextureIndexByName("u_oitWeightTexture"), ShaderStageType::FRAGMENT, &emptyBlack);
        render.setImmediatePipelines(true);
        if (!render.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)){
            Log::error("Order Independent transparency: could not load its resolve pass");
            return;
        }
        lighting.resolveLoaded = true;
    }
    PipelineState state;
    state.target = targetFormatFor(colorPip, msaaSamples);
    state.blend = BlendMode::ALPHA;
    state.depthCompare = CompareFunc::ALWAYS;
    state.depthWrite = false;
    if (!lighting.resolveRender.beginDraw(state))
        return;
    FramebufferRender& targets = rv().lightingTargets->oit;
    ShaderData& data = lighting.resolveShader->shaderData;
    lighting.resolveRender.addTexture(data.getTextureIndexByName("u_oitAccumTexture"), ShaderStageType::FRAGMENT, &targets.getColorAttachmentTexture(0));
    lighting.resolveRender.addTexture(data.getTextureIndexByName("u_oitWeightTexture"), ShaderStageType::FRAGMENT, &targets.getColorAttachmentTexture(1));
    lighting.resolveRender.draw(0, 3, 1);
}
