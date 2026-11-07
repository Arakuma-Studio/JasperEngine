// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RenderSystem's 3D shadows (spec S10, docs/rendering/shadows.md): the quality tier, the slots
// of the shadow-casting lights and their atlases, the shadow cameras (stable cascades), the
// shader data, the shadow caster registry, the cached shadow passes and their statistics. The
// fit, cache, registry and planner logic are the pure CPU units of render/shadow/.

#include "RenderSystem.h"
#include "RenderSystemShared.h"
#include "render/SystemRender.h"
#include "device/QualityResolver.h"
#include "Scene.h"
#include "Engine.h"
#include "Log.h"
#include "profiler/Profile.h"
#include "pool/ShaderPool.h"
#include "render/shadow/CascadeFit.h"
#include "util/Angle.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::rendershared;

namespace {

    // FNV-1a over the bytes of a value
    struct ShadowHash {
        uint64_t value = 1469598103934665603ULL;

        template <typename T>
        void add(const T& data){
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&data);
            for (size_t b = 0; b < sizeof(T); b++){
                value = (value ^ bytes[b]) * 1099511628211ULL;
            }
        }
    };

    ShadowBounds toShadowBounds(const AABB& box){
        ShadowBounds bounds;
        if (box.isNull() || !box.isFinite() || box == AABB::ZERO)
            return bounds;
        bounds.min = box.getMinimum();
        bounds.max = box.getMaximum();
        bounds.valid = std::isfinite(bounds.min.x) && std::isfinite(bounds.max.x);
        return bounds;
    }

    Rect toRect(const ShadowRect& rect){
        return Rect((float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height);
    }

    // a slot's rect as offset.xy, scale.zw of the atlas UV (bottom-left origin)
    Vector4 atlasUVRect(const ShadowRect& rect, unsigned int width, unsigned int height){
        if (rect.empty() || width == 0 || height == 0)
            return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        return Vector4((float)rect.x / (float)width, (float)rect.y / (float)height,
                       (float)rect.width / (float)width, (float)rect.height / (float)height);
    }

    // a direction rotated about an axis (Rodrigues), angle in radians
    Vector3 rotateAbout(const Vector3& v, const Vector3& axis, float angle){
        const Vector3 k = axis.normalized();
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        return v * c + k.crossProduct(v) * s + k * (k.dotProduct(v) * (1.0f - c));
    }

    // the cache path of a slot as the debug views read it (fade.z, state.x)
    float pathCode(ShadowSlotPath path){
        switch (path){
            case ShadowSlotPath::SKIP: return 0.0f;
            case ShadowSlotPath::RESTORE_DYNAMIC: return 1.0f;
            case ShadowSlotPath::RENDER_STATIC: return 2.0f;
            case ShadowSlotPath::FULL: return 3.0f;
            case ShadowSlotPath::UNCACHED: return 4.0f;
        }
        return 0.0f;
    }

    // guard band of a cube face: the face's field of view holds `reach` extra texels on every
    // side, tan(fov / 2) = res / (res - 2 reach), so no tap of the widest kernel leaves the face
    float pointGuardScale(unsigned int resolution, float reach){
        const float res = (float)std::max(resolution, 16u);
        const float texels = std::ceil(reach);
        return res / std::max(res - 2.0f * texels, res * 0.5f);
    }

    constexpr unsigned int MIN_SHADOW_RESOLUTION = 256;
}

// ---------------------------------------------------------------- tier

void RenderSystem::updateShadowTier(){
    const ShadowQuality quality = scene->getShadowQuality();
    shadowHardOnly = quality == ShadowQuality::NONE;
    switch (quality){
        case ShadowQuality::LOW: shadowTier = ShadowTier::LOW; break;
        case ShadowQuality::MEDIUM: shadowTier = ShadowTier::MEDIUM; break;
        case ShadowQuality::HIGH: shadowTier = ShadowTier::HIGH; break;
        case ShadowQuality::ULTRA: shadowTier = ShadowTier::ULTRA; break;
        case ShadowQuality::NONE:
        case ShadowQuality::AUTO:
            shadowTier = ShadowTiers::getPlatformTier(static_cast<int>(Engine::getPlatform()));
            break;
    }
    // the device tier's cap (and a Critical thermal step's, the tier below's)
    shadowTier = QualityResolver::shadowTier(shadowTier);

    // a build with no shader compiler has the High filters only where its export cooked them
    // (spec S10 R25): above that it renders the Medium tier
#if !defined(JASPERENGINE_EDITOR) && !defined(JASPERENGINE_RUNTIME_SHADERS)
    if (ShadowTiers::usesHighFilters(shadowTier) && !ShadowTiers::cooksHighFilters(static_cast<int>(ShaderPool::getShaderBackend()))){
        if (!shadowTierCapWarned){
            Log::warn("Shadow quality %s: this build's shaders have no High filters for %s (Project Settings > Graphics > Shadows "
                      "sets the platforms' tiers), shadows render at Medium", ShadowTiers::getName(shadowTier),
                      ShaderPool::getShaderBackendName(ShaderPool::getShaderBackend()).c_str());
            shadowTierCapWarned = true;
        }
        shadowTier = ShadowTier::MEDIUM;
    }
#endif

    // one global variant change when the tier crosses into or out of the High filters; every
    // mesh keeps drawing its variant until its new one is built
    const bool highFilters = ShadowTiers::usesHighFilters(shadowTier) && !shadowHardOnly;
    if (highFilters != shadowHQVariants){
        shadowHQVariants = highFilters;
        needReloadMeshVariants();
    }
}

const ShadowTierSettings& RenderSystem::shadowTierSettings() const{
    return ShadowTiers::getSettings(shadowTier);
}

float RenderSystem::shadowPCFRadius() const{
    // the legacy entry points' PCF tap radius (includes/shadows.glsl: kernel (2r+1)^2)
    if (shadowHardOnly)
        return 0.0f;
    switch (shadowTier){
        case ShadowTier::LOW: return 1.0f;
        case ShadowTier::MEDIUM: return 2.0f;
        case ShadowTier::HIGH:
        case ShadowTier::ULTRA: return 3.0f;
    }
    return 1.0f;
}

unsigned int RenderSystem::lightShadowCascades(const LightComponent& light, const CameraComponent& camera) const{
    if (light.type == LightType::POINT)
        return SHADOW_CUBE_FACES;
    if (light.type == LightType::SPOT)
        return 1;
    if (camera.type != CameraType::CAMERA_PERSPECTIVE)
        return 1;
    const unsigned int tierMax = std::min(shadowTierSettings().maxCascades, (unsigned int)MAX_SHADOWCASCADES);
    const unsigned int wanted = (light.numShadowCascades == 0) ? tierMax : light.numShadowCascades;
    return std::clamp(wanted, 1u, tierMax);
}

float RenderSystem::lightShadowDistance(const LightComponent& light, const CameraComponent& camera) const{
    float distance = light.shadowDistance;
    if (!(distance > 0.0f))
        distance = scene->getShadowDistance();
    if (!(distance > 0.0f))
        distance = shadowTierSettings().shadowDistance;
    distance = QualityResolver::shadowDistance(distance);
    if (camera.farClip > camera.nearClip)
        distance = std::min(distance, camera.farClip);
    return distance;
}

unsigned int RenderSystem::lightShadowResolution(const LightComponent& light) const{
    const ShadowTierSettings& tier = shadowTierSettings();
    unsigned int resolution = light.mapResolution;
    if (resolution == 0){
        resolution = (light.type == LightType::DIRECTIONAL) ? tier.slotResolution :
                     (light.type == LightType::SPOT ? tier.spotResolution : tier.pointResolution);
    }
    return ShadowSlotPlanner::powerOfTwoResolution(resolution, MIN_SHADOW_RESOLUTION, tier.slotResolution);
}

// ---------------------------------------------------------------- slots and atlases

bool RenderSystem::loadLights(int numLights){
    updateShadowTier();

    auto lights = scene->getComponentArray<LightComponent>();
    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        if (light.needUpdateShadowMap){
            needUpdateShadowBindings = true;
            light.needUpdateShadowMap = false;
            light.shadowRevision++;
        }
    }

    planShadowSlots(numLights);
    if (!ensureShadowAtlases()){
        Log::warn("Failed to create the shadow atlases");
        for (int i = 0; i < numLights; i++){
            LightComponent& light = lights->getComponentFromIndex(i);
            if ((light.type == LightType::POINT) ? !hasShadowPointAtlas : !hasShadowAtlas)
                light.shadowMapIndex = -1;
        }
    }
    return true;
}

void RenderSystem::planShadowSlots(int numLights){
    auto lights = scene->getComponentArray<LightComponent>();
    const ShadowTierSettings& tier = shadowTierSettings();

    // importance is judged from the scene camera
    Vector3 cameraPosition(0.0f, 0.0f, 0.0f);
    CameraComponent* sceneCamera = scene->findComponent<CameraComponent>(scene->getCamera());
    if (Transform* cameraTransform = scene->findComponent<Transform>(scene->getCamera()))
        cameraPosition = cameraTransform->worldPosition;

    shadowRequests.clear();
    shadowRequestLight.clear();
    // a static layer pays while static and dynamic casters share the slots; it is kept a while
    // after the last dynamic caster went (one toggled off and on does not rebuild it)
    if (shadowCasters.getDynamicBounds().valid)
        shadowDynamicSeen = shadowFrame;
    const bool staticAndDynamic = shadowCasters.getStaticBounds().valid && shadowDynamicSeen > 0 &&
                                  shadowFrame - shadowDynamicSeen <= STATIC_CACHE_KEEP_UPDATES;
    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        light.shadowMapIndex = -1;
        light.shadowCascades = 0;
        for (unsigned int& resolution : light.shadowSlotResolution)
            resolution = 0;
        const Entity entity = lights->getEntity(i);
        if (!hasShadows || !light.shadows || light.intensity <= 0.0f || !isLightActive(entity, light))
            continue;

        ShadowLightRequest request;
        request.id = (uint32_t)entity;
        request.kind = (light.type == LightType::DIRECTIONAL) ? ShadowLightKind::DIRECTIONAL :
                       (light.type == LightType::SPOT ? ShadowLightKind::SPOT : ShadowLightKind::POINT);
        request.slots = sceneCamera ? lightShadowCascades(light, *sceneCamera) :
                        ((light.type == LightType::POINT) ? SHADOW_CUBE_FACES : 1u);
        request.resolution = lightShadowResolution(light);
        if (light.type == LightType::DIRECTIONAL){
            request.importance = light.resolvedIntensity;
        }else{
            const Transform* transform = scene->findComponent<Transform>(entity);
            const Vector3 position = transform ? transform->worldPosition : Vector3(0.0f, 0.0f, 0.0f);
            const float distance = (position - cameraPosition).length();
            // the share of the screen its range covers, about, and how far that is
            float coverage = 1.0f;
            float away = distance;
            if (light.range > 0.0f){
                coverage = std::clamp((light.range * light.range) / std::max(distance * distance, 1e-4f), 0.0f, 1.0f);
                away = std::max(0.0f, distance - light.range);
            }
            request.importance = light.resolvedIntensity * coverage / (1.0f + away);
        }
        request.wantsStatic = light.shadowUpdateMode == ShadowUpdateMode::AUTO && staticAndDynamic;
        shadowRequests.push_back(request);
        shadowRequestLight.push_back(i);
    }

    ShadowSlotPlanner::Limits limits;
    limits.projectiveSlots = MAX_SHADOW_ATLAS_SLOTS;
    limits.pointSlots = MAX_POINT_SHADOW_ATLAS_SLOTS;
    const int maxTexture = Engine::isViewLoaded() ? sg_query_limits().max_image_size_2d : 0;
    limits.maxTextureSize = (maxTexture > 0) ? (unsigned int)maxTexture : 4096u;
    limits.maxResolution = tier.slotResolution;
    limits.minResolution = MIN_SHADOW_RESOLUTION;
    limits.memoryBytes = tier.memoryBytes;
    // JASPERENGINE_SHADOW_MEMORY_MB=<n>: the tier's memory row replaced, for budget investigations
    // (docs/rendering/shadow-budgets.md: what a static layer saves on a device)
    static const uint64_t memoryOverride = [](){
        const char* value = std::getenv("JASPERENGINE_SHADOW_MEMORY_MB");
        return value ? (uint64_t)std::max(0, std::atoi(value)) * 1024ull * 1024ull : 0ull;
    }();
    if (memoryOverride > 0)
        limits.memoryBytes = memoryOverride;
    shadowPlan = shadowPlanner.plan(shadowRequests, limits, shadowFrame);

    for (size_t r = 0; r < shadowRequests.size(); r++){
        LightComponent& light = lights->getComponentFromIndex(shadowRequestLight[r]);
        const ShadowLightAssignment& assignment = shadowPlan.lights[r];
        light.shadowMapIndex = assignment.firstSlot;
        if (assignment.firstSlot < 0)
            continue;
        light.shadowCascades = shadowRequests[r].slots;
        for (unsigned int s = 0; s < SHADOW_LIGHT_MAX_SLOTS; s++){
            light.shadowSlotResolution[s] = assignment.resolution[s];
        }
    }
}

