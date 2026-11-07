// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "subsystem/VoxelSystem.h"

#include "subsystem/VoxelSystemState.h"

#include "subsystem/RenderSystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "io/AtomicFile.h"
#include "io/Data.h"
#include "io/FileUtils.h"
#include "io/ResourcePack.h"
#include "jobs/JobSystem.h"
#include "object/GameObject.h"
#include "object/navigation/NavMeshSurface.h"
#include "voxel/TransvoxelMesher.h"
#include "voxel/VoxelSample.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::voxel;

// ---- settings

VoxelTierSettings jasperengine::voxelTierSettings(VoxelTier tier){
    VoxelTierSettings out;
    switch (tier){
        case VoxelTier::High:
            break;
        case VoxelTier::Medium:
            out.lodLevels = 3;
            out.collisionRadius = 3;
            out.uploadBytesPerFrame = 8 * 1024 * 1024;
            out.uploadMeshesPerFrame = 16;
            out.bodyCap = 256;
            out.bodyBatch = 16;
            out.rescueBodiesPerTick = 32;
            out.maxEditVoxels = 1000 * 1000;
            out.shadowLevels = 2;
            break;
        case VoxelTier::Low:
            out.lodLevels = 3;
            out.collisionRadius = 2;
            out.uploadBytesPerFrame = 2 * 1024 * 1024;
            out.uploadMeshesPerFrame = 6;
            out.bodyCap = 96;
            out.bodyBatch = 4;
            out.rescueBodiesPerTick = 16;
            out.maxEditVoxels = 256 * 1000;
            out.shadowLevels = 1;
            break;
    }
    return out;
}

namespace {
    bool limitsOverridden = false;
    VoxelLimits limitsOverride;
}

VoxelLimits jasperengine::voxelPlatformLimits(){
    VoxelLimits limits;
#if defined(__EMSCRIPTEN__)
    limits.maxDimension = 256;
    limits.cpuBudget = 32 * 1024 * 1024;
    limits.meshBudget = 24 * 1024 * 1024;
    limits.autoTier = VoxelTier::Low;
#elif defined(__ANDROID__) || (defined(__APPLE__) && defined(JE_PLATFORM_IOS))
    limits.maxDimension = 512;
    limits.cpuBudget = 64 * 1024 * 1024;
    limits.meshBudget = 32 * 1024 * 1024;
    limits.autoTier = VoxelTier::Low;
#endif
    return limits;
}

void jasperengine::setVoxelLimitsOverride(const VoxelLimits& limits){
    limitsOverride = limits;
    limitsOverridden = true;
    VoxelAssets::sharedBudget()->setCapacity(limits.cpuBudget);
}

VoxelLimits jasperengine::voxelActiveLimits(){
    return limitsOverridden ? limitsOverride : voxelPlatformLimits();
}

// ---- assets

namespace {

    std::mutex assetMutex;
    std::map<std::string, std::weak_ptr<VoxelAsset>> assetCache;
    VoxelBaseResolver baseResolver;

    // view: the file's bytes, which `bytes` owns, or a pack in memory does (R54: cold bytes not doubled)
    std::shared_ptr<VoxelAsset> loadAsset(const std::string& path, std::shared_ptr<const std::vector<uint8_t>> bytes, std::span<const uint8_t> view){
        auto asset = std::make_shared<VoxelAsset>();
        asset->path = path;
        asset->fileBytes = std::move(bytes);
        const VolumeFileResult info = readVolumeFileInfo(view, voxelActiveLimits().cpuBudget);
        if (!info.ok){
            asset->error = info.error;
            asset->errorMessage = info.message;
            return asset;
        }
        generatorFromJson(info.info.generatorJson, asset->generator);
        asset->slotCount = info.info.slotCount;
        asset->fileFlags = info.info.flags;
        asset->terrainCut = info.info.terrainCut;
        asset->fileHash = volumeFileHash(view);
        asset->store = std::make_shared<Store>(info.info.desc());
        VoxelAssets::resolveBase(*asset);
        const VolumeFileResult loaded = loadVolumeFile(view, *asset->store, asset->fileBytes);
        if (!loaded.ok){
            asset->store.reset();
            asset->error = loaded.error;
            asset->errorMessage = loaded.message;
        }
        return asset;
    }

}

std::shared_ptr<voxel::MemoryBudget> VoxelAssets::sharedBudget(){
    static std::shared_ptr<MemoryBudget> budget = std::make_shared<MemoryBudget>(voxelPlatformLimits().cpuBudget);
    return budget;
}

std::shared_ptr<VoxelAsset> VoxelAssets::acquire(const std::string& path){
    std::lock_guard<std::mutex> lock(assetMutex);
    auto found = assetCache.find(path);
    if (found != assetCache.end()){
        if (std::shared_ptr<VoxelAsset> alive = found->second.lock()) return alive;
    }
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    Data data;
    std::shared_ptr<VoxelAsset> asset;
    std::span<const uint8_t> inPlace;
    if (ResourcePack::view(path, inPlace)){
        asset = loadAsset(path, nullptr, inPlace);
    }else if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
        asset = std::make_shared<VoxelAsset>();
        asset->path = path;
        asset->error = FileError::Truncated;
        asset->errorMessage = "cannot read '" + path + "'";
    }else{
        const unsigned char* begin = data.getMemPtr();
        bytes->assign(begin, begin + data.length());
        const std::span<const uint8_t> view(*bytes);
        asset = loadAsset(path, std::move(bytes), view);
    }
    assetCache[path] = asset;
    return asset;
}

std::shared_ptr<VoxelAsset> VoxelAssets::fromBytes(const std::string& path, std::shared_ptr<const std::vector<uint8_t>> bytes){
    std::lock_guard<std::mutex> lock(assetMutex);
    const std::span<const uint8_t> view(*bytes);
    std::shared_ptr<VoxelAsset> asset = loadAsset(path, std::move(bytes), view);
    assetCache[path] = asset;
    return asset;
}