bool RenderSystem::ensureShadowAtlases(){
    auto ensure = [&](FramebufferRender& atlas, TextureRender* raw, const ShadowAtlasLayout& layout, uint64_t& generation, bool& has, const char* label) -> bool{
        if (layout.width == 0 || layout.height == 0){
            if (atlas.isCreated()){
                atlas.destroyFramebuffer();
                if (raw)
                    raw->destroyTexture();
                generation++;
                needUpdateShadowBindings = true;
            }
            has = false;
            return true;
        }
        if (atlas.isCreated() && atlas.getDepthTexture().getWidth() == (int)layout.width &&
                atlas.getDepthTexture().getHeight() == (int)layout.height){
            if (!has)
                needUpdateShadowBindings = true;
            has = true;
            return true;
        }
        atlas.destroyFramebuffer();
        if (raw)
            raw->destroyTexture();
        generation++;
        needUpdateShadowBindings = true;
        // LINEAR on a comparison sampler is hardware PCF: each tap is bilinearly filtered over
        // 2x2 comparison results; the white border reads as lit
        JE_GPU_ALLOC_SCOPE(ShadowMaps, std::strstr(label, "static") ? "Static shadow cache" : (std::strstr(label, "point") ? "Point shadow atlas" : "CSM atlas"));
        if (!atlas.createDepthOnlyFramebuffer((int)layout.width, (int)layout.height,
                TextureFilter::LINEAR, TextureFilter::LINEAR,
                TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER, true)){
            has = false;
            return false;
        }
        if (raw)
            raw->createAliasView(atlas.getDepthTexture(), label);
        has = true;
        return true;
    };

    bool ok = ensure(shadowAtlasFramebuffer, &shadowAtlasRaw, shadowPlan.projective, shadowAtlasGeneration, hasShadowAtlas, "shadow-atlas-raw");
    ok = ensure(shadowPointAtlasFramebuffer, &shadowPointAtlasRaw, shadowPlan.point, shadowPointAtlasGeneration, hasShadowPointAtlas, "shadow-point-atlas-raw") && ok;

    // the static caches: created when a slot has a static layer, kept a while unused
    auto ensureStatic = [&](FramebufferRender& atlas, TextureRender& raw, const ShadowAtlasLayout& layout, uint64_t& generation, uint64_t& used, const char* label){
        bool has = atlas.isCreated();
        if (layout.width > 0){
            used = shadowFrame;
            if (!ensure(atlas, &raw, layout, generation, has, label))
                Log::warn("Failed to create the shadow static cache (%ux%u)", layout.width, layout.height);
        }else if (atlas.isCreated() && shadowFrame - used > STATIC_CACHE_KEEP_UPDATES){
            atlas.destroyFramebuffer();
            raw.destroyTexture();
            generation++;
        }
    };
    ensureStatic(shadowStaticAtlas, shadowStaticAtlasRaw, shadowPlan.projectiveStatic, shadowStaticGeneration, shadowStaticUsed, "shadow-static-raw");
    ensureStatic(shadowStaticPointAtlas, shadowStaticPointAtlasRaw, shadowPlan.pointStatic, shadowStaticPointGeneration, shadowStaticPointUsed, "shadow-static-point-raw");
    return ok;
}

void RenderSystem::releaseShadowAtlases(){
    shadowAtlasFramebuffer.destroyFramebuffer();
    shadowAtlasRaw.destroyTexture();
    shadowPointAtlasFramebuffer.destroyFramebuffer();
    shadowPointAtlasRaw.destroyTexture();
    shadowStaticAtlas.destroyFramebuffer();
    shadowStaticAtlasRaw.destroyTexture();
    shadowStaticPointAtlas.destroyFramebuffer();
    shadowStaticPointAtlasRaw.destroyTexture();
    shadowAtlasGeneration++;
    shadowPointAtlasGeneration++;
    shadowStaticGeneration++;
    shadowStaticPointGeneration++;
    hasShadowAtlas = false;
    hasShadowPointAtlas = false;
}

Rect RenderSystem::getShadowAtlasSlotRect(int slotIndex) const{
    if (slotIndex < 0 || slotIndex >= (int)shadowPlan.projective.rects.size())
        return Rect();
    return toRect(shadowPlan.projective.rects[slotIndex]);
}

Rect RenderSystem::getShadowPointAtlasSlotRect(int slotIndex) const{
    if (slotIndex < 0 || slotIndex >= (int)shadowPlan.point.rects.size())
        return Rect();
    return toRect(shadowPlan.point.rects[slotIndex]);
}

void RenderSystem::updateLightShadowRevision(LightComponent& light, const Transform& transform){
    // what the maps of a light depend on (spec S10 R18); receiver-only settings (biases,
    // strength, filter, fade) are left out, they never redraw a map
    ShadowHash hash;
    hash.add(light.type);
    hash.add(light.enabled);
    hash.add(light.shadows);
    hash.add(light.cullingMask);
    hash.add(light.mapResolution);
    hash.add(light.numShadowCascades);
    hash.add(light.cascadeSplitMode);
    hash.add(light.cascadeSplits);
    hash.add(light.shadowDistance);
    hash.add(light.shadowUpdateMode);
    hash.add(light.automaticShadowCamera);
    hash.add(light.shadowCameraNearFar);
    hash.add(light.range);
    hash.add(light.innerConeCos);
    hash.add(light.outerConeCos);
    hash.add(light.worldDirection);
    if (light.type != LightType::DIRECTIONAL){
        hash.add(transform.worldPosition);
        hash.add(light.spotMaskReady);
        hash.add(light.spotMaskAspect);
        hash.add(light.worldUp);
    }
    if (hash.value != light.shadowSettingsHash){
        light.shadowSettingsHash = hash.value;
        light.shadowRevision++;
    }
}

// ---------------------------------------------------------------- casters

// The layers holding meshes the shadow passes draw this update, and among them those whose
// casters a camera draws its own way: a terrain through its CDLOD cut, a billboard turned
// to it, instances culled or faded from its eye.
void RenderSystem::ensureCasterLayers(){
    if (casterLayersEpoch == shadowEpoch)
        return;
    casterLayersEpoch = shadowEpoch;
    shadowCasterLayers = 0;
    cameraCasterLayers = 0;

    // the spot and point lights the shadow passes draw: their shadow cameras are the
    // lights' own, so what reaches their maps is the same for every camera. Only lights
    // holding a shadow slot, so at most one per slot.
    auto lights = scene->getComponentArray<LightComponent>();
    const int numLights = (int)lights->size();
    lightCasterLayers.assign(numLights, 0u);
    int localLights[MAX_SHADOW_ATLAS_SLOTS + MAX_POINT_SHADOW_LIGHTS];
    int numLocalLights = 0;
    for (int l = 0; l < numLights; l++){
        const LightComponent& light = lights->getComponentFromIndex(l);
        if (light.type != LightType::DIRECTIONAL && light.shadows && light.intensity > 0 && light.shadowMapIndex >= 0 &&
                isLightActive(lights->getEntity(l), light) &&
                numLocalLights < MAX_SHADOW_ATLAS_SLOTS + MAX_POINT_SHADOW_LIGHTS){
            localLights[numLocalLights++] = l;
        }
    }

#ifdef JASPERENGINE_EDITOR
    // what a shadow pass draws: never the editor's gizmos and icons, always a mirror
    const bool drawsOverlays = passDrawsOverlays;
    const Entity excludedEntity = passExcludedEntity;
    passDrawsOverlays = false;
    passExcludedEntity = NULL_ENTITY;
#endif

    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (!mesh.loaded || mesh.needReload || !mesh.castsShadowMaps())
            continue;
        Entity entity = meshes->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !transform->visible || !isRenderActive(entity))
            continue;

        const uint32_t layerBit = 1u << scene->getEntityLayer(entity);
        shadowCasterLayers |= layerBit;

        // a terrain's cut stays inside its bounds; a turned billboard or a camera's own
        // instance list may not stay inside the frame's
        const bool terrain = scene->findComponent<TerrainComponent>(entity) != nullptr;
        bool unbounded = transform->billboard && !transform->fakeBillboard && !mesh.shadowsBillboard;
        InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        if (instmesh){
            unbounded = unbounded || instmesh->instancedBillboard ||
                instanceFadeCullEnd(*instmesh, mesh, *scene) >= 0.0f || instanceFadeCullNear(*instmesh, mesh, *scene) > 0.0f ||
                (instmesh->cullInstances && std::isfinite(instmesh->cullDistance) && instmesh->cullDistance > 0.0f);
        }
        if ((terrain && !mesh.staticShadowCaster) || unbounded){
            cameraCasterLayers |= layerBit;
        }

        // the batch test of the depth passes (isBatchVisible), against each shadow camera
        const bool everywhere = unbounded || !cullsBatch(mesh, instmesh);
        for (int k = 0; k < numLocalLights; k++){
            const int l = localLights[k];
            if (lightCasterLayers[l] & layerBit)
                continue;
            const LightComponent& light = lights->getComponentFromIndex(l);
            bool reaches = everywhere;
            const int faces = (light.type == LightType::POINT) ? SHADOW_CUBE_FACES : 1;
            for (int c = 0; c < faces && !reaches; c++){
                reaches = isInsideCamera(light.cameras[c].nearFar.y, light.cameras[c].frustumPlanes, mesh.worldAABB);
            }
            if (reaches){
                lightCasterLayers[l] |= layerBit;
            }
        }
    }

    for (int l = 0; l < numLights; l++){
        if (lights->getComponentFromIndex(l).type == LightType::DIRECTIONAL){
            lightCasterLayers[l] = shadowCasterLayers;
        }
    }

#ifdef JASPERENGINE_EDITOR
    passDrawsOverlays = drawsOverlays;
    passExcludedEntity = excludedEntity;
#endif
}

// The casters of this update and what changed in them (the shadow cache's input): sampled once
// per update, after the transforms, bounds and animation of the update are final
// This update's changes into the log, keeping what the frame's atlases have not evaluated yet
// (an atlas behind the log's first entry redraws its slots: renderShadowMaps)
void RenderSystem::appendCasterChanges(){
    const CameraShadows& frame = primaryView.shadows;
    const uint64_t seen = std::min(frame.projectiveChangesSeen, frame.pointChangesSeen);
    if (seen > casterChangeBase){
        const size_t consumed = (size_t)std::min<uint64_t>(seen - casterChangeBase, casterChangeLog.size());
        casterChangeLog.erase(casterChangeLog.begin(), casterChangeLog.begin() + (std::ptrdiff_t)consumed);
        casterChangeBase += consumed;
    }
    const std::vector<ShadowCasterChange>& changes = shadowCasters.getChanges();
    casterChangeLog.insert(casterChangeLog.end(), changes.begin(), changes.end());
    if (casterChangeLog.size() > CASTER_CHANGE_LOG_MAX){
        const size_t dropped = casterChangeLog.size() - CASTER_CHANGE_LOG_MAX;
        casterChangeLog.erase(casterChangeLog.begin(), casterChangeLog.begin() + (std::ptrdiff_t)dropped);
        casterChangeBase += dropped;
    }
}

void RenderSystem::updateShadowCasters(){
    shadowCasters.beginUpdate();
    if (!hasShadows){
        shadowCasters.endUpdate();
        appendCasterChanges();
        return;
    }

#ifdef JASPERENGINE_EDITOR
    const bool drawsOverlays = passDrawsOverlays;
    const Entity excludedEntity = passExcludedEntity;
    passDrawsOverlays = false;
    passExcludedEntity = NULL_ENTITY;
#endif

    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (!mesh.loaded || mesh.needReload || !mesh.castsShadowMaps())
            continue;
        const Entity entity = meshes->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !transform->visible || !isRenderActive(entity))
            continue;

        // its buffers or alpha-clip textures update this frame (a terrain: its heights)
        TerrainComponent* terrainComponent = scene->findComponent<TerrainComponent>(entity);
        bool contentChanged = mesh.needUpdateBuffer || (terrainComponent && terrainComponent->needUpdateTexture);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            contentChanged = contentChanged || mesh.submeshes[s].needUpdateDepthTexture;
        }
        if (contentChanged)
            mesh.shadowCasterRevision++;

        ShadowCasterState state;
        state.bounds = toShadowBounds(mesh.worldAABB);
        state.layerBit = 1u << scene->getEntityLayer(entity);
        state.isStatic = mesh.staticShadowCaster;

        // what the bounds can miss: the mesh's own revision (transforms, reloads, buffers),
        // the detail levels and billboarding it is drawn with, its morph weights
        ShadowHash revision;
        revision.add(mesh.shadowCasterRevision);
        revision.add(mesh.lodEnabled);
        revision.add(mesh.lodBias);
        revision.add(QualityResolver::lodBias() * QualityResolver::shadowLodBias());
        revision.add(mesh.shadowsBillboard);
        revision.add(transform->billboard);
        bool morphs = false;
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            morphs = morphs || mesh.submeshes[s].hasMorphTarget;
        }
        if (morphs)
            revision.add(mesh.morphWeights);
        // upstream's Vertex Animator moves the vertices on the GPU: what it plays (the payload,
        // the u_vs_vat block's clips and times) is what it casts, so the cached shadow follows it
        if (mesh.vat.variantActive != VatVariant::NONE){
            revision.add(mesh.vat.variantActive);
            revision.add(mesh.vat.texture);
            for (const Vector4& v : mesh.vat.block)
                revision.add(v);
        }
        state.revision = (uint32_t)(revision.value ^ (revision.value >> 32));

        const bool terrain = terrainComponent != nullptr;
        bool unbounded = transform->billboard && !transform->fakeBillboard && !mesh.shadowsBillboard;
        InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        if (instmesh){
            unbounded = unbounded || instmesh->instancedBillboard ||
                instanceFadeCullEnd(*instmesh, mesh, *scene) >= 0.0f || instanceFadeCullNear(*instmesh, mesh, *scene) > 0.0f ||
                (instmesh->cullInstances && std::isfinite(instmesh->cullDistance) && instmesh->cullDistance > 0.0f);
        }
        // a static terrain draws a cut of its own in each slot (shadowTerrainCut): the camera
        // does not move it
        state.cameraDependent = (terrain && !mesh.staticShadowCaster) || unbounded;
        // skinned meshes whose bones moved this update
        state.animated = mesh.needUpdateBones;

        shadowCasters.update((uint32_t)entity, state);
    }
    shadowCasters.endUpdate();
    appendCasterChanges();