std::shared_ptr<VoxelAsset> VoxelAssets::create(const std::string& path, const voxel::VolumeDesc& desc, const voxel::GeneratorDesc& generator, int slotCount){
    auto asset = std::make_shared<VoxelAsset>();
    asset->path = path;
    asset->generator = generator;
    asset->slotCount = slotCount;
    asset->dirty = true;
    asset->store = std::make_shared<Store>(desc);
    resolveBase(*asset);
    std::lock_guard<std::mutex> lock(assetMutex);
    assetCache[path] = asset;
    return asset;
}

void VoxelAssets::forget(const std::string& path){
    std::lock_guard<std::mutex> lock(assetMutex);
    assetCache.erase(path);
}

void VoxelAssets::setBaseResolver(VoxelBaseResolver resolver){
    baseResolver = std::move(resolver);
}

void VoxelAssets::resolveBase(VoxelAsset& asset){
    if (!asset.store) return;
    const GeneratorDesc& generator = asset.generator;
    std::shared_ptr<const BaseSource> base;
    if (generator.kind == "empty"){
        base = makeEmptyBase();
    }else if (generator.kind == "solid_ground"){
        base = makeSolidGroundBase(generator.height, asset.store->desc().voxelSize, 0);
    }else if (baseResolver){
        base = baseResolver(generator, asset.store->desc());
    }
    asset.baseResolved = base != nullptr;
    if (base) asset.store->setBase(std::move(base));
}

// ---- the system

VoxelSystem::VoxelSystem(Scene* scene): SubSystem(scene){
    declareHookType(scene->getComponentId<VoxelVolumeComponent>());
}

VoxelSystem::~VoxelSystem() = default;

void VoxelSystem::load(){}
void VoxelSystem::draw(){}

void VoxelSystem::destroy(){
    while (!states.empty()) removeState(states.begin()->first);
    queue.reset();
    jobs.clear();
}

void VoxelSystem::setPaused(bool value){
    if (paused == value) return;
    paused = value;
    // Play gets a copy of the tables (R6): a shared store is replaced by its clone, or back
    for (auto& item : states) item.second->store.reset();
}

Entity VoxelSystem::getGeneratedOwner(Entity entity) const{
    const auto found = generatedOwners.find(entity);
    return found == generatedOwners.end() ? NULL_ENTITY : found->second;
}