#ifdef JASPERENGINE_EDITOR
    passDrawsOverlays = drawsOverlays;
    passExcludedEntity = excludedEntity;
#endif
}

// ---------------------------------------------------------------- shadow cameras

void RenderSystem::updateLightFromScene(LightComponent& light, Transform& transform, CameraComponent&, Transform&){
    light.worldDirection = transform.worldRotation * light.direction;
    light.worldUp = (transform.worldRotation * light.getLocalSpotProjectionUp()).normalized();
}

void RenderSystem::shadowFitCamera(const CameraComponent& camera, const Transform& cameraTransform, bool frame,
                                   Matrix4& view, Vector3& position, Vector3& forward) const{
    view = camera.viewMatrix;
    position = cameraTransform.worldPosition;
    // the camera's axes from its view matrix: rows of the rotation
    forward = Vector3(-view[0][2], -view[1][2], -view[2][2]).normalized();
    if (!frame)
        return;
    const ShadowDebugSettings& debug = shadowDebug;
    if (debug.fitCameraOffset == Vector3(0.0f, 0.0f, 0.0f) && debug.fitCameraEuler == Vector3(0.0f, 0.0f, 0.0f))
        return;

    Vector3 up(view[0][1], view[1][1], view[2][1]);
    const Vector3 right(view[0][0], view[1][0], view[2][0]);
    // pitch about the camera's right, then yaw about world up, then roll about the forward
    const float pitch = Angle::degToRad(debug.fitCameraEuler.x);
    const float yaw = Angle::degToRad(debug.fitCameraEuler.y);
    const float roll = Angle::degToRad(debug.fitCameraEuler.z);
    forward = rotateAbout(forward, right, pitch);
    up = rotateAbout(up, right, pitch);
    forward = rotateAbout(forward, Vector3(0.0f, 1.0f, 0.0f), yaw);
    up = rotateAbout(up, Vector3(0.0f, 1.0f, 0.0f), yaw);
    up = rotateAbout(up, forward, roll);
    position = position + debug.fitCameraOffset;
    view = Matrix4::lookAtMatrix(position, position + forward, up);
}

void RenderSystem::computeLightCameras(const LightComponent& light, const Transform& transform, const CameraComponent& camera, const Transform& cameraTransform,
                                       LightCamera* cameras, unsigned int& numCascades, CascadeFitResult& fit, bool& hasFit, bool frame,
                                       Vector3& fitPosition, Vector3& fitForward){
    // near and far of the spot and point cameras: the camera near and the light's range (or
    // the camera far), or the light's own
    Vector2 nearFar = light.shadowCameraNearFar;
    if (light.automaticShadowCamera){
        nearFar.x = camera.nearClip;
        nearFar.y = (light.range == 0.0f) ? camera.farClip : light.range;
    }
    nearFar.x = std::max(nearFar.x, 0.001f);
    nearFar.y = std::max(nearFar.y, nearFar.x * 1.01f);

    if (light.type == LightType::DIRECTIONAL){
        Matrix4 view;
        shadowFitCamera(camera, cameraTransform, frame, view, fitPosition, fitForward);

        CascadeFitInput input;
        input.cameraView = view;
        const Matrix4& projection = camera.projectionMatrix;
        input.orthographic = camera.type != CameraType::CAMERA_PERSPECTIVE;
        input.tanHalfY = (projection[1][1] != 0.0f) ? 1.0f / projection[1][1] : 1.0f;
        input.aspect = (projection[0][0] != 0.0f) ? projection[1][1] / projection[0][0] : 1.0f;
        input.orthoHalfWidth = (projection[0][0] != 0.0f) ? 1.0f / std::abs(projection[0][0]) : 1.0f;
        input.orthoHalfHeight = (projection[1][1] != 0.0f) ? 1.0f / std::abs(projection[1][1]) : 1.0f;
        input.nearClip = input.orthographic ? std::max(camera.nearClip, 0.0f) : getCameraNearFar(camera).x;
        input.shadowDistance = lightShadowDistance(light, camera);
        input.lightDir = light.worldDirection;
        input.cascades = std::max(numCascades, 1u);
        input.resolution = std::max(light.shadowSlotResolution[0], 1u);
        for (unsigned int c = 0; c < CASCADE_FIT_MAX; c++){
            input.cascadeResolution[c] = light.shadowSlotResolution[c];
        }
        input.splits[0] = light.cascadeSplits.x;
        input.splits[1] = light.cascadeSplits.y;
        input.splits[2] = light.cascadeSplits.z;
        input.mode = light.cascadeSplitMode;
        input.blendFraction = CASCADE_BLEND_FRACTION;
        input.freeze = shadowTierSettings().freezeFit && light.shadowUpdateMode == ShadowUpdateMode::AUTO;
        input.casterBounds = shadowCasters.getBounds();

        const CascadeFitResult previous = fit;
        fit = CascadeFit::fit(input, hasFit ? &previous : nullptr);
        hasFit = fit.cascades > 0;
        numCascades = fit.cascades;
        for (unsigned int c = 0; c < fit.cascades; c++){
            cameras[c].lightViewMatrix = fit.view;
            cameras[c].lightProjectionMatrix = fit.proj[c];
            cameras[c].lightViewProjectionMatrix = fit.viewProj[c];
            cameras[c].nearFar = Vector2(fit.splitNear[c], fit.splitFar[c]);
            updateCameraFrustumPlanes(cameras[c].lightViewProjectionMatrix, cameras[c].frustumPlanes);
        }
    }else if (light.type == LightType::SPOT){
        const Vector3 lightUp = (std::abs(light.worldDirection.normalized().y) > 0.99f) ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
        const Vector3 projectionUp = light.spotMaskReady ? light.worldUp : lightUp;
        const float projectionAspect = light.spotMaskReady ? light.spotMaskAspect : 1.0f;
        const Matrix4 viewMatrix = Matrix4::lookAtMatrix(transform.worldPosition, light.worldDirection + transform.worldPosition, projectionUp);
        const Matrix4 projectionMatrix = Matrix4::perspectiveMatrix(std::acos(std::clamp(light.outerConeCos, -1.0f, 1.0f)) * 2.0f,
                                                                    projectionAspect, nearFar.x, nearFar.y);
        cameras[0].lightViewMatrix = viewMatrix;
        cameras[0].lightProjectionMatrix = projectionMatrix;
        cameras[0].lightViewProjectionMatrix = projectionMatrix * viewMatrix;
        cameras[0].nearFar = nearFar;
        updateCameraFrustumPlanes(cameras[0].lightViewProjectionMatrix, cameras[0].frustumPlanes);
        numCascades = 1;
    }else{
        // the faces' look-at bases are the ones includes/shadow_sampling.glsl's
        // shadowCubeFaceSpace reads them with
        static const Vector3 faceDirections[6] = {
            Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1)};
        static const Vector3 faceUps[6] = {
            Vector3(0, -1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(0, -1, 0), Vector3(0, -1, 0)};
        const float guard = pointGuardScale(light.shadowSlotResolution[0], ShadowTiers::pointKernelReach(shadowTier));
        const Matrix4 projectionMatrix = Matrix4::perspectiveMatrix(2.0f * std::atan(guard), 1.0f, nearFar.x, nearFar.y);

        // window depth = x + y / distance along the face axis (shadowData.projection.yz)
        Vector2 depthTerms;
        const float range = nearFar.y - nearFar.x;
        depthTerms.x = (nearFar.y + nearFar.x) / range * 0.5f + 0.5f;
        depthTerms.y = -(nearFar.y * nearFar.x) / range;
        for (int f = 0; f < SHADOW_CUBE_FACES; f++){
            cameras[f].lightViewMatrix = Matrix4::lookAtMatrix(transform.worldPosition, faceDirections[f] + transform.worldPosition, faceUps[f]);
            cameras[f].lightProjectionMatrix = projectionMatrix;
            cameras[f].lightViewProjectionMatrix = projectionMatrix * cameras[f].lightViewMatrix;
            cameras[f].nearFar = depthTerms;
            updateCameraFrustumPlanes(cameras[f].lightViewProjectionMatrix, cameras[f].frustumPlanes);
        }
        numCascades = SHADOW_CUBE_FACES;
    }
}

void RenderSystem::updateLightCameras(CameraComponent& camera, Transform& cameraTransform){
    auto lights = scene->getComponentArray<LightComponent>();
    const int numLights = (int)lights->size();
    CameraShadows& frame = primaryView.shadows;
    shadowFitCamera(camera, cameraTransform, true, frame.lightCameras[0].cameras[0].lightViewMatrix, frame.fitCameraPosition, frame.fitCameraForward);
    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        Transform* transform = scene->findComponent<Transform>(lights->getEntity(i));
        if (!transform)
            continue;
        updateLightShadowRevision(light, *transform);
        if (!hasShadows || !light.shadows || light.intensity <= 0.0f || light.shadowMapIndex < 0)
            continue;

        Matrix4 before[SHADOW_CUBE_FACES];
        for (int c = 0; c < SHADOW_CUBE_FACES; c++){
            before[c] = light.cameras[c].lightViewProjectionMatrix;
        }
        unsigned int cascades = light.shadowCascades;
        Vector3 fitPosition = frame.fitCameraPosition;
        Vector3 fitForward = frame.fitCameraForward;
        computeLightCameras(light, *transform, camera, cameraTransform, light.cameras, cascades, light.cascadeFit, light.hasCascadeFit, true,
                            fitPosition, fitForward);
        light.shadowCascades = cascades;
        for (int c = 0; c < SHADOW_CUBE_FACES; c++){
            if (before[c] != light.cameras[c].lightViewProjectionMatrix){
                // the instance views culled for its slots
                instanceViewsDirty = true;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------- shader data

void RenderSystem::processLights(CameraShadows& view, int numLights, const CameraComponent& camera, const Transform& cameraTransform){
    auto lights = scene->getComponentArray<LightComponent>();
    const ShadowTierSettings& tier = shadowTierSettings();

    fs_shadow_data_t& data = view.fs_shadow_data;
    fs_point_shadow_data_t& pointData = view.fs_point_shadow_data;
    data = fs_shadow_data_t();
    pointData = fs_point_shadow_data_t();
    for (int s = 0; s < MAX_SHADOW_ATLAS_SLOTS; s++){
        data.lightVP[s] = Matrix4();
    }

    // tier-wide flags: 1 receiver plane, 2 dithered / 4 blended transition, 8 PCSS
    float flags = 0.0f;
    if (tier.receiverPlane)
        flags += 1.0f;
    if (scene->isShadowCascadeBlend()){
        flags += (tier.transition == CascadeTransition::DITHER) ? 2.0f : ((tier.transition == CascadeTransition::BLEND) ? 4.0f : 0.0f);
    }
    if (tier.softFilter == ShadowSoftFilter::PCSS && !shadowHardOnly && Engine::getGraphicBackend() != GraphicBackend::GLES3)
        flags += 8.0f;
    float debugView = 0.0f;
#ifdef JASPERENGINE_EDITOR
    if (&view == &primaryView.shadows){
        const ShadowDebugView debug = scene->getShadowDebugView();
        if (debug != ShadowDebugView::ATLAS)
            debugView = (float)static_cast<int>(debug);
    }
#endif
    data.header = Vector4(-1.0f, 0.0f, flags, debugView);
    data.fitCameraPosition = Vector4(view.fitCameraPosition.x, view.fitCameraPosition.y, view.fitCameraPosition.z, 0.0f);
    data.fitCameraForward = Vector4(view.fitCameraForward.x, view.fitCameraForward.y, view.fitCameraForward.z, 0.0f);

    // the filter of a light: 0 hard, 1 grid (radius in y), 2 Vogel, 3 PCSS
    auto filterOf = [&](const LightComponent& light, bool point) -> Vector2{
        if (shadowHardOnly || !light.softShadows)
            return Vector2(0.0f, 0.0f);
        switch (tier.softFilter){
            case ShadowSoftFilter::GRID3: return Vector2(1.0f, 1.0f);
            case ShadowSoftFilter::GRID5: return Vector2(1.0f, 2.0f);
            case ShadowSoftFilter::VOGEL: return Vector2(2.0f, 0.0f);
            case ShadowSoftFilter::PCSS: return Vector2(point ? 2.0f : 3.0f, 0.0f);
        }
        return Vector2(0.0f, 0.0f);
    };

    const unsigned int projectiveWidth = shadowPlan.projective.width;
    const unsigned int projectiveHeight = shadowPlan.projective.height;
    const unsigned int pointWidth = shadowPlan.point.width;
    const unsigned int pointHeight = shadowPlan.point.height;
    float mainIntensity = -1.0f;

    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);

        // shadowMapIndex < 0: the planner gave this light no slot this frame; the per-object
        // lists pass that on (cone_shadow.z), which the shader treats as "no shadow"
        if (!light.shadows || light.shadowMapIndex < 0)
            continue;
        const Vector2 filter = filterOf(light, light.type == LightType::POINT);

        if (light.type == LightType::POINT){
            if (light.shadowMapIndex + SHADOW_CUBE_FACES > MAX_POINT_SHADOW_ATLAS_SLOTS)
                continue;
            const int pointLight = light.shadowMapIndex / SHADOW_CUBE_FACES;
            const unsigned int resolution = std::max(light.shadowSlotResolution[0], 1u);
            const float guard = pointGuardScale(resolution, ShadowTiers::pointKernelReach(shadowTier));
            const float texelPerUnit = 2.0f * guard / (float)resolution;
            const LightCamera& face = lightCamera(view, i, light, 0);
            for (int f = 0; f < SHADOW_CUBE_FACES; f++){
                const int slot = light.shadowMapIndex + f;
                pointData.faceRect[slot] = atlasUVRect(shadowPlan.point.rects[slot], pointWidth, pointHeight);
                // the legacy block: atlas rects only (its point lookups read lit)
                view.fs_point_shadows.atlasRect[slot] = pointData.faceRect[slot];
                view.fs_point_shadows.bias_texSize_nearFar[slot] = Vector4(0.0f, (float)resolution, face.nearFar.x, face.nearFar.y);
            }
            pointData.projection[pointLight] = Vector4(guard, face.nearFar.x, face.nearFar.y, texelPerUnit);
            pointData.bias[pointLight] = Vector4(light.shadowBias * texelPerUnit, light.shadowNormalBias * texelPerUnit, 0.0f, (float)resolution);
            pointData.filtering[pointLight] = Vector4(filter.x, filter.y, light.shadowSourceRadius, std::clamp(light.shadowStrength, 0.0f, 1.0f));
            pointData.state[pointLight] = Vector4(pathCode(view.pointSlots[light.shadowMapIndex].lastPath), 0.0f, 0.0f, 0.0f);
            continue;
        }

        const int numMaps = (light.type == LightType::DIRECTIONAL) ? (int)lightCascades(view, i, light) : 1;
        const CascadeFitResult* fit = nullptr;
        if (light.type == LightType::DIRECTIONAL){
            if (view.ownLightCameras && light.shadowMapIndex < MAX_SHADOW_ATLAS_SLOTS && view.lightCameras[light.shadowMapIndex].hasFit)
                fit = &view.lightCameras[light.shadowMapIndex].fit;
            else if (light.hasCascadeFit)
                fit = &light.cascadeFit;
            if (!fit)
                continue;
            if (light.resolvedIntensity > mainIntensity){
                mainIntensity = light.resolvedIntensity;
                data.header.x = (float)light.shadowMapIndex;
                data.header.y = (float)numMaps;
            }
        }

        const float distance = lightShadowDistance(light, camera);
        const float fadeEnd = distance;
        const float fadeStart = distance * (1.0f - std::clamp(scene->getShadowFadeFraction(), 0.0f, 1.0f));
        const float strength = std::clamp(light.shadowStrength, 0.0f, 1.0f);
        const float tanHalfAngle = std::tan(0.5f * Angle::degToRad(std::max(light.shadowAngularDiameter, 0.0f)));

        for (int c = 0; c < numMaps; c++){
            const int slot = light.shadowMapIndex + c;
            if (slot < 0 || slot >= MAX_SHADOW_ATLAS_SLOTS)
                continue;
            const LightCamera& shadowCamera = lightCamera(view, i, light, c);
            const ShadowRect& rect = shadowPlan.projective.rects[slot];
            const float resolution = (float)std::max(rect.width, 1);
            data.lightVP[slot] = shadowCamera.lightViewProjectionMatrix;
            data.atlasRect[slot] = atlasUVRect(rect, projectiveWidth, projectiveHeight);
            data.fade[slot] = Vector4(fadeStart, fadeEnd, pathCode(view.projectiveSlots[slot].lastPath), 0.0f);

            float legacyBias;
            float legacyNormalOffset;
            if (light.type == LightType::DIRECTIONAL){
                const float texel = fit->texelWorld[c];
                const float span = std::max(fit->depthSpan[c], 1e-6f);
                data.bias[slot] = Vector4(light.shadowBias * texel, light.shadowNormalBias * texel, texel, span);
                data.split[slot] = Vector4(fit->splitNear[c], fit->splitFar[c], fit->blendStart[c], resolution);
                const float radius = (filter.x >= 2.0f) ? std::clamp(tanHalfAngle * 2.0f / std::max(texel, 1e-6f), 1.0f, 8.0f) : filter.y;
                data.filtering[slot] = Vector4(filter.x, radius, tanHalfAngle, strength);
                // the legacy compare: [0, 1] depth, about two texels of it
                legacyBias = 2.0f * light.shadowBias * texel / span;
                legacyNormalOffset = light.shadowNormalBias * texel;
            }else{
                // spot: texel size per metre of distance from the light
                const float projectionScale = std::max(std::abs(shadowCamera.lightProjectionMatrix[1][1]), 1e-6f);
                const float texelPerUnit = 2.0f / (projectionScale * resolution);
                data.bias[slot] = Vector4(light.shadowBias * texelPerUnit, light.shadowNormalBias * texelPerUnit, texelPerUnit, shadowCamera.nearFar.y);
                data.split[slot] = Vector4(shadowCamera.nearFar.x, shadowCamera.nearFar.y, shadowCamera.nearFar.y, resolution);
                data.filtering[slot] = Vector4(filter.x, (filter.x >= 2.0f) ? 0.0f : filter.y, light.shadowSourceRadius, strength);
                data.fade[slot].x = 1e30f;
                data.fade[slot].y = 1e30f;
                legacyBias = 0.0005f * light.shadowBias;
                legacyNormalOffset = light.shadowNormalBias * texelPerUnit * 0.5f * (shadowCamera.nearFar.x + shadowCamera.nearFar.y);
            }

            // the legacy blocks of older forks
            view.vs_shadows.lightViewProjectionMatrix[slot] = shadowCamera.lightViewProjectionMatrix;
            view.vs_shadows.shadowParams[slot] = Vector4(legacyNormalOffset, 0.0f, 0.0f, 0.0f);
            view.fs_shadows.bias_texSize_nearFar[slot] = Vector4(legacyBias, resolution, shadowCamera.nearFar.x, shadowCamera.nearFar.y);
            view.fs_shadows.atlasRect[slot] = data.atlasRect[slot];
        }
    }

    // the camera part of the legacy u_fs_lighting (lights and eyePos.w are per draw); the
    // frame block of the camera's passes is filled by updatePassFrame
    view.fs_lighting.eyePos = Vector4(cameraTransform.worldPosition.x, cameraTransform.worldPosition.y, cameraTransform.worldPosition.z, 0.0);
    view.fs_lighting.cameraDir = Vector4(camera.worldDirection.x, camera.worldDirection.y, camera.worldDirection.z, shadowPCFRadius());
    view.fs_lighting.globalIllum = Vector4(scene->getGlobalIlluminationColorLinear(), scene->getGlobalIlluminationIntensity());
    view.fs_lighting.envColor = environmentInfo();
    view.fs_lighting.viewportInfo = lightingViewportInfo(camera);
}

// ---------------------------------------------------------------- bindings

void RenderSystem::updateShadowBindings(){
    if (!needUpdateShadowBindings){
        return;
    }

    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (!mesh.loaded || mesh.needReload){
            continue;
        }

        for (int s = 0; s < mesh.numSubmeshes; s++){
            bool has3DShadows = mesh.submeshes[s].shaderProperties & (1u << 4);  // 'Shw'
            bool has2DShadows = mesh.submeshes[s].shaderProperties & (1u << 23); // 'S2d'
            if (!has3DShadows && !has2DShadows){
                continue;
            }
            if (!mesh.submeshes[s].shader){
                continue;
            }

            ShaderData& shaderData = mesh.submeshes[s].shader.get()->shaderData;
            if (has3DShadows){
                loadShadowTextures(shaderData, mesh.submeshes[s].render, mesh.receiveLights, true);
            }
            if (has2DShadows){
                loadShadow2DTexture(shaderData, mesh.submeshes[s].render, true);
            }
        }
    }

    needUpdateShadowBindings = false;
}

void RenderSystem::loadShadowTextures(ShaderData& shaderData, ObjectRender& render, bool receiveLights, bool receiveShadows){
    if (!(hasLights && receiveLights && hasShadows && receiveShadows))
        return;

    // A fork declaring the legacy u_fs_point_shadows samples a packed colour atlas that is
    // gone: white reads as lit there
    const bool legacyPoint = shaderData.getUniformBlockIndexByName("u_fs_point_shadows") >= 0;
    if (legacyPoint){
        static std::unordered_set<std::string> warned;
        const std::string name = shaderData.stages.empty() ? std::string() : shaderData.stages[0].name;
        if (warned.insert(name).second){
            Log::warn("Shader '%s' has the legacy shadow interface: point lights cast no shadows on it. Re-fork it to get point shadows, the cascade blend and PCSS",
                      name.c_str());
        }
    }

    // the point atlas shares the projective atlas's comparison sampler: bound first, so the
    // projective atlas's binding is the sampler that stays
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::SHADOWPOINTATLAS), ShaderStageType::FRAGMENT,
                      legacyPoint ? &emptyWhite : (hasShadowPointAtlas ? &shadowPointAtlasFramebuffer.getDepthTexture() : &emptyShadowDepth));
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::SHADOWATLAS), ShaderStageType::FRAGMENT,
                      hasShadowAtlas ? &shadowAtlasFramebuffer.getDepthTexture() : &emptyShadowDepth);
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::SHADOWATLASRAW), ShaderStageType::FRAGMENT,
                      (hasShadowAtlas && shadowAtlasRaw.isCreated()) ? &shadowAtlasRaw : &emptyShadowDepthRaw);
}

bool RenderSystem::bindViewShadowMaps(ShaderData& shaderData, ObjectRender& render, bool receiveLights, bool restore){
    const CameraShadows& shadows = ps();
    const bool otherProjective = shadows.projectiveAtlas && shadows.projectiveAtlas != &shadowAtlasFramebuffer;
    const bool otherPoint = shadows.pointAtlas && shadows.pointAtlas != &shadowPointAtlasFramebuffer;
    if ((!otherProjective && !otherPoint) || !hasLights || !receiveLights)
        return false;

    const bool legacyPoint = shaderData.getUniformBlockIndexByName("u_fs_point_shadows") >= 0;
    if (otherPoint && !legacyPoint){
        TextureRender* atlas = restore
            ? (hasShadowPointAtlas ? &shadowPointAtlasFramebuffer.getDepthTexture() : &emptyShadowDepth)
            : &shadows.pointAtlas->getDepthTexture();
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::SHADOWPOINTATLAS), ShaderStageType::FRAGMENT, atlas);
    }
    // the projective atlas after the point atlas: its sampler is the one both read
    TextureRender* projectiveAtlas = hasShadowAtlas ? &shadowAtlasFramebuffer.getDepthTexture() : &emptyShadowDepth;
    TextureRender* projectiveRaw = (hasShadowAtlas && shadowAtlasRaw.isCreated()) ? &shadowAtlasRaw : &emptyShadowDepthRaw;
    if (otherProjective && !restore){
        projectiveAtlas = &shadows.projectiveAtlas->getDepthTexture();
        projectiveRaw = (shadows.projectiveAtlasRaw && shadows.projectiveAtlasRaw->isCreated()) ? shadows.projectiveAtlasRaw : &emptyShadowDepthRaw;
    }
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::SHADOWATLAS), ShaderStageType::FRAGMENT, projectiveAtlas);
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::SHADOWATLASRAW), ShaderStageType::FRAGMENT, projectiveRaw);
    return true;
}

// ---------------------------------------------------------------- slots of a pass

RenderSystem::LodView RenderSystem::shadowLodView(const LightComponent& light, int lightIndex, int cameraIndex, int slot) const{
    LodView view;
    view.enabled = scene->isMeshLodEnabled();
    // shadow LOD (spec S54 R7): the light's own error budget, about one level coarser than the
    // camera's rule (SHADOW_LOD_COARSER) over the project's and the device tier's Shadow LOD Bias
    // (QualityResolver, contract C4: above 1 keeps detail); casters snap, never fade
    view.threshold = SHADOW_LOD_TEXELS * SHADOW_LOD_COARSER / std::max(QualityResolver::shadowLodBias(), 0.001f);
    view.bias = QualityResolver::lodBias();

    const LightCamera& shadowCamera = lightCamera(lightIndex, light, cameraIndex);
    const Matrix4& projection = shadowCamera.lightProjectionMatrix;
    float texel = 2.0f / (std::abs(projection[1][1]) * std::max(getShadowAtlasSlotRect(slot).getHeight(), 1.0f));
    if (light.type == LightType::DIRECTIONAL){
        view.constant = texel;
    }else{
        view.origin = shadowCamera.lightViewMatrix.inverse() * Vector3(0, 0, 0);
        view.perUnit = texel;
    }
    return view;
}