std::vector<Entity> VoxelSystem::getGeneratedEntities(Entity owner) const{
    std::vector<Entity> out;
    for (const auto& item : generatedOwners){
        if (item.second == owner) out.push_back(item.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

AABB VoxelSystem::getGeneratedBounds(Entity owner) const{
    const VolumeState* state = stateOf(owner);
    if (!state || !state->store) return AABB();
    const VolumeDesc& desc = state->store->desc();
    const AABB local(0.0f, 0.0f, 0.0f, desc.dimensions.x * desc.voxelSize, desc.dimensions.y * desc.voxelSize, desc.dimensions.z * desc.voxelSize);
    return state->model * local;
}

VoxelSystem::VolumeState* VoxelSystem::stateOf(Entity owner){
    const auto found = states.find(owner);
    return found == states.end() ? nullptr : found->second.get();
}

const VoxelSystem::VolumeState* VoxelSystem::stateOf(Entity owner) const{
    const auto found = states.find(owner);
    return found == states.end() ? nullptr : found->second.get();
}

bool VoxelSystem::isVolumeReady(Entity owner) const{
    const VolumeState* state = stateOf(owner);
    return state && state->firstSetActive;
}

void VoxelSystem::forEachLoading(const std::function<void(Entity)>& visit) const{
    auto volumes = scene->getComponentArray<VoxelVolumeComponent>();
    for (size_t i = 0; i < volumes->size(); i++){
        const Entity owner = volumes->getEntity(i);
        const VoxelVolumeComponent& component = volumes->getComponentFromIndex(i);
        if (!component.enabled || component.volumeData.path.empty()) continue;
        const VolumeState* state = stateOf(owner);
        if (state && !state->error.empty()) continue;
        if (!state || !state->firstSetActive) visit(owner);
    }
}

Store* VoxelSystem::getStore(Entity owner){
    VolumeState* state = stateOf(owner);
    return state ? state->store.get() : nullptr;
}

std::shared_ptr<VoxelAsset> VoxelSystem::getAsset(Entity owner) const{
    const VolumeState* state = stateOf(owner);
    return state ? state->asset : nullptr;
}

std::string VoxelSystem::getError(Entity owner) const{
    const VolumeState* state = stateOf(owner);
    return state ? state->error : std::string();
}

bool VoxelSystem::worldToLocal(Entity owner, const Vector3& world, Vec3d& local) const{
    const VolumeState* state = stateOf(owner);
    if (!state) return false;
    local = toVec3d(state->inverseModel * world);
    return true;
}

Vector3 VoxelSystem::localToWorld(Entity owner, const Vec3d& local) const{
    const VolumeState* state = stateOf(owner);
    if (!state) return Vector3();
    return state->model * Vector3(static_cast<float>(local.x), static_cast<float>(local.y), static_cast<float>(local.z));
}

Quatd VoxelSystem::localRotationOf(Entity owner, const Quaternion& worldRotation) const{
    const VolumeState* state = stateOf(owner);
    if (!state) return Quatd{};
    const Quaternion r = state->worldRotation.inverse() * worldRotation;
    return Quatd{r.w, r.x, r.y, r.z};
}

Matrix4 VoxelSystem::modelMatrixOf(Entity owner) const{
    const VolumeState* state = stateOf(owner);
    return state ? state->model : Matrix4();
}

VoxelTier VoxelSystem::resolveTier(const VoxelVolumeComponent& component) const{
    switch (component.quality){
        case VoxelQuality::High: return VoxelTier::High;
        case VoxelQuality::Medium: return VoxelTier::Medium;
        case VoxelQuality::Low: return VoxelTier::Low;
        case VoxelQuality::Auto: break;
    }
    return voxelActiveLimits().autoTier;
}

void VoxelSystem::syncVolumes(){
    auto volumes = scene->getComponentArray<VoxelVolumeComponent>();
    // Whatever lost its component, or was disabled, goes with its chunks and bodies (R19)
    std::vector<Entity> gone;
    for (const auto& item : states){
        const VoxelVolumeComponent* component = scene->isEntityCreated(item.first) ? scene->findComponent<VoxelVolumeComponent>(item.first) : nullptr;
        const Transform* transform = component ? scene->findComponent<Transform>(item.first) : nullptr;
        if (!component || !component->enabled || (transform && !transform->visible)) gone.push_back(item.first);
    }
    for (Entity owner : gone) removeState(owner);
    for (size_t i = 0; i < volumes->size(); i++){
        const Entity owner = volumes->getEntity(i);
        VoxelVolumeComponent& component = volumes->getComponentFromIndex(i);
        const Transform* transform = scene->findComponent<Transform>(owner);
        if (!component.enabled || (transform && !transform->visible)) continue;
        auto& slot = states[owner];
        if (!slot){
            slot = std::make_unique<VolumeState>();
            slot->owner = owner;
        }
    }
}

void VoxelSystem::removeState(Entity owner){
    auto found = states.find(owner);
    if (found == states.end()) return;
    VolumeState& state = *found->second;
    for (auto it = jobs.begin(); it != jobs.end();){
        if (it->second->owner == owner){
            if (queue) queue->cancelAndWait(it->first);
            it = jobs.erase(it);
        }else{
            ++it;
        }
    }
    if (VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner)){
        detachTerrain(state, *component);
    }
    destroyCollision(state);
    for (const auto& chunk : state.chunks){
        generatedOwners.erase(chunk.second);
        if (scene->isEntityCreated(chunk.second)) scene->destroyEntity(chunk.second);
    }
    for (Entity chunk : state.chunkPool){
        generatedOwners.erase(chunk);
        if (scene->isEntityCreated(chunk)) scene->destroyEntity(chunk);
    }
    states.erase(found);
}

bool VoxelSystem::ensureLoaded(Entity owner, VoxelVolumeComponent& component, VolumeState& state){
    const std::string& path = component.volumeData.path;
    if (state.assetPath != path || (!state.asset && !path.empty())){
        // Another file, or the first look: everything built on the old one goes
        if (!state.assetPath.empty() && state.assetPath != path){
            const Entity keep = owner;
            removeState(owner);
            auto& fresh = states[keep];
            fresh = std::make_unique<VolumeState>();
            fresh->owner = keep;
            return false;
        }
        state.assetPath = path;
        state.asset = path.empty() ? nullptr : VoxelAssets::acquire(path);
        state.store.reset();
        state.error.clear();
        state.loggedError = false;
    }
    if (!state.asset) return false;
    if (state.asset->error != FileError::None || !state.asset->store){
        state.error = state.asset->errorMessage.empty() ? std::string("the volume did not load") : state.asset->errorMessage;
        if (!state.loggedError){
            state.loggedError = true;
            Log::error("Voxel Volume '%s': %s", scene->getEntityName(owner).c_str(), state.error.c_str());
        }
        return false;
    }
    if (!state.store){
        // edit mode edits the asset; Play and players work on a copy (R6)
        if (paused){
            state.store = state.asset->store;
            state.ownsStore = false;
        }else{
            state.store = std::shared_ptr<Store>(state.asset->store->clone());
            state.ownsStore = true;
        }
        if (!state.store->budget()) state.store->setBudget(VoxelAssets::sharedBudget());
        state.firstSetRequested = false;
    }
    return true;
}

Vector3 VoxelSystem::focusPositionWorld(const VoxelVolumeComponent& component) const{
    Entity focus = component.focus;
    if (focus == NULL_ENTITY || !scene->isEntityCreated(focus)) focus = scene->getCamera();
    if (focus != NULL_ENTITY && scene->isEntityCreated(focus)){
        if (const Transform* transform = scene->findComponent<Transform>(focus)) return transform->worldPosition;
    }
    return Vector3();
}

Vec3d VoxelSystem::worldVoxels(const VolumeState& state, const Vector3& world) const{
    const Vector3 local = state.inverseModel * world;
    const double voxelSize = state.store->desc().voxelSize;
    return Vec3d{local.x / voxelSize, local.y / voxelSize, local.z / voxelSize};
}

void VoxelSystem::updateFocus(VolumeState& state, VoxelVolumeComponent& component){
    const VoxelTier tier = resolveTier(component);
    const VoxelTierSettings settings = voxelTierSettings(tier);
    const int wantedLevels = component.lodLevels > 0 ? std::clamp(component.lodLevels, 1, MAX_LEVELS) : settings.lodLevels;
    const int levels = std::min(wantedLevels, state.store->levels());
    const int radius = (component.lodRadius == 6 || component.lodRadius == 8) ? component.lodRadius : 4;
    bool reconfigure = !state.firstSetRequested || levels != state.levels || radius != state.lodRadius || tier != state.tier;
    if (reconfigure){
        // A tier is a shader variant and a budget: chunks already up keep drawing until the new set lands
        state.tier = tier;
        state.levels = levels;
        state.lodRadius = radius;
        state.clipbox.configure(state.store->desc().dimensions, levels, radius);
    }
    state.clipbox.setMaxDrawDistance(component.maxDrawDistance > 0.0f ? component.maxDrawDistance / state.store->desc().voxelSize : 0.0);
    const Vec3d focus = worldVoxels(state, focusPositionWorld(component));
    const bool moved = state.clipbox.update(focus.x, focus.y, focus.z);
    const bool refresh = state.scheduler.wantsLodRefresh() && state.scheduler.inFlight() == 0;
    if (moved || reconfigure || refresh){
        state.scheduler.clearLodRefresh();
        std::vector<VisibleBlock> visible;
        state.clipbox.visibleBlocks(visible);
        state.scheduler.setDesiredLod(visible);
        state.firstSetRequested = true;
        flushRequests(state);
    }
}

void VoxelSystem::flushRequests(VolumeState& state){
    if (!queue) queue = std::make_unique<WorkQueue>();
    // A set has one data version (R17): a job's view is taken with its request, never later
    std::vector<MeshId> cancelled;
    state.scheduler.takeCancelled(cancelled);
    for (const MeshId& id : cancelled){
        const auto found = state.pendingJobs.find(id);
        if (found == state.pendingJobs.end()) continue;
        if (queue->cancel(found->second)) jobs.erase(found->second);
        state.pendingJobs.erase(found);
    }
    std::vector<MeshBuildRequest> requests;
    state.scheduler.takeRequests(requests);
    const Vec3d focus = worldVoxels(state, focusPositionWorld(*scene->findComponent<VoxelVolumeComponent>(state.owner)));
    for (const MeshBuildRequest& request : requests){
        const uint64_t jobId = nextJobId++;
        auto job = std::make_unique<Job>();
        job->kind = Job::Mesh;
        job->owner = state.owner;
        job->id = request.id;
        job->view = state.store->readView(request.id.key, request.id.sideMask);
        job->mesh = std::make_shared<MeshOut>();
        job->coverage = state.coverage;
        Job* raw = job.get();
        jobs[jobId] = std::move(job);
        state.pendingJobs[request.id] = jobId;

        const double span = static_cast<double>(BLOCK_SIZE << request.id.key.level);
        const double cx = (request.id.key.x + 0.5) * span, cy = (request.id.key.y + 0.5) * span, cz = (request.id.key.z + 0.5) * span;
        const double distance = std::sqrt((cx - focus.x) * (cx - focus.x) + (cy - focus.y) * (cy - focus.y) + (cz - focus.z) * (cz - focus.z));
        const WorkPriority priority = request.editPriority ? WorkPriority::Edit : (request.id.key.level == 0 ? WorkPriority::Near : WorkPriority::Far);
        queue->push(jobId, priority, distance, [raw]{
            const auto start = std::chrono::steady_clock::now();
            MeshRequest mesh;
            mesh.key = raw->id.key;
            mesh.sideMask = raw->id.sideMask;
            mesh.mode = MeshMode::Full;
            mesh.coverage = raw->coverage.get();
            meshBlock(*raw->view, mesh, *raw->mesh);
            raw->buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        });
    }
}

void VoxelSystem::pumpJobs(){
    if (!queue) return;
    queue->pump();
    std::vector<uint64_t> finished, dropped;
    queue->takeFinished(finished, dropped);
    for (uint64_t id : finished){
        const auto found = jobs.find(id);
        if (found == jobs.end()) continue;
        std::unique_ptr<Job> job = std::move(found->second);
        jobs.erase(found);
        VolumeState* state = stateOf(job->owner);
        if (!state) continue;
        if (job->kind == Job::Mesh){
            state->pendingJobs.erase(job->id);
            if (state->store) state->store->publish(job->view->takeResolved());
            state->lastMeshMs = job->buildMs;
            state->scheduler.meshBuilt(job->id, job->mesh);
        }else{
            onCollisionJobDone(*state, *job);
        }
    }
    for (uint64_t id : dropped){
        const auto found = jobs.find(id);
        if (found == jobs.end()) continue;
        std::unique_ptr<Job> job = std::move(found->second);
        jobs.erase(found);
        VolumeState* state = stateOf(job->owner);
        if (!state || job->kind != Job::Mesh) continue;
        state->pendingJobs.erase(job->id);
        state->scheduler.meshFailed(job->id);
        flushRequests(*state);
    }
}

Entity VoxelSystem::acquireChunk(VolumeState& state, const VoxelVolumeComponent&){
    // a pooled chunk something else destroyed (a script's destroy on a raycast hit) is not reused
    while (!state.chunkPool.empty()){
        const Entity chunk = state.chunkPool.back();
        state.chunkPool.pop_back();
        if (scene->isEntityCreated(chunk) && scene->findComponent<Transform>(chunk) && scene->findComponent<MeshComponent>(chunk)) return chunk;
        generatedOwners.erase(chunk);
    }
    const Entity chunk = scene->createEntity();
    scene->addComponent<Transform>(chunk);
    scene->addComponent<MeshComponent>(chunk);
    scene->setEntityLayer(chunk, scene->getEntityLayer(state.owner));
    generatedOwners[chunk] = state.owner;
    applyOwnerTransform(state, chunk);
    return chunk;
}

void VoxelSystem::releaseChunk(VolumeState& state, Entity chunk){
    Transform* transform = scene->isEntityCreated(chunk) ? scene->findComponent<Transform>(chunk) : nullptr;
    if (!transform){
        generatedOwners.erase(chunk);
        return;
    }
    transform->visible = false;
    state.chunkPool.push_back(chunk);
}

void VoxelSystem::applyOwnerTransform(VolumeState& state, Entity chunk){
    Transform* transform = scene->isEntityCreated(chunk) ? scene->findComponent<Transform>(chunk) : nullptr;
    if (!transform) return;
    transform->parent = NULL_ENTITY;
    transform->position = state.worldPosition;
    transform->rotation = state.worldRotation;
    transform->scale = Vector3(1.0f, 1.0f, 1.0f);
    transform->worldPosition = state.worldPosition;
    transform->worldRotation = state.worldRotation;
    transform->modelMatrix = state.model;
    transform->needUpdate = true;
}

void VoxelSystem::writeChunk(VolumeState& state, const VoxelVolumeComponent& component, Entity chunk, const MeshId& id, const MeshOut& mesh){
    MeshComponent& target = scene->getComponent<MeshComponent>(chunk);
    Transform& transform = scene->getComponent<Transform>(chunk);
    transform.visible = false;
    const float voxelSize = state.store->desc().voxelSize;

    target.voxelOwner = state.owner;
    target.voxelTier = static_cast<uint8_t>(state.tier);
    target.numSubmeshes = 1;
    target.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    target.submeshes[0].vertexCount = static_cast<unsigned int>(mesh.indices.size());
    target.castShadows = component.castShadows && id.key.level < voxelTierSettings(state.tier).shadowLevels;
    // The attached volume loses a depth tie to its terrain (R41)
    const bool attached = component.terrain != NULL_ENTITY;
    target.submeshes[0].material.depthOffsetUnits = attached ? 2.0f : 0.0f;
    target.submeshes[0].material.depthOffsetFactor = attached ? 1.0f : 0.0f;

    target.buffer.clearAll();
    // Fixed point becomes the float the GPU reads through the one function (V5, R11)
    const size_t bytes = mesh.vertices.size() * sizeof(MeshVertex);
    target.buffer.Buffer::addAttribute(AttributeType::POSITION, AttributeDataType::FLOAT, 3, offsetof(MeshVertex, position));
    target.buffer.Buffer::addAttribute(AttributeType::NORMAL, AttributeDataType::FLOAT, 3, offsetof(MeshVertex, normal));
    target.buffer.Buffer::addAttribute(AttributeType::VOXELSLOTS, AttributeDataType::UNSIGNED_SHORT, 2, offsetof(MeshVertex, slots));
    target.buffer.Buffer::addAttribute(AttributeType::VOXELWEIGHTS, AttributeDataType::UNSIGNED_BYTE, 4, offsetof(MeshVertex, weights));
    target.buffer.getAttribute(AttributeType::VOXELSLOTS)->setNormalized(true);
    target.buffer.getAttribute(AttributeType::VOXELWEIGHTS)->setNormalized(true);
    target.buffer.setStride(sizeof(MeshVertex));
    target.buffer.setVertexSize(sizeof(MeshVertex));
    target.buffer.setCount(static_cast<unsigned int>(mesh.vertices.size()));
    target.buffer.importData(mesh.vertices.data(), bytes);
    auto* written = reinterpret_cast<MeshVertex*>(target.buffer.getData());
    AABB local;
    for (size_t i = 0; i < mesh.vertices.size(); i++){
        const MeshVertex& source = mesh.vertices[i];
        const float position[3] = {fixedToLocal(source.position[0], voxelSize), fixedToLocal(source.position[1], voxelSize), fixedToLocal(source.position[2], voxelSize)};
        std::memcpy(written[i].position, position, sizeof(position));
    }
    local.setMinimum(fixedToLocal(mesh.boundsMin[0], voxelSize), fixedToLocal(mesh.boundsMin[1], voxelSize), fixedToLocal(mesh.boundsMin[2], voxelSize));
    local.setMaximum(fixedToLocal(mesh.boundsMax[0], voxelSize), fixedToLocal(mesh.boundsMax[1], voxelSize), fixedToLocal(mesh.boundsMax[2], voxelSize));

    target.indices.clear();
    target.indices.setValues(0, target.indices.getAttribute(AttributeType::INDEX), static_cast<unsigned int>(mesh.indices.size()),
                             reinterpret_cast<char*>(const_cast<uint16_t*>(mesh.indices.data())), sizeof(uint16_t));
    target.verticesAABB = local;
    target.aabb = local;
    target.vertexCount = static_cast<unsigned int>(mesh.vertices.size());
    target.needUpdateAABB = true;
    if (target.loaded) target.needReload = true;
}

void VoxelSystem::uploadChunks(VolumeState& state, const VoxelVolumeComponent& component){
    std::vector<std::pair<MeshId, std::shared_ptr<const MeshOut>>> uploads;
    state.scheduler.takeUploads(uploads);
    for (auto& item : uploads) state.uploadQueue.push_back(std::move(item));

    const VoxelTierSettings settings = voxelTierSettings(state.tier);
    state.uploadBytesThisFrame = 0;
    state.uploadMeshesThisFrame = 0;
    while (!state.uploadQueue.empty()){
        const size_t size = state.uploadQueue.front().second->byteSize();
        const bool first = state.uploadMeshesThisFrame == 0;
        // R18: one mesh always goes, however big
        if (!first && (state.uploadMeshesThisFrame >= settings.uploadMeshesPerFrame
                       || state.uploadBytesThisFrame + static_cast<int64_t>(size) > settings.uploadBytesPerFrame)) break;
        const auto item = std::move(state.uploadQueue.front());
        state.uploadQueue.pop_front();
        const Entity chunk = acquireChunk(state, component);
        writeChunk(state, component, chunk, item.first, *item.second);
        state.chunks[item.first] = chunk;
        state.uploading.push_back(item.first);
        state.uploadBytesThisFrame += static_cast<int64_t>(size);
        state.uploadMeshesThisFrame++;
    }
    // A hidden chunk loads like any mesh; with no view, or no RenderSystem running in this scene (a
    // server, a test), nothing would ever load it
    bool renderRuns = false;
    for (const auto& system : scene->getScheduler().getSystems()){
        if (dynamic_cast<RenderSystem*>(system->getOwner()) && system->isEnabled()) renderRuns = true;
    }
    const bool viewLoaded = Engine::isViewLoaded() && renderRuns;
    for (size_t i = 0; i < state.uploading.size();){
        const MeshId& id = state.uploading[i];
        const auto found = state.chunks.find(id);
        bool done = found == state.chunks.end() || !viewLoaded;
        if (!done){
            const MeshComponent* mesh = scene->isEntityCreated(found->second) ? scene->findComponent<MeshComponent>(found->second) : nullptr;
            done = !mesh || (mesh->loaded && !mesh->needReload);
        }
        if (!done){
            i++;
            continue;
        }
        state.scheduler.meshUploaded(id);
        state.uploading.erase(state.uploading.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void VoxelSystem::pollActivations(VolumeState& state){
    ChangeSetScheduler::Activation activation;
    while (state.scheduler.pollActivation(activation)){
        for (const MeshId& id : activation.hide){
            const auto chunk = state.chunks.find(id);
            if (chunk != state.chunks.end()){
                releaseChunk(state, chunk->second);
                state.chunks.erase(chunk);
            }
            const auto screen = state.onScreen.find(id);
            if (screen != state.onScreen.end()){
                state.meshBytes -= static_cast<int64_t>(screen->second->byteSize());
                state.triangles -= screen->second->triangleCount;
                state.onScreen.erase(screen);
            }
            state.scheduler.meshReleased(id);
        }
        for (const auto& shown : activation.show){
            if (!shown.second || shown.second->empty()) continue;
            const auto chunk = state.chunks.find(shown.first);
            Transform* transform = chunk != state.chunks.end() && scene->isEntityCreated(chunk->second) ? scene->findComponent<Transform>(chunk->second) : nullptr;
            if (transform) transform->visible = true;
            state.onScreen[shown.first] = shown.second;
            state.meshBytes += static_cast<int64_t>(shown.second->byteSize());
            state.triangles += shown.second->triangleCount;
        }
        state.firstSetActive = true;
    }
}

void VoxelSystem::updateVolume(VolumeState& state, VoxelVolumeComponent& component, double dt){
    if (!ensureLoaded(state.owner, component, state)) return;
    Transform* transform = scene->findComponent<Transform>(state.owner);
    if (transform){
        const bool moved = std::memcmp(&transform->modelMatrix, &state.model, sizeof(Matrix4)) != 0;
        if (moved){
            // R1: static in Play. A move logs once and does nothing, in edit mode the chunks follow.
            if (!paused && state.firstSetRequested){
                if (!state.warnedMoved){
                    state.warnedMoved = true;
                    Log::warn("Voxel Volume '%s' is static in Play: moving its Transform does nothing", scene->getEntityName(state.owner).c_str());
                }
            }else{
                state.model = transform->modelMatrix;
                state.inverseModel = state.model.inverse();
                state.worldPosition = transform->worldPosition;
                state.worldRotation = transform->worldRotation;
                for (const auto& chunk : state.chunks) applyOwnerTransform(state, chunk.second);
                for (const Entity chunk : state.chunkPool) applyOwnerTransform(state, chunk);
            }
        }
    }
    attachTerrain(state, component);
    updateFocus(state, component);
    uploadChunks(state, component);
    pollActivations(state);
    fireEvents(state, component);
    updateNavigation(state, component, dt);
}

void VoxelSystem::update(double dt){
    auto volumes = scene->getComponentArray<VoxelVolumeComponent>();
    if (volumes->size() == 0 && states.empty()) return;
    frameCounter++;
    syncVolumes();
    pumpJobs();
    // ensureLoaded may replace a state, so the loop is over a copy of the owners
    for (Entity owner : stateOwners()){
        VolumeState* state = stateOf(owner);
        VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
        if (state && component) updateVolume(*state, *component, dt);
    }
}

std::vector<Entity> VoxelSystem::stateOwners() const{
    std::vector<Entity> owners;
    owners.reserve(states.size());
    for (const auto& item : states) owners.push_back(item.first);
    return owners;
}

void VoxelSystem::fireEvents(VolumeState& state, VoxelVolumeComponent& component){
    // In the frame of the edit, in order; an edit inside a callback gets its event next frame (R31)
    if (!state.events.empty()){
        std::vector<VoxelEditEvent> events;
        events.swap(state.events);
        for (VoxelEditEvent& event : events) component.onEdited.call(event);
    }
    if (!state.collisionEvents.empty()){
        std::vector<AABB> events;
        events.swap(state.collisionEvents);
        for (const AABB& bounds : events) component.onCollisionUpdated.call(bounds);
    }
    if (!state.rescuedEvents.empty()){
        std::vector<Entity> events;
        events.swap(state.rescuedEvents);
        for (Entity entity : events) component.onBodyRescued.call(GameObject(scene, entity));
    }
}

void VoxelSystem::updateNavigation(VolumeState& state, VoxelVolumeComponent& component, double dt){
    if (!state.navDirty) return;
    state.navSinceFirst += dt;
    state.navSinceLast += dt;
    // R25: in edit mode an edit only marks tiles dirty, as moving a collider does; no automatic bake
    if (paused || component.navigationUpdate == VoxelNavigationUpdate::Off){
        state.navDirty = false;
        return;
    }
    if (state.navSinceLast < 1.0 && state.navSinceFirst < 5.0) return;
    auto surfaces = scene->getComponentArray<NavMeshSurfaceComponent>();
    bool blocked = false;
    for (size_t i = 0; i < surfaces->size(); i++){
        NavMeshSurface surface(scene, surfaces->getEntity(i));
        if (!surface.isValid()) continue;
        // at most one in flight per surface (R25)
        if (surface.isBaking()){
            blocked = true;
            continue;
        }
        surface.updateTiles(state.navBounds);
    }
    if (!blocked) state.navDirty = false;
}

EditResult VoxelSystem::edit(Entity owner, std::span<const EditOp> ops, EditOptions options){
    EditResult result;
    VolumeState* state = stateOf(owner);
    VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
    if (state && component && !state->store) ensureLoaded(owner, *component, *state);
    if (!state || !component || !state->store){
        result.ok = false;
        result.error = EditError::NotLoaded;
        return result;
    }
    if (!component->materialSlots.empty()) options.slotCount = std::min<int>(static_cast<int>(component->materialSlots.size()), MAX_SLOTS);
    else if (state->asset && state->asset->slotCount > 0) options.slotCount = state->asset->slotCount;
    if (options.debrisCount == 0) options.debrisCount = component->debrisPointCount;
    options.debrisSeed = editIndex;
    if (options.maxVoxels == 0 && !paused) options.maxVoxels = voxelTierSettings(state->tier).maxEditVoxels;
    result = voxel::applyEdit(*state->store, ops, options);
    if (!result.ok || options.dryRun) return result;
    editIndex++;
    if (result.changedVoxels.empty()) return result;
    if (state->asset && !state->ownsStore) state->asset->dirty = true;

    // The event of R31, in world space
    const float voxelSize = state->store->desc().voxelSize;
    VoxelEditEvent event;
    const AABB localBounds(result.changedVoxels.min.x * voxelSize, result.changedVoxels.min.y * voxelSize, result.changedVoxels.min.z * voxelSize,
                           result.changedVoxels.max.x * voxelSize, result.changedVoxels.max.y * voxelSize, result.changedVoxels.max.z * voxelSize);
    event.bounds = state->model * localBounds;
    event.center = (event.bounds.getMinimum() + event.bounds.getMaximum()) * 0.5f;
    event.removedVolume = static_cast<float>(result.removedVolume);
    event.addedVolume = static_cast<float>(result.addedVolume);
    event.removedByMaterial.assign(result.removedByMaterial.begin(), result.removedByMaterial.end());
    for (const Vec3d& point : result.debrisPoints) event.debrisPoints.push_back(localToWorld(owner, point));
    state->events.push_back(std::move(event));
    state->latencyStartFrame = frameCounter;

    notifyChanged(state->store.get(), result.changedVoxels);
    result.holesCut = state->lastHolesCut;
    result.holesFilled = state->lastHolesFilled;
    result.entranceNarrow = state->lastEntranceNarrow;
    return result;
}

void VoxelSystem::notifyChanged(Store* store, const IntBox& changed){
    if (changed.empty()) return;
    for (auto& item : states){
        VolumeState& state = *item.second;
        if (state.store.get() != store) continue;
        // The cut decides what the mesher may drop, so it comes first; the ring around a change remeshes too (R41)
        VoxelVolumeComponent* attached = scene->findComponent<VoxelVolumeComponent>(item.first);
        if (attached) updateTerrainCut(state, *attached, changed);
        IntBox dirty = changed;
        if (state.coverageRing > 0){
            const int ring = state.coverageRing + 1;
            dirty.min = Int3{changed.min.x - ring, changed.min.y, changed.min.z - ring};
            dirty.max = Int3{changed.max.x + ring, changed.max.y, changed.max.z + ring};
        }
        const uint64_t setId = state.scheduler.onEdit(dirty, store->levels());
        if (!state.pendingOpens.empty()) state.pendingOpenSet = setId;
        flushRequests(state);
        onCollisionEdited(state, dirty, setId);
        const float voxelSize = store->desc().voxelSize;
        const AABB local(changed.min.x * voxelSize, changed.min.y * voxelSize, changed.min.z * voxelSize,
                         changed.max.x * voxelSize, changed.max.y * voxelSize, changed.max.z * voxelSize);
        const AABB world = state.model * local;
        if (!state.navDirty){
            state.navBounds = world;
            state.navSinceFirst = 0.0;
        }else{
            state.navBounds.merge(world);
        }
        state.navDirty = true;
        state.navSinceLast = 0.0;
    }
}

void VoxelSystem::applyDelta(Entity owner, const std::vector<SlotChange>& delta, bool undo){
    VolumeState* state = stateOf(owner);
    if (!state || !state->store) return;
    voxel::applyDelta(*state->store, delta, undo);
    IntBox changed;
    for (const SlotChange& change : delta){
        const int span = BLOCK_SIZE << change.key.level;
        changed.include(Int3{change.key.x * span, change.key.y * span, change.key.z * span});
        changed.include(Int3{(change.key.x + 1) * span - 1, (change.key.y + 1) * span - 1, (change.key.z + 1) * span - 1});
    }
    notifyChanged(state->store.get(), changed);
}

void VoxelSystem::reloadVolume(Entity owner){
    VolumeState* state = stateOf(owner);
    if (!state) return;
    state->assetPath.clear();
    state->asset.reset();
    state->store.reset();
    removeState(owner);
}

// ---- runtime saves (R79)

namespace {
    std::string savePathOf(uint64_t fileHash, const std::string& slot){
        // data://voxels/<asset>/<slot>.voxelvolume: the asset's identity without its path's separators
        char name[32];
        snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(fileHash));
        return "data://voxels/" + std::string(name) + "/" + slot + ".voxelvolume";
    }
}

bool VoxelSystem::saveChanges(Entity owner, const std::string& slot, std::string& error){
    VolumeState* state = stateOf(owner);
    if (!state || !state->store || !state->asset){
        error = "not_loaded";
        return false;
    }
    SaveOptions options;
    options.mode = SaveOptions::Mode::Delta;
    options.baseHash = state->asset->fileHash;
    if (state->cutWidth > 0){
        options.terrainCut = TerrainCutRuns::fromMask(state->cutMask.data(), state->cutWidth, state->cutHeight);
    }
    const std::vector<uint8_t> bytes = saveVolumeFile(*state->store, options);
    std::string normalized, systemPath;
    const std::string path = savePathOf(state->asset->fileHash, slot);
    if (!FileUtils::resolve(path, true, normalized, systemPath, error)) return false;
    const io::WriteResult written = io::writeFileAtomic(std::filesystem::path(systemPath), std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!written.ok()){
        error = io::describeWriteError(written);
        return false;
    }
    return true;
}

bool VoxelSystem::hasSavedChanges(Entity owner, const std::string& slot) const{
    const VolumeState* state = stateOf(owner);
    if (!state || !state->asset) return false;
    return FileUtils::exists(savePathOf(state->asset->fileHash, slot));
}

bool VoxelSystem::loadChanges(Entity owner, const std::string& slot, std::string& error){
    VolumeState* state = stateOf(owner);
    if (!state || !state->store || !state->asset){
        error = "not_loaded";
        return false;
    }
    std::string normalized, systemPath;
    const std::string path = savePathOf(state->asset->fileHash, slot);
    if (!FileUtils::resolve(path, false, normalized, systemPath, error)) return false;
    Data data;
    if (data.open(systemPath.c_str()) != FileErrors::FILEDATA_OK){
        error = "not found: " + path;
        return false;
    }
    const std::span<const uint8_t> bytes(data.getMemPtr(), data.length());
    // All or nothing over the loaded state, so an earlier Play edit does not survive under it
    auto fresh = std::shared_ptr<Store>(state->asset->store->clone());
    const VolumeFileResult loaded = loadDeltaFile(bytes, *fresh, state->asset->fileHash);
    if (!loaded.ok){
        error = fileErrorCode(loaded.error);
        return false;
    }
    fresh->setBudget(VoxelAssets::sharedBudget());
    const std::vector<BlockKey> changed = fresh->editedBlocks();
    state->store = fresh;
    state->ownsStore = true;
    IntBox box;
    box.include(Int3{0, 0, 0});
    box.include(Int3{fresh->desc().dimensions.x - 1, fresh->desc().dimensions.y - 1, fresh->desc().dimensions.z - 1});
    notifyChanged(state->store.get(), box);
    return true;
}

void VoxelSystem::revertChanges(Entity owner){
    VolumeState* state = stateOf(owner);
    if (!state || !state->asset || !state->asset->store) return;
    // R30: no Play edit is undoable, and this returns to the loaded state
    state->store = std::shared_ptr<Store>(state->asset->store->clone());
    state->ownsStore = true;
    state->store->setBudget(VoxelAssets::sharedBudget());
    IntBox box;
    box.include(Int3{0, 0, 0});
    box.include(Int3{state->store->desc().dimensions.x - 1, state->store->desc().dimensions.y - 1, state->store->desc().dimensions.z - 1});
    notifyChanged(state->store.get(), box);
}

// ---- navigation (R25)

void VoxelSystem::appendNavTriangles(const AABB& worldBounds, std::vector<Vector3>& vertices, std::vector<uint32_t>& indices){
    auto volumes = scene->getComponentArray<VoxelVolumeComponent>();
    for (size_t i = 0; i < volumes->size(); i++){
        const Entity owner = volumes->getEntity(i);
        VoxelVolumeComponent& component = volumes->getComponentFromIndex(i);
        VolumeState* state = stateOf(owner);
        if (!component.enabled || !state || !state->store) continue;
        const float voxelSize = state->store->desc().voxelSize;
        const AABB local = state->inverseModel * worldBounds;
        const Int3 count = state->store->blockCount(0);
        const int x0 = std::max(0, static_cast<int>(std::floor(local.getMinimum().x / voxelSize / BLOCK_SIZE)) - 1);
        const int y0 = std::max(0, static_cast<int>(std::floor(local.getMinimum().y / voxelSize / BLOCK_SIZE)) - 1);
        const int z0 = std::max(0, static_cast<int>(std::floor(local.getMinimum().z / voxelSize / BLOCK_SIZE)) - 1);
        const int x1 = std::min(count.x - 1, static_cast<int>(std::floor(local.getMaximum().x / voxelSize / BLOCK_SIZE)) + 1);
        const int y1 = std::min(count.y - 1, static_cast<int>(std::floor(local.getMaximum().y / voxelSize / BLOCK_SIZE)) + 1);
        const int z1 = std::min(count.z - 1, static_cast<int>(std::floor(local.getMaximum().z / voxelSize / BLOCK_SIZE)) + 1);
        for (int z = z0; z <= z1; z++){
            for (int y = y0; y <= y1; y++){
                for (int x = x0; x <= x1; x++){
                    const BlockKey key{0, x, y, z};
                    const auto view = state->store->readView(key, 0);
                    MeshRequest request;
                    request.key = key;
                    request.mode = MeshMode::CollisionOnly;
                    request.coverage = state->coverage.get();
                    MeshOut mesh;
                    meshBlock(*view, request, mesh);
                    state->store->publish(view->takeResolved());
                    const uint32_t base = static_cast<uint32_t>(vertices.size());
                    for (const MeshVertex& vertex : mesh.vertices){
                        const Vector3 p(fixedToLocal(vertex.position[0], voxelSize), fixedToLocal(vertex.position[1], voxelSize), fixedToLocal(vertex.position[2], voxelSize));
                        vertices.push_back(state->model * p);
                    }
                    for (uint16_t index : mesh.indices) indices.push_back(base + index);
                }
            }
        }
    }
}

// ---- stats

VoxelStats VoxelSystem::getStats(Entity owner) const{
    VoxelStats stats;
    const VolumeState* state = stateOf(owner);
    if (!state || !state->store) return stats;
    stats.blocks = state->store->countBlocks();
    stats.memory = state->store->memory();
    stats.meshBytes = state->meshBytes;
    stats.triangles = state->triangles;
    stats.chunks = static_cast<int>(state->onScreen.size());
    stats.draws = stats.chunks;
    stats.jobsPending = queue ? static_cast<int>(queue->queued() + queue->inFlight()) : 0;
    stats.collisionBodies = state->bodies;
    stats.lastMeshMs = state->lastMeshMs;
    stats.lastLatencyFrames = state->lastLatencyFrames;
    stats.uploadBytesThisFrame = state->uploadBytesThisFrame;
    stats.uploadMeshesThisFrame = state->uploadMeshesThisFrame;
    stats.rescues = state->rescues;
    stats.shapesBuiltOnMain = state->shapesBuiltOnMain;
    stats.tier = state->tier;
    stats.levels = state->levels;
    return stats;
}

std::vector<std::shared_ptr<const MeshOut>> VoxelSystem::getVisibleMeshes(Entity owner) const{
    std::vector<std::shared_ptr<const MeshOut>> out;
    const VolumeState* state = stateOf(owner);
    if (!state) return out;
    for (const auto& item : state->onScreen) out.push_back(item.second);
    return out;
}

std::map<BlockKey, MeshId> VoxelSystem::getVisibleBlocks(Entity owner) const{
    const VolumeState* state = stateOf(owner);
    return state ? state->scheduler.visible() : std::map<BlockKey, MeshId>{};
}

void VoxelSystem::finishPendingWork(){
    for (int guard = 0; guard < 4096; guard++){
        if (queue) queue->finishAll();
        pumpJobs();
        bool idle = !queue || (queue->queued() == 0 && queue->inFlight() == 0);
        for (Entity owner : stateOwners()){
            VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
            if (VolumeState* state = stateOf(owner); state && component) updateVolume(*state, *component, 0.0);
            if (const VolumeState* state = stateOf(owner)) idle = idle && state->scheduler.idle() && state->uploadQueue.empty();
        }
        if (idle && (!queue || (queue->queued() == 0 && queue->inFlight() == 0))) return;
    }
}