RenderSystem::LodView RenderSystem::pointFaceLodView(const LightComponent& light, int lightIndex, int face) const{
    LodView view;
    view.enabled = scene->isMeshLodEnabled();
    // as a shadow slot's: the light's own budget, over the Shadow LOD Bias (QualityResolver, C4)
    view.threshold = SHADOW_LOD_TEXELS * SHADOW_LOD_COARSER / std::max(QualityResolver::shadowLodBias(), 0.001f);
    view.bias = QualityResolver::lodBias();
    const LightCamera& faceCamera = lightCamera(lightIndex, light, face);
    const Matrix4& projection = faceCamera.lightProjectionMatrix;
    const int slot = std::max(light.shadowMapIndex, 0) + face;
    view.origin = faceCamera.lightViewMatrix.inverse() * Vector3(0, 0, 0);
    view.perUnit = 2.0f / (std::abs(projection[1][1]) * std::max(getShadowPointAtlasSlotRect(slot).getHeight(), 1.0f));
    return view;
}

RenderSystem::ShadowTerrainCut& RenderSystem::shadowTerrainCut(Entity entity, TerrainComponent& terrain, Transform& transform, const MeshComponent& mesh,
                                                              const LightCamera& shadowCamera){
    ShadowTerrainCut& cut = shadowTerrainScratch;
    cut.nodes[0].clear();
    cut.nodes[1].clear();
    if (!terrain.heightMapLoaded)
        return cut;

    if (mesh.staticShadowCaster){
        // from above the slot's centre by its size (a directional cascade: the bigger its
        // texels, the coarser its nodes) or the light (a spot), whatever the camera
        const Matrix4 inverseViewProjection = shadowCamera.lightViewProjectionMatrix.inverse();
        const bool orthographic = shadowCamera.lightProjectionMatrix[3][3] == 1.0f;
        if (orthographic){
            const Vector3 centre = inverseViewProjection * Vector3(0, 0, 0);
            const float radius = (inverseViewProjection * Vector3(1, 1, 0) - centre).length();
            cut.eye = Vector3(centre.x, transform.worldPosition.y + radius, centre.z);
        }else{
            cut.eye = shadowCamera.lightViewMatrix.inverse() * Vector3(0, 0, 0);
        }
        for (int i = 0; i < terrain.rootGridSize * terrain.rootGridSize; i++){
            selectShadowTerrainNode(terrain, transform, cut.eye, shadowCamera.nearFar.y, shadowCamera.frustumPlanes,
                                    terrain.nodes[terrain.grid[i]], terrain.levels - 1, cut);
        }
        return cut;
    }

    // the camera's cut, node by node against the slot's frustum
    TerrainView& view = terrainViewData(entity, terrain, mainTerrainView());
    cut.eye = view.nodesEyePos;
    for (int s = 0; s < 2; s++){
        const InterleavedBuffer& records = view.nodesbuffer[s];
        const size_t count = std::min<size_t>(view.nodeIndices[s].size(), records.getCount());
        if (records.getStride() != 5 * sizeof(float) || !records.getData())
            continue;
        for (size_t k = 0; k < count; k++){
            const uint32_t index = view.nodeIndices[s][k];
            if (index >= terrain.nodes.size())
                continue;
            const AABB box = getTerrainNodeAABB(transform, terrain.nodes[index]);
            if (!isInsideCamera(shadowCamera.nearFar.y, shadowCamera.frustumPlanes, box))
                continue;
            const float* record = reinterpret_cast<const float*>(records.getData() + k * records.getStride());
            cut.nodes[s].insert(cut.nodes[s].end(), record, record + 5);
        }
    }
    return cut;
}

bool RenderSystem::selectShadowTerrainNode(TerrainComponent& terrain, const Transform& transform, const Vector3& eye, float cameraFar,
                                           const Plane frustumPlanes[6], TerrainNode& node, int lodLevel, ShadowTerrainCut& cut){
    const float range = terrain.ranges[lodLevel];
    const AABB box = getTerrainNodeAABB(transform, node);
    if (!isTerrainNodeInSphere(eye, range, box))
        return false;
    if (!isInsideCamera(cameraFar, frustumPlanes, box))
        return true;
    auto append = [&cut](int buffer, const TerrainNode& selected, float selectedRange, float resolution){
        cut.nodes[buffer].insert(cut.nodes[buffer].end(), {selected.position.x, selected.position.y, selected.size, selectedRange, resolution});
    };
    if (lodLevel == 0 || !isTerrainNodeInSphere(eye, terrain.ranges[lodLevel - 1], box)){
        append(0, node, range, (float)terrain.resolution);
        return true;
    }
    for (int i = 0; i < 4; i++){
        TerrainNode& child = terrain.nodes[node.childs[i]];
        if (!selectShadowTerrainNode(terrain, transform, eye, cameraFar, frustumPlanes, child, lodLevel - 1, cut))
            append(1, child, range, (float)(terrain.resolution / 2));
    }
    return true;
}

// projective atlas slots the shadow pass draws this frame
const std::vector<RenderSystem::ShadowSlot>& RenderSystem::collectShadowSlots(){
    std::vector<ShadowSlot>& slots = shadowSlots;
    slots.clear();
    if (!hasShadows || !hasShadowAtlas)
        return slots;

    auto lights = scene->getComponentArray<LightComponent>();
    int shadowLightCount = (int)lights->size();
    for (int l = 0; l < shadowLightCount; l++){
        LightComponent& light = lights->getComponentFromIndex(l);
        if (light.type == LightType::POINT || light.intensity <= 0 || !light.shadows || light.shadowMapIndex < 0 ||
                !isLightActive(lights->getEntity(l), light))
            continue;
        int numShadowCameras = (light.type == LightType::DIRECTIONAL) ? (int)lightCascades(l, light) : 1;
        for (int c = 0; c < numShadowCameras; c++){
            int slot = light.shadowMapIndex + c;
            if (slot >= 0 && slot < MAX_SHADOW_ATLAS_SLOTS && !shadowPlan.projective.rects[slot].empty())
                slots.push_back({slot, &light, l, c});
        }
    }
    return slots;
}

// ---------------------------------------------------------------- per-camera shadows

bool RenderSystem::ShadowAtlasKey::sameDepths(const ShadowAtlasKey& other) const{
    if (slots != other.slots || cameraCasters != other.cameraCasters)
        return false;
    for (int s = 0; s < SHADOW_KEY_SLOTS; s++){
        if (casterMask[s] != other.casterMask[s] || viewProjection[s] != other.viewProjection[s])
            return false;
    }
    return true;
}

RenderSystem::CameraShadows::~CameraShadows(){
    release();
}

void RenderSystem::CameraShadows::releaseProjective(){
    ownProjective.destroyFramebuffer();
    ownProjectiveRaw.destroyTexture();
    ownProjectiveWidth = 0;
    ownProjectiveHeight = 0;
    ownProjectiveGeneration++;
    projectiveKey.epoch = 0;
    if (projectiveAtlas == &ownProjective){
        projectiveAtlas = nullptr;
        projectiveAtlasRaw = nullptr;
    }
}

void RenderSystem::CameraShadows::releasePoint(){
    ownPoint.destroyFramebuffer();
    ownPointWidth = 0;
    ownPointHeight = 0;
    ownPointGeneration++;
    pointKey.epoch = 0;
    if (pointAtlas == &ownPoint)
        pointAtlas = nullptr;
}

void RenderSystem::CameraShadows::releaseInstances(){
    for (InstanceView& view : instanceViews){
        view.buffer.getRender()->destroyBuffer();
        view.data.clear();
        view.data.shrink_to_fit();
        view.capacity = 0;
        view.vatBuffer.getRender()->destroyBuffer();
        view.vatData.clear();
        view.vatData.shrink_to_fit();
        view.vatCapacity = 0;
        view.needUpload = false;
    }
    instanceRanges.clear();
    instanceBuild = 0;
}

void RenderSystem::CameraShadows::release(){
    releaseProjective();
    releasePoint();
    releaseInstances();
    projectiveAtlas = nullptr;
    projectiveAtlasRaw = nullptr;
    pointAtlas = nullptr;
    drawProjective = false;
    drawPoint = false;
    keysEpoch = 0;
}

RenderSystem::CameraShadows& RenderSystem::getCameraShadows(Entity entity){
    std::unique_ptr<CameraShadows>& shadows = cameraShadows[entity];
    if (!shadows){
        shadows = std::make_unique<CameraShadows>();
    }
    return *shadows;
}

// A camera that stopped rendering keeps its maps a moment, so one that comes back
// (re-enabled, a probe capturing again) reallocates nothing.
void RenderSystem::releaseUnusedCameraShadows(){
    static constexpr uint64_t KEEP_UNUSED_DRAWS = 120;
    for (auto it = cameraShadows.begin(); it != cameraShadows.end();){
        if (drawCount - it->second->lastUsed > KEEP_UNUSED_DRAWS){
            it = cameraShadows.erase(it);
        }else{
            ++it;
        }
    }
}

// What target's atlases hold once drawn: each slot's shadow camera and the layers casting
// into it. A slot nothing casts into is plain clear depth, whatever its camera.
void RenderSystem::computeShadowKeys(const CameraShadows& target, ShadowAtlasKey& projective, ShadowAtlasKey& point){
    for (ShadowAtlasKey* key : {&projective, &point}){
        key->slots = 0;
        key->cameraCasters = nullptr;
        for (int s = 0; s < SHADOW_KEY_SLOTS; s++){
            key->viewProjection[s] = Matrix4();
            key->casterMask[s] = 0;
        }
    }
    if (!hasShadows)
        return;

    ensureCasterLayers();

    auto lights = scene->getComponentArray<LightComponent>();
    const int numLights = std::min((int)lights->size(), (int)lightCasterLayers.size());
    for (int l = 0; l < numLights; l++){
        LightComponent& light = lights->getComponentFromIndex(l);
        // the lights renderShadowMaps draws
        if (light.intensity <= 0 || !light.shadows || light.shadowMapIndex < 0 ||
                !isLightActive(lights->getEntity(l), light))
            continue;

        const uint32_t mask = target.cameraMask & light.cullingMask & lightCasterLayers[l];
        const void* cameraCasters = (mask & cameraCasterLayers) ? (const void*)&target : nullptr;

        if (light.type == LightType::POINT){
            if (!hasShadowPointAtlas)
                continue;
            for (int c = 0; c < SHADOW_CUBE_FACES; c++){
                const int slot = light.shadowMapIndex + c;
                if (slot < 0 || slot >= MAX_POINT_SHADOW_ATLAS_SLOTS)
                    continue;
                point.slots++;
                point.casterMask[slot] = mask;
                if (mask)
                    point.viewProjection[slot] = lightCamera(target, l, light, c).lightViewProjectionMatrix;
                if (cameraCasters)
                    point.cameraCasters = cameraCasters;
            }
        }else{
            if (!hasShadowAtlas)
                continue;
            const int numCameras = (light.type == LightType::DIRECTIONAL) ? (int)lightCascades(target, l, light) : 1;
            for (int c = 0; c < numCameras; c++){
                const int slot = light.shadowMapIndex + ((light.type == LightType::DIRECTIONAL) ? c : 0);
                if (slot < 0 || slot >= MAX_SHADOW_ATLAS_SLOTS || shadowPlan.projective.rects[slot].empty())
                    continue;
                projective.slots++;
                projective.casterMask[slot] = mask;
                if (mask)
                    projective.viewProjection[slot] = lightCamera(target, l, light, c).lightViewProjectionMatrix;
                if (cameraCasters)
                    projective.cameraCasters = cameraCasters;
            }
        }
    }
}

// An atlas drawn after this update holding what target wants: the frame's, target's own
// from an earlier draw, or another camera's. Null when none does.
FramebufferRender* RenderSystem::findShadowSource(CameraShadows& target, const ShadowAtlasKey& wanted, bool projective){
    auto holds = [&](const CameraShadows& source){
        const ShadowAtlasKey& key = projective ? source.projectiveKey : source.pointKey;
        return key.epoch == shadowEpoch && key.sameDepths(wanted);
    };

    // the frame's maps, described the first time a camera asks after the update
    CameraShadows& frame = primaryView.shadows;
    if ((frame.projectiveKey.epoch == shadowEpoch || frame.pointKey.epoch == shadowEpoch) && frame.keysEpoch != shadowEpoch){
        const uint64_t projectiveEpoch = frame.projectiveKey.epoch;
        const uint64_t pointEpoch = frame.pointKey.epoch;
        computeShadowKeys(frame, frame.projectiveKey, frame.pointKey);
        frame.projectiveKey.epoch = projectiveEpoch;
        frame.pointKey.epoch = pointEpoch;
        frame.keysEpoch = shadowEpoch;
    }
    if (holds(frame))
        return projective ? &shadowAtlasFramebuffer : &shadowPointAtlasFramebuffer;

    if (holds(target))
        return projective ? &target.ownProjective : &target.ownPoint;

    auto other = [&](CameraShadows& source) -> FramebufferRender*{
        if (&source == &target || !holds(source))
            return nullptr;
        return projective ? &source.ownProjective : &source.ownPoint;
    };
    for (auto& entry : cameraShadows){
        if (FramebufferRender* atlas = other(*entry.second))
            return atlas;
    }
#ifdef JASPERENGINE_EDITOR
    for (auto& entry : secondaryViews){
        if (FramebufferRender* atlas = other(entry.second->shadows))
            return atlas;
        for (auto& mirror : entry.second->mirrors){
            if (FramebufferRender* atlas = other(mirror.second->shadows))
                return atlas;
        }
    }
#endif
    return nullptr;
}

// the raw-depth view of an atlas findShadowSource gave (the frame's, or a camera's own)
TextureRender* RenderSystem::shadowSourceRaw(FramebufferRender* atlas){
    if (atlas == &shadowAtlasFramebuffer)
        return &shadowAtlasRaw;
    for (auto& entry : cameraShadows){
        if (atlas == &entry.second->ownProjective)
            return &entry.second->ownProjectiveRaw;
    }
#ifdef JASPERENGINE_EDITOR
    for (auto& entry : secondaryViews){
        if (atlas == &entry.second->shadows.ownProjective)
            return &entry.second->shadows.ownProjectiveRaw;
        for (auto& mirror : entry.second->mirrors){
            if (atlas == &mirror.second->shadows.ownProjective)
                return &mirror.second->shadows.ownProjectiveRaw;
        }
    }
#endif
    return nullptr;
}

// A camera's own atlases have the frame's layout, so the slots and their uniforms line up
bool RenderSystem::ensureOwnShadowAtlas(CameraShadows& target, bool projective){
    const ShadowAtlasLayout& layout = projective ? shadowPlan.projective : shadowPlan.point;
    const int width = (int)layout.width;
    const int height = (int)layout.height;
    FramebufferRender& atlas = projective ? target.ownProjective : target.ownPoint;
    int& ownWidth = projective ? target.ownProjectiveWidth : target.ownPointWidth;
    int& ownHeight = projective ? target.ownProjectiveHeight : target.ownPointHeight;
    if (atlas.isCreated() && ownWidth == width && ownHeight == height)
        return true;
    if (projective)
        target.releaseProjective();
    else
        target.releasePoint();
    // LINEAR on a comparison sampler is hardware PCF, as on the frame's atlases
    JE_GPU_ALLOC_SCOPE(ShadowMaps, projective ? "Camera CSM atlas" : "Camera point shadow atlas");
    if (width <= 0 || height <= 0 || !atlas.createDepthOnlyFramebuffer(
            width, height,
            TextureFilter::LINEAR, TextureFilter::LINEAR,
            TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER, true)){
        return false;
    }
    if (projective)
        target.ownProjectiveRaw.createAliasView(atlas.getDepthTexture(), "camera-shadow-atlas-raw");
    ownWidth = width;
    ownHeight = height;
    return true;
}

void RenderSystem::prepareCameraShadows(CameraShadows& target, const CameraComponent& camera, const Transform& cameraTransform, int terrainView, bool ownInstances){
    target.lastUsed = drawCount;
    target.cameraMask = camera.cullingMask;
    target.terrainView = terrainView;
    target.ownLightCameras = true;
    target.ownShadowInstances = ownInstances;

    auto lights = scene->getComponentArray<LightComponent>();
    const int numLights = (int)lights->size();

    // directional cascades fitted to this camera's frustum, by the light's base shadow slot
    Matrix4 fitView;
    shadowFitCamera(camera, cameraTransform, false, fitView, target.fitCameraPosition, target.fitCameraForward);
    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        if (light.type != LightType::DIRECTIONAL || light.shadowMapIndex < 0 || light.shadowMapIndex >= MAX_SHADOW_ATLAS_SLOTS)
            continue;
        LightCameraSet& cameras = target.lightCameras[light.shadowMapIndex];
        cameras.numCascades = light.shadowCascades;

        if (!hasShadows || !light.shadows || light.intensity <= 0)
            continue;

        Transform* transform = scene->findComponent<Transform>(lights->getEntity(i));
        if (!transform){
            for (int c = 0; c < MAX_SHADOWCASCADES; c++){
                cameras.cameras[c] = light.cameras[c];
            }
            continue;
        }
        cameras.numCascades = lightShadowCascades(light, camera);
        computeLightCameras(light, *transform, camera, cameraTransform, cameras.cameras, cameras.numCascades, cameras.fit, cameras.hasFit, false,
                            target.fitCameraPosition, target.fitCameraForward);
    }

    processLights(target, numLights, camera, cameraTransform);

    target.projectiveAtlas = nullptr;
    target.projectiveAtlasRaw = nullptr;
    target.pointAtlas = nullptr;
    target.drawProjective = false;
    target.drawPoint = false;

    if (!hasShadows){
        target.releaseProjective();
        target.releasePoint();
        target.releaseInstances();
        return;
    }

    computeShadowKeys(target, wantedProjectiveKey, wantedPointKey);

    if (wantedProjectiveKey.slots == 0){
        target.releaseProjective();
        target.releaseInstances();
    }else if (FramebufferRender* source = findShadowSource(target, wantedProjectiveKey, true)){
        target.projectiveAtlas = source;
        target.projectiveAtlasRaw = shadowSourceRaw(source);
        if (source != &target.ownProjective){
            target.releaseProjective();
            target.releaseInstances();
            target.projectiveAtlas = source;
            target.projectiveAtlasRaw = shadowSourceRaw(source);
        }
    }else if (ensureOwnShadowAtlas(target, true)){
        target.projectiveKey = wantedProjectiveKey;
        target.projectiveKey.epoch = 0; // until drawn
        target.projectiveAtlas = &target.ownProjective;
        target.projectiveAtlasRaw = &target.ownProjectiveRaw;
        target.drawProjective = true;
    }

    if (wantedPointKey.slots == 0){
        target.releasePoint();
    }else if (FramebufferRender* source = findShadowSource(target, wantedPointKey, false)){
        target.pointAtlas = source;
        if (source != &target.ownPoint){
            target.releasePoint();
            target.pointAtlas = source;
        }
    }else if (ensureOwnShadowAtlas(target, false)){
        target.pointKey = wantedPointKey;
        target.pointKey.epoch = 0; // until drawn
        target.pointAtlas = &target.ownPoint;
        target.drawPoint = true;
    }
}
void RenderSystem::buildCameraShadowInstances(CameraShadows& target){
    if (!target.drawProjective || !target.ownShadowInstances)
        return;

    auto instmeshes = scene->getComponentArray<InstancedMeshComponent>();
    if (instmeshes->size() == 0){
        if (target.instanceBuild != 0)
            target.releaseInstances();
        return;
    }

    // the slots, their cameras and detail levels are target's
    CameraShadows* previousShadows = passShadows;
    passShadows = &target;

    const std::vector<ShadowSlot>& slots = collectShadowSlots();

    uint64_t shadowHash = 1469598103934665603ULL;
    for (const ShadowSlot& shadow : slots){
        const LightCamera& shadowCamera = lightCamera(shadow.lightIndex, *shadow.light, shadow.cameraIndex);
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&shadowCamera.lightViewProjectionMatrix);
        for (size_t b = 0; b < sizeof(Matrix4); b++){
            shadowHash = (shadowHash ^ bytes[b]) * 1099511628211ULL;
        }
        shadowHash = (shadowHash ^ (uint64_t)(shadow.slot + 1)) * 1099511628211ULL;
    }

    if (target.instanceBuild != 0 && target.instanceRevision == instanceRevision && target.instanceShadowHash == shadowHash){
        passShadows = previousShadows;
        return;
    }
    target.instanceBuild++;
    target.instanceRevision = instanceRevision;
    target.instanceShadowHash = shadowHash;

    for (InstanceView& view : target.instanceViews){
        view.data.clear();
        view.vatData.clear();
    }

    for (int i = 0; i < instmeshes->size(); i++){
        InstancedMeshComponent& instmesh = instmeshes->getComponentFromIndex(i);
        Entity entity = instmeshes->getEntity(i);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);

        CameraShadows::InstanceRanges& ranges = target.instanceRanges[entity];
        ranges.build = target.instanceBuild;
        ranges.slots.fill(InstanceViewRange());
        if (!mesh || !transform || !mesh->loaded || !mesh->castsShadowMaps())
            continue;

        // the frame's list, the one this camera's color pass draws
        const InstanceRenderData* instances = instmesh.renderInstances.data();
        const InstanceVatRenderData* vat = instanceVatData(instmesh);
        const InstanceBounds* bounds = instmesh.renderBounds.data();
        const unsigned int count = (instmesh.renderBounds.size() < instmesh.numVisible) ? 0 : instmesh.numVisible;

        for (const ShadowSlot& shadow : slots){
            const LightCamera& shadowCamera = lightCamera(shadow.lightIndex, *shadow.light, shadow.cameraIndex);
            LodView shadowLod = shadowLodView(*shadow.light, shadow.lightIndex, shadow.cameraIndex, shadow.slot);
            Vector3 lightDirection = (shadow.light->type == LightType::DIRECTIONAL) ? shadow.light->worldDirection : Vector3::ZERO;
            buildInstanceView(target.instanceViews[shadow.slot], ranges.slots[shadow.slot], instances, vat, bounds, count, instmesh, *mesh, *transform,
                shadowCamera.nearFar.y, shadowCamera.frustumPlanes, shadowLod, shadowLod.origin, shadowLod.origin, lightDirection, InstanceOrder::NEAREST, false);
        }
    }

    // batches that went away
    for (auto it = target.instanceRanges.begin(); it != target.instanceRanges.end();){
        if (it->second.build != target.instanceBuild){
            it = target.instanceRanges.erase(it);
        }else{
            ++it;
        }
    }

    for (InstanceView& view : target.instanceViews){
        view.needUpload = !view.data.empty();
    }
    uploadInstanceViews(target.instanceViews, MAX_SHADOW_ATLAS_SLOTS);

    passShadows = previousShadows;
}

void RenderSystem::renderCameraShadows(CameraShadows& target){
    if (!target.drawProjective && !target.drawPoint)
        return;

    CameraShadows* previousShadows = passShadows;
    passShadows = &target;
    renderShadowMaps(target.drawProjective, target.drawPoint);
    passShadows = previousShadows;

    if (target.drawProjective)
        target.projectiveKey.epoch = shadowEpoch;
    if (target.drawPoint)
        target.pointKey.epoch = shadowEpoch;
    target.drawProjective = false;
    target.drawPoint = false;
}

// ---------------------------------------------------------------- the shadow cache's program

bool RenderSystem::loadShadowRestore(){
    if (shadowRestoreLoaded)
        return true;
    shadowRestoreShader = ShaderPool::get(ShaderType::SHADOW_CACHE, 0);
    if (!shadowRestoreShader || !shadowRestoreShader->isCreated())
        return false;
    shadowRestoreRender.beginLoad(PrimitiveType::TRIANGLES);
    shadowRestoreRender.setShader(shadowRestoreShader.get());
    ShaderData& data = shadowRestoreShader->shaderData;
    shadowRestoreSlotParams = data.getUniformBlockIndexByName("u_fs_shadowRestore");
    shadowRestoreSourceSlot = data.getTextureIndexByName("u_shadowSource");
    // a pass of the frame: created at once, never skipped (spec S32 §6.4)
    shadowRestoreRender.setImmediatePipelines(true);
    if (!shadowRestoreRender.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE))
        return false;
    shadowRestoreLoaded = true;
    return true;
}

// One fullscreen triangle in the depth-only pass being drawn: the slot's viewport, the scissor
// of what is written, every texel overwritten: the source's depths copied, or the far plane
// (clear)
void RenderSystem::drawShadowRestore(CameraRender& pass, TextureRender* source, const ShadowRect& sourceRect, unsigned int sourceWidth, unsigned int sourceHeight,
                                     const ShadowRect& slotRect, const ShadowRect& scissor, bool clear){
    PipelineState state;
    state.target.colorless = true;
    state.target.depth = DepthStencilFormat::DEPTH;
    state.target.sampleCount = 1;
    state.blend = BlendMode::NONE;
    state.writeMask = ColorWriteMask::NONE;
    // NOT_EQUAL, not ALWAYS: Vulkan writes depth only with the depth test on, and sokol turns
    // the test off for ALWAYS; a texel already holding the value needs no write anyway
    state.depthCompare = CompareFunc::NOT_EQUAL;
    state.depthWrite = true;
    if (!shadowRestoreRender.beginDraw(state))
        return;
    pass.applyViewport(toRect(slotRect));
    pass.applyScissor(toRect(scissor));
    fs_shadow_restore_t params;
    params.sourceRect = clear ? Vector4(0.0f, 0.0f, 1.0f, 1.0f) : atlasUVRect(sourceRect, sourceWidth, sourceHeight);
    params.params = Vector4(clear ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    shadowRestoreRender.addTexture(shadowRestoreSourceSlot, ShaderStageType::FRAGMENT, (clear || !source) ? &emptyShadowDepthRaw : source);
    shadowRestoreRender.applyUniformBlock(shadowRestoreSlotParams, sizeof(fs_shadow_restore_t), &params);
    shadowRestoreRender.draw(0, 3, 1);
}

// ---------------------------------------------------------------- shadow passes

// Depth passes of the shadow-casting lights into the projective and point atlases of the
// camera being drawn (ps(): the frame's for the scene camera, its own otherwise), through its
// shadow cameras. Every slot asks the shadow cache first (ShadowCache, spec S10 R17): kept
// slots are not drawn, a slot whose dynamic casters alone changed gets its static layer copied
// back under them and only them redrawn, and a pass whose slots all hold is not begun at all.
void RenderSystem::renderShadowMaps(bool projective, bool point){
    if (!hasShadows)
        return;

#ifdef JASPERENGINE_EDITOR
    // the scene's editor gizmos and icons never cast shadows
    passDrawsOverlays = false;
#endif

    CameraShadows& target = ps();
    const bool frame = &target == &primaryView.shadows;
    // the frame's atlases cache with a static layer; another camera's skip or redraw a slot
    const bool caching = frame;
    ensureCasterLayers();
    loadShadowRestore();

    auto lights = scene->getComponentArray<LightComponent>();
    auto meshes = scene->getComponentArray<MeshComponent>();
    const int shadowLightCount = (int)lights->size();
    // the lights a caster of skipped, its pipeline still being created or warmed (spec S32):
    // their cached slots are not complete, so the next frame draws them again
    std::vector<int> waitedLights;
    // the caster changes an atlas has not evaluated yet; one that fell behind the log's first
    // entry missed some: its slots are redrawn
    const uint64_t changesEnd = casterChangeBase + casterChangeLog.size();
    auto unseenChanges = [&](bool pointAtlas) -> std::span<const ShadowCasterChange>{
        uint64_t& seen = pointAtlas ? target.pointChangesSeen : target.projectiveChangesSeen;
        if (seen < casterChangeBase){
            if (pointAtlas)
                target.pointSlots.fill(ShadowSlotState());
            else
                target.projectiveSlots.fill(ShadowSlotState());
            seen = casterChangeBase;
        }
        const size_t first = (size_t)std::min<uint64_t>(seen - casterChangeBase, casterChangeLog.size());
        seen = changesEnd;
        return std::span<const ShadowCasterChange>(casterChangeLog.data() + first, casterChangeLog.size() - first);
    };

    // as in Unity, a layer the rendering camera or the light culls casts no shadow
    const uint32_t shadowCameraMask = target.cameraMask;

    // the casters the slots draw from, looked up once for every slot (spec S55 R4)
    struct Caster {
        Entity entity;
        MeshComponent* mesh;
        Transform* transform;
        InstancedMeshComponent* instmesh;
        TerrainComponent* terrain;
        TilemapComponent* tilemap;
    };
    std::vector<Caster> casterList;
    casterList.reserve(meshes->size());
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        Entity entity = meshes->getEntity(i);
        if (!mesh.castShadows)
            continue;
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !transform->visible || !isRenderActive(entity))
            continue;
        casterList.push_back({entity, &mesh, transform, scene->findComponent<InstancedMeshComponent>(entity),
                              scene->findComponent<TerrainComponent>(entity), scene->findComponent<TilemapComponent>(entity)});
    }

    // casters: 1 static, 2 dynamic, 3 both; a projective slot has its own culled instance
    // view, point faces draw everything
    auto drawShadowCasters = [&](LightComponent& light, int lightIndex, int cameraIndex, int instanceView, int casters){
        const LightCamera& shadowCamera = lightCamera(lightIndex, light, cameraIndex);
        const LodView shadowLod = (instanceView >= 0) ? shadowLodView(light, lightIndex, cameraIndex, instanceView - 1) : pointFaceLodView(light, lightIndex, cameraIndex);
        const LodView* lodView = &shadowLod;
        const uint32_t casterMask = shadowCameraMask & light.cullingMask;
        // Dynamic Batching: casters of one depth state draw as one (depth only: in any order)
        beginBatchPass(BatchPass::SHADOW, BatchMode::GROUP, nullptr, nullptr, PIP_SHADOW_DEPTH, shadowCamera.lightViewProjectionMatrix,
                       shadowCamera.nearFar.y, shadowCamera.frustumPlanes);
        batchPass.instanceView = instanceView;
        batchPass.lodView = lodView;

        auto drawCaster = [&](Entity entity, MeshComponent& mesh, Transform& transform, InstancedMeshComponent* instmesh,
                              TerrainComponent* terrain, TilemapComponent* tilemap){
            const Matrix4& model = objectModel(transform);
            vs_depth_t vsDepthParams;
            if (transform.billboard && mesh.shadowsBillboard){
                Matrix4 modelViewMatrix = shadowCamera.lightViewMatrix * model;

                modelViewMatrix.set(0, 0, transform.worldScale.x);
                modelViewMatrix.set(0, 1, 0.0);
                modelViewMatrix.set(0, 2, 0.0);

                if (!transform.cylindricalBillboard) {
                    modelViewMatrix.set(1, 0, 0.0);
                    modelViewMatrix.set(1, 1, transform.worldScale.y);
                    modelViewMatrix.set(1, 2, 0.0);
                }

                modelViewMatrix.set(2, 0, 0.0);
                modelViewMatrix.set(2, 1, 0.0);
                modelViewMatrix.set(2, 2, transform.worldScale.z);

                const Matrix4& projectionMatrix = shadowCamera.lightProjectionMatrix;
                vsDepthParams = {modelViewMatrix, projectionMatrix, projectionMatrix * modelViewMatrix};
            }else{
                const Matrix4& lightVP = shadowCamera.lightViewProjectionMatrix;
                vsDepthParams = {model, lightVP, lightVP * model};
            }

            passShadowTerrain = terrain ? &shadowTerrainCut(entity, *terrain, transform, mesh, shadowCamera) : nullptr;
            // (a billboard's shadow faces the light: its own view matrix, drawn alone)
            if (transform.billboard && mesh.shadowsBillboard){
                drawMeshDepth(entity, mesh, transform, shadowCamera.nearFar.y, shadowCamera.frustumPlanes, vsDepthParams,
                              instmesh, terrain, tilemap, false, PIP_SHADOW_DEPTH, instanceView, lodView);
            }else{
                submitMeshDepth(entity, mesh, transform, shadowCamera.nearFar.y, shadowCamera.frustumPlanes, vsDepthParams,
                                instmesh, terrain, tilemap, false, PIP_SHADOW_DEPTH, instanceView, lodView);
            }
            passShadowTerrain = nullptr;
        };

        // GPU Instancing: the slot's casters culled at once (on jobs) and drawn in batches, in
        // component order (spec S55); else one by one, as before
        const bool batching = SystemRender::isDrawBatching();
        depthQueue.clear();
        const Vector3 eye = shadowCamera.lightViewMatrix.inverse() * Vector3(0, 0, 0);
        for (const Caster& caster : casterList){
            MeshComponent& mesh = *caster.mesh;
            if (!(casters & (mesh.staticShadowCaster ? 1 : 2)))
                continue;
            if (!isLayerVisible(caster.entity, casterMask))
                continue;
            if (batching){
                const float distance = mesh.worldAABB.isNull() ? 0.0f : mesh.worldAABB.squaredDistance(eye);
                depthQueue.push_back({caster.entity, caster.mesh, caster.instmesh, caster.terrain, caster.tilemap, caster.transform,
                                      distance, 0, (uint32_t)depthQueue.size()});
                continue;
            }
            drawCaster(caster.entity, mesh, *caster.transform, caster.instmesh, caster.terrain, caster.tilemap);
        }
        if (batching){
            VisibleFrustum frustum;
            frustum.planes = shadowCamera.frustumPlanes;
            frustum.farClip = shadowCamera.nearFar.y;
            frustum.eye = eye;
            buildVisibleQueue(depthQueue, frustum, PIP_SHADOW_DEPTH, true);
            SystemRender::beginDrawBatch();
            for (const OpaqueRenderData& entry : depthQueue){
                passSubmeshMask = entry.submeshMask;
                passPreCulled = entry.preCulled;
                drawCaster(entry.entity, *entry.mesh, *entry.transform, entry.instmesh, entry.terrain, entry.tilemap);
            }
            SystemRender::endDrawBatch();
            passSubmeshMask = 0xFFFFFFFFu;
            passPreCulled = false;
        }
        endBatchPass();
    };

    // what one slot of an atlas does this frame
    struct SlotWork {
        int light = -1;
        int camera = 0;
        int slot = -1;
        ShadowRect rect;
        ShadowRect staticRect;
        ShadowSlotDecision decision;
    };

    auto runAtlas = [&](bool pointAtlas){
        const ShadowAtlasLayout& layout = pointAtlas ? shadowPlan.point : shadowPlan.projective;
        const ShadowAtlasLayout& staticLayout = pointAtlas ? shadowPlan.pointStatic : shadowPlan.projectiveStatic;
        FramebufferRender& atlas = pointAtlas ? shadowPointAtlasTarget() : shadowAtlasTarget();
        FramebufferRender& staticAtlas = pointAtlas ? shadowStaticPointAtlas : shadowStaticAtlas;
        TextureRender& staticRaw = pointAtlas ? shadowStaticPointAtlasRaw : shadowStaticAtlasRaw;
        CameraRender& pass = pointAtlas ? shadowPointAtlasPassRender : shadowAtlasPassRender;
        std::array<ShadowSlotState, MAX_POINT_SHADOW_ATLAS_SLOTS>* pointStates = &target.pointSlots;
        std::array<ShadowSlotState, MAX_SHADOW_ATLAS_SLOTS>* projectiveStates = &target.projectiveSlots;
        const uint64_t generation = pointAtlas ? (frame ? shadowPointAtlasGeneration : target.ownPointGeneration)
                                               : (frame ? shadowAtlasGeneration : target.ownProjectiveGeneration);
        const uint64_t staticGeneration = pointAtlas ? shadowStaticPointGeneration : shadowStaticGeneration;
        const std::span<const ShadowCasterChange> changes = unseenChanges(pointAtlas);
        const bool hasAtlas = (pointAtlas ? hasShadowPointAtlas : hasShadowAtlas) && atlas.isCreated();
        if (!hasAtlas)
            return;
        // the static layer needs the copy program and its atlas
        const bool staticAvailable = caching && staticAtlas.isCreated() && staticRaw.isCreated() && shadowRestoreLoaded;

        std::vector<SlotWork> work;
        for (int l = 0; l < shadowLightCount; l++){
            LightComponent& light = lights->getComponentFromIndex(l);
            if ((light.type == LightType::POINT) != pointAtlas ||
                light.intensity <= 0 || !light.shadows || light.shadowMapIndex < 0 ||
                !isLightActive(lights->getEntity(l), light)){
                continue;
            }
            const int cameras = pointAtlas ? SHADOW_CUBE_FACES :
                ((light.type == LightType::DIRECTIONAL) ? (int)lightCascades(l, light) : 1);
            const uint32_t mask = shadowCameraMask & light.cullingMask &
                ((l < (int)lightCasterLayers.size()) ? lightCasterLayers[l] : 0u);
            for (int c = 0; c < cameras; c++){
                const int slot = light.shadowMapIndex + c;
                if (slot < 0 || slot >= (int)layout.rects.size() || layout.rects[slot].empty())
                    continue;
                SlotWork item;
                item.light = l;
                item.camera = c;
                item.slot = slot;
                item.rect = layout.rects[slot];
                if (slot < (int)staticLayout.rects.size())
                    item.staticRect = staticLayout.rects[slot];

                ShadowSlotInput input;
                input.key.viewProjection = lightCamera(l, light, c).lightViewProjectionMatrix;
                input.key.rect = item.rect;
                input.key.casterMask = mask;
                input.key.lightRevision = light.shadowRevision;
                input.key.atlasGeneration = generation;
                input.key.cameraCasters = (mask & cameraCasterLayers) != 0;
                input.mode = light.shadowUpdateMode;
                input.caching = caching;
                input.hasStaticLayer = staticAvailable && !item.staticRect.empty();
                input.staticRect = item.staticRect;
                input.staticGeneration = staticGeneration;
                input.requested = light.shadowUpdateRequested;
                input.forceRerender = frame && shadowDebug.forceRerender;
                input.freezeCache = frame && shadowDebug.freezeCache;
                ShadowSlotState& state = pointAtlas ? (*pointStates)[slot] : (*projectiveStates)[slot];
                item.decision = ShadowCache::evaluate(state, input, changes, shadowFrame);
                work.push_back(item);
            }
        }

        bool anyDrawn = false;
        bool anyKept = false;
        bool anyStatic = false;
        for (const SlotWork& item : work){
            const ShadowSlotPath path = item.decision.path;
            anyDrawn = anyDrawn || path != ShadowSlotPath::SKIP;
            anyKept = anyKept || path == ShadowSlotPath::SKIP || path == ShadowSlotPath::RESTORE_DYNAMIC;
            anyStatic = anyStatic || path == ShadowSlotPath::RENDER_STATIC;
        }
        if (!anyDrawn)
            return;
        // without the copy / clear program (not built yet, or not shipped) a slot cannot be
        // cleared inside a kept atlas: the pass clears it whole and every slot is drawn
        if (anyKept && !shadowRestoreLoaded){
            for (SlotWork& item : work){
                if (item.decision.path == ShadowSlotPath::SKIP){
                    item.decision.path = ShadowSlotPath::FULL;
                    item.decision.drawStatic = true;
                    item.decision.drawDynamic = true;
                }
            }
            anyKept = false;
        }

        // the static layers first: static casters into the static cache
        if (anyStatic){
            shadowStaticPassRender.setLabel(pointAtlas ? "point shadow static" : "shadow static");
            shadowStaticPassRender.setDepthStencilActions(LoadAction::LOAD, StoreAction::STORE, 1.0f);
            shadowStaticPassRender.startRenderPass(&staticAtlas);
            for (const SlotWork& item : work){
                if (item.decision.path != ShadowSlotPath::RENDER_STATIC)
                    continue;
                LightComponent& light = lights->getComponentFromIndex(item.light);
                drawShadowRestore(shadowStaticPassRender, nullptr, ShadowRect(), 0, 0, item.staticRect, item.staticRect, true);
                shadowStaticPassRender.applyViewport(toRect(item.staticRect));
                shadowStaticPassRender.applyScissor(toRect(item.staticRect));
                passResolution = Vector2((float)item.staticRect.width, (float)item.staticRect.height);
                const uint32_t waitingBefore = ObjectRender::getWaitingDrawCount();
                drawShadowCasters(light, item.light, item.camera, pointAtlas ? -1 : 1 + item.slot, 1);
                if (ObjectRender::getWaitingDrawCount() != waitingBefore)
                    waitedLights.push_back(item.light);
            }
            shadowStaticPassRender.endRenderPass();
            if (frame)
                shadowStats.passes++;
        }

        // then the atlas: loaded when a slot is kept, cleared when every slot is redrawn
        pass.setLabel(pointAtlas ? "point shadow" : "shadow");
        pass.setDepthStencilActions(anyKept ? LoadAction::LOAD : LoadAction::CLEAR, StoreAction::STORE, 1.0f);
        pass.startRenderPass(&atlas);
        if (frame)
            shadowStats.passes++;
        for (const SlotWork& item : work){
            const ShadowSlotDecision& decision = item.decision;
            if (decision.path == ShadowSlotPath::SKIP)
                continue;
            LightComponent& light = lights->getComponentFromIndex(item.light);
            const uint32_t slotDrawsBefore = Engine::getPendingDrawCalls();
            ShadowRect scissor = item.rect;
            if (decision.path == ShadowSlotPath::RESTORE_DYNAMIC)
                scissor = decision.dirtyRect;
            if (decision.copyStatic){
                drawShadowRestore(pass, &staticRaw, item.staticRect, staticLayout.width, staticLayout.height, item.rect, scissor, false);
            }else if (decision.clearSlot && anyKept){
                drawShadowRestore(pass, nullptr, ShadowRect(), 0, 0, item.rect, item.rect, true);
            }
            pass.applyViewport(toRect(item.rect));
            pass.applyScissor(toRect(scissor));
            passResolution = Vector2((float)item.rect.width, (float)item.rect.height);
            const int casters = (decision.path == ShadowSlotPath::RESTORE_DYNAMIC || decision.path == ShadowSlotPath::RENDER_STATIC) ? 2 : 3;
            const uint32_t waitingBefore = ObjectRender::getWaitingDrawCount();
            drawShadowCasters(light, item.light, item.camera, pointAtlas ? -1 : 1 + item.slot, casters);
            if (ObjectRender::getWaitingDrawCount() != waitingBefore)
                waitedLights.push_back(item.light);
            if (frame)
                shadowStats.drawCalls[static_cast<int>(decision.path)] += Engine::getPendingDrawCalls() - slotDrawsBefore;
        }
        pass.endRenderPass();
    };

    if (frame){
        shadowStats.passes = 0;
        for (uint32_t& count : shadowStats.drawCalls)
            count = 0;
    }
    if (projective)
        runAtlas(false);
    if (point)
        runAtlas(true);

    if (frame){
        // the Cache debug view shows this frame's paths; requested updates are done
        for (int s = 0; s < MAX_SHADOW_ATLAS_SLOTS; s++){
            primaryView.shadows.fs_shadow_data.fade[s].z = pathCode(target.projectiveSlots[s].lastPath);
        }
        for (int l = 0; l < MAX_POINT_SHADOW_LIGHTS; l++){
            primaryView.shadows.fs_point_shadow_data.state[l].x = pathCode(target.pointSlots[l * SHADOW_CUBE_FACES].lastPath);
        }
        for (int l = 0; l < shadowLightCount; l++){
            lights->getComponentFromIndex(l).shadowUpdateRequested = false;
        }
        for (int l : waitedLights){
            lights->getComponentFromIndex(l).shadowUpdateRequested = true;
        }
        fillShadowStats();
    }
}

// ---------------------------------------------------------------- debug and statistics

void RenderSystem::setShadowDebug(const ShadowDebugSettings& settings){
    shadowDebug = settings;
}

void RenderSystem::fillShadowStats(){
    ShadowStats& stats = shadowStats;
    stats.tier = shadowTier;
    stats.hardOnly = shadowHardOnly;
    stats.frame = shadowFrame;
    stats.projectiveWidth = shadowPlan.projective.width;
    stats.projectiveHeight = shadowPlan.projective.height;
    stats.pointWidth = shadowPlan.point.width;
    stats.pointHeight = shadowPlan.point.height;
    stats.staticWidth = shadowStaticAtlas.isCreated() ? shadowPlan.projectiveStatic.width : 0;
    stats.staticHeight = shadowStaticAtlas.isCreated() ? shadowPlan.projectiveStatic.height : 0;
    stats.staticPointWidth = shadowStaticPointAtlas.isCreated() ? shadowPlan.pointStatic.width : 0;
    stats.staticPointHeight = shadowStaticPointAtlas.isCreated() ? shadowPlan.pointStatic.height : 0;
    stats.memoryBytes = 4ull * ((uint64_t)stats.projectiveWidth * stats.projectiveHeight + (uint64_t)stats.pointWidth * stats.pointHeight +
                                (uint64_t)stats.staticWidth * stats.staticHeight + (uint64_t)stats.staticPointWidth * stats.staticPointHeight);
    stats.lights.clear();

    const CameraShadows& frame = primaryView.shadows;
    CameraComponent* sceneCamera = scene->findComponent<CameraComponent>(scene->getCamera());
    auto lights = scene->getComponentArray<LightComponent>();
    for (int l = 0; l < (int)lights->size(); l++){
        const LightComponent& light = lights->getComponentFromIndex(l);
        if (!light.shadows)
            continue;
        ShadowLightStats entry;
        entry.entity = lights->getEntity(l);
        entry.type = light.type;
        entry.direction = light.worldDirection.normalized();
        entry.firstSlot = light.shadowMapIndex;
        entry.cascades = (light.type == LightType::DIRECTIONAL) ? light.shadowCascades : 0;
        for (int s = 0; s < 6; s++){
            entry.resolution[s] = light.shadowSlotResolution[s];
        }
        if (light.type == LightType::DIRECTIONAL && light.hasCascadeFit){
            entry.shadowDistance = sceneCamera ? lightShadowDistance(light, *sceneCamera) : 0.0f;
            for (unsigned int c = 0; c < light.cascadeFit.cascades && c < MAX_SHADOWCASCADES; c++){
                entry.splitFar[c] = light.cascadeFit.splitFar[c];
                entry.radius[c] = light.cascadeFit.radius[c];
                entry.texelWorld[c] = light.cascadeFit.texelWorld[c];
            }
        }
        if (light.shadowMapIndex >= 0){
            const bool point = light.type == LightType::POINT;
            const int count = point ? SHADOW_CUBE_FACES : ((light.type == LightType::DIRECTIONAL) ? (int)light.shadowCascades : 1);
            for (int c = 0; c < count; c++){
                const int slot = light.shadowMapIndex + c;
                const ShadowAtlasLayout& layout = point ? shadowPlan.point : shadowPlan.projective;
                const ShadowAtlasLayout& staticLayout = point ? shadowPlan.pointStatic : shadowPlan.projectiveStatic;
                if (slot < 0 || slot >= (int)layout.rects.size())
                    continue;
                const ShadowSlotState& state = point ? frame.pointSlots[slot] : frame.projectiveSlots[slot];
                ShadowSlotStats slotStats;
                slotStats.slot = slot;
                slotStats.point = point;
                slotStats.rect = layout.rects[slot];
                slotStats.resolution = (unsigned int)std::max(slotStats.rect.width, 0);
                slotStats.path = state.lastPath;
                slotStats.cause = state.lastCause;
                slotStats.renderPath = state.renderPath;
                slotStats.renderCause = state.renderCause;
                slotStats.framesSinceRender = state.valid ? shadowFrame - state.lastRender : 0;
                slotStats.dirtyRect = state.lastDirtyRect;
                slotStats.staticLayer = slot < (int)staticLayout.rects.size() && !staticLayout.rects[slot].empty();
                entry.slots.push_back(slotStats);
            }
        }
        stats.lights.push_back(entry);
    }
}

// The Atlas debug view: the projective atlas, the point atlas and the static caches side by
// side, as grey depths, in one colour image the Scene view draws over the scene
void RenderSystem::renderShadowAtlasDebug(){
#ifdef JASPERENGINE_EDITOR
    if (scene->getShadowDebugView() != ShadowDebugView::ATLAS){
        if (shadowAtlasDebugTarget.isCreated())
            shadowAtlasDebugTarget.destroyFramebuffer();
        return;
    }
    if (!loadShadowRestore())
        return;
    if (!shadowAtlasViewLoaded){
        shadowAtlasViewShader = ShaderPool::get(ShaderType::SHADOW_CACHE, 1);   // 'Vis'
        if (!shadowAtlasViewShader || !shadowAtlasViewShader->isCreated())
            return;
        shadowAtlasViewRender.beginLoad(PrimitiveType::TRIANGLES);
        shadowAtlasViewRender.setShader(shadowAtlasViewShader.get());
        // a pass of the frame: created at once, never skipped (spec S32 §6.4)
        shadowAtlasViewRender.setImmediatePipelines(true);
        if (!shadowAtlasViewRender.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE))
            return;
        shadowAtlasViewLoaded = true;
    }
    ShaderData& viewData = shadowAtlasViewShader->shaderData;
    const int viewParams = viewData.getUniformBlockIndexByName("u_fs_shadowRestore");
    const std::pair<int, int> viewSource = viewData.getTextureIndexByName("u_shadowSource");

    struct Panel { TextureRender* raw; unsigned int width, height; };
    std::vector<Panel> panels;
    if (hasShadowAtlas && shadowAtlasRaw.isCreated())
        panels.push_back({&shadowAtlasRaw, shadowPlan.projective.width, shadowPlan.projective.height});
    if (hasShadowPointAtlas && shadowPointAtlasRaw.isCreated())
        panels.push_back({&shadowPointAtlasRaw, shadowPlan.point.width, shadowPlan.point.height});
    if (shadowStaticAtlas.isCreated() && shadowStaticAtlasRaw.isCreated())
        panels.push_back({&shadowStaticAtlasRaw, shadowPlan.projectiveStatic.width, shadowPlan.projectiveStatic.height});
    if (shadowStaticPointAtlas.isCreated() && shadowStaticPointAtlasRaw.isCreated())
        panels.push_back({&shadowStaticPointAtlasRaw, shadowPlan.pointStatic.width, shadowPlan.pointStatic.height});

    const int panelSize = 256;
    const int width = std::max((int)panels.size(), 1) * panelSize;
    if (!shadowAtlasDebugTarget.isCreated() || shadowAtlasDebugTarget.getColorTexture().getWidth() != width){
        shadowAtlasDebugTarget.destroyFramebuffer();
        JE_GPU_ALLOC_SCOPE(Editor, "Shadow atlas view");
        if (!shadowAtlasDebugTarget.createFramebuffer(TextureType::TEXTURE_2D, width, panelSize,
                TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, false, FramebufferDepth::NONE)){
            return;
        }
    }
    shadowAtlasDebugPassRender.setLabel("shadow atlas view");
    shadowAtlasDebugPassRender.setClearColor(Vector4(0.1f, 0.1f, 0.12f, 1.0f));
    shadowAtlasDebugPassRender.startRenderPass(&shadowAtlasDebugTarget);
    PipelineState state;
    state.target = targetFormatFor(PIP_EFFECT);
    state.target.depth = DepthStencilFormat::NONE;
    state.blend = BlendMode::NONE;
    state.depthCompare = CompareFunc::ALWAYS;
    for (size_t p = 0; p < panels.size(); p++){
        if (!shadowAtlasViewRender.beginDraw(state))
            break;
        // the panel keeps the atlas's aspect
        const float aspect = (panels[p].height > 0) ? (float)panels[p].width / (float)panels[p].height : 1.0f;
        const float w = (aspect >= 1.0f) ? (float)panelSize : (float)panelSize * aspect;
        const float h = (aspect >= 1.0f) ? (float)panelSize / aspect : (float)panelSize;
        shadowAtlasDebugPassRender.applyViewport(Rect((float)(p * panelSize), 0.0f, w, h));
        fs_shadow_restore_t params;
        params.sourceRect = Vector4(0.0f, 0.0f, 1.0f, 1.0f);
        params.params = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        shadowAtlasViewRender.addTexture(viewSource, ShaderStageType::FRAGMENT, panels[p].raw);
        shadowAtlasViewRender.applyUniformBlock(viewParams, sizeof(fs_shadow_restore_t), &params);
        shadowAtlasViewRender.draw(0, 3, 1);
    }
    shadowAtlasDebugPassRender.endRenderPass();
#endif
}

TextureRender* RenderSystem::getShadowAtlasDebugTexture(){
    return shadowAtlasDebugTarget.isCreated() ? &shadowAtlasDebugTarget.getColorTexture() : nullptr;
}
