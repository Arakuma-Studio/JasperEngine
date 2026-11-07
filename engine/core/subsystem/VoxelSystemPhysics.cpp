// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R21-R24, V7, V8: one static body per level-0 block near a focus. Shapes are built on jobs,
// added in batches, and swapped in one fixed tick after the render activation, so there is no time
// with a gap. A body past a triangle has no thickness to stop it: the SDF is the inside-outside
// oracle that brings it back.

#include "subsystem/VoxelSystem.h"

#include "subsystem/VoxelSystemState.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/CharacterControllerComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "voxel/TransvoxelMesher.h"
#include "voxel/VoxelSample.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "physics/JoltShapeBuilder.h"
#include "subsystem/PhysicsSystem.h"
#include "Jolt/Physics/Body/BodyInterface.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h"
#include "Jolt/Physics/Collision/ObjectLayer.h"
#include "Jolt/Physics/PhysicsSystem.h"
#endif

#include <algorithm>
#include <cmath>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::voxel;

namespace {
    using BlockTuple = std::tuple<int, int, int>;
}

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    // Runs on a job or inline; touches only what the job owns
    void buildCollisionShape(const ReadView& view, const BlockKey& key, const Coverage* coverage, float voxelSize, JPH::ShapeRefC& shape, bool& empty) {
        MeshRequest request;
        request.key = key;
        request.mode = MeshMode::CollisionOnly;
        request.coverage = coverage;
        MeshOut mesh;
        meshBlock(view, request, mesh);
        empty = mesh.empty();
        if (empty) return;
        std::vector<Vector3> vertices;
        vertices.reserve(mesh.vertices.size());
        for (const MeshVertex& vertex : mesh.vertices) {
            vertices.emplace_back(fixedToLocal(vertex.position[0], voxelSize), fixedToLocal(vertex.position[1], voxelSize), fixedToLocal(vertex.position[2], voxelSize));
        }
        std::vector<uint32_t> indices(mesh.indices.begin(), mesh.indices.end());
        std::string error;
        shape = JoltShapeBuilder::buildMeshShape(vertices, indices, &error, nullptr);
        if (!shape) empty = true;
    }

}

void VoxelSystem::startCollisionJob(VolumeState& state, const Int3& key, bool partOfSwap){
    if (!queue) queue = std::make_unique<WorkQueue>();
    VoxelCollisionBlock& block = state.collision[BlockTuple(key.x, key.y, key.z)];
    block.building = true;
    block.built = false;
    block.lastNeededTick = fixedTick;
    const uint64_t jobId = nextJobId++;
    auto job = std::make_unique<Job>();
    job->kind = Job::Collision;
    job->owner = state.owner;
    job->collisionKey = key;
    job->partOfSwap = partOfSwap;
    job->collisionVersion = block.wantedVersion;
    job->view = state.store->readView(BlockKey{0, key.x, key.y, key.z}, 0);
    job->coverage = state.coverage;
    Job* raw = job.get();
    jobs[jobId] = std::move(job);
    const float voxelSize = state.store->desc().voxelSize;
    const double span = BLOCK_SIZE;
    const Vec3d focus = worldVoxels(state, focusPositionWorld(*scene->findComponent<VoxelVolumeComponent>(state.owner)));
    const double cx = (key.x + 0.5) * span - focus.x, cy = (key.y + 0.5) * span - focus.y, cz = (key.z + 0.5) * span - focus.z;
    queue->push(jobId, partOfSwap ? WorkPriority::Edit : WorkPriority::Near, std::sqrt(cx * cx + cy * cy + cz * cz), [raw, voxelSize]{
        raw->builtOnMainThread = jobs::isMainThread();
        const BlockKey blockKey{0, raw->collisionKey.x, raw->collisionKey.y, raw->collisionKey.z};
        buildCollisionShape(*raw->view, blockKey, raw->coverage.get(), voxelSize, raw->shape, raw->empty);
    });
}

void VoxelSystem::onCollisionJobDone(VolumeState& state, Job& job){
    const BlockTuple key(job.collisionKey.x, job.collisionKey.y, job.collisionKey.z);
    const auto found = state.collision.find(key);
    if (state.store) state.store->publish(job.view->takeResolved());
    if (job.builtOnMainThread) state.shapesBuiltOnMain++;
    if (found == state.collision.end()) return;
    VoxelCollisionBlock& block = found->second;
    block.building = false;
    // An edit landed while this ran: its result is stale, the swap's own job follows
    if (job.collisionVersion != block.wantedVersion){
        startCollisionJob(state, job.collisionKey, true);
        return;
    }
    block.built = true;
    block.version = job.collisionVersion;
    block.pendingShape = job.shape;
    block.pendingEmpty = job.empty;
    if (!job.partOfSwap && !block.inWorld && !job.empty) state.readyBodies.push_back(key);
}

void VoxelSystem::onCollisionEdited(VolumeState& state, const IntBox& changed, uint64_t setId){
    VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(state.owner);
    if (!component || !component->collision) return;
    // The window of a collision block: samples 0..16 of level 0
    const int x0 = std::max(0, (changed.min.x - 1) >> BLOCK_SHIFT), y0 = std::max(0, (changed.min.y - 1) >> BLOCK_SHIFT), z0 = std::max(0, (changed.min.z - 1) >> BLOCK_SHIFT);
    const int x1 = changed.max.x >> BLOCK_SHIFT, y1 = changed.max.y >> BLOCK_SHIFT, z1 = changed.max.z >> BLOCK_SHIFT;
    VoxelCollisionSwap swap;
    swap.renderSet = setId;
    swap.changed = changed;
    swap.editFrame = state.latencyStartFrame;
    state.collisionEdits++;
    for (auto& item : state.collision){
        const int x = std::get<0>(item.first), y = std::get<1>(item.first), z = std::get<2>(item.first);
        if (x < x0 || x > x1 || y < y0 || y > y1 || z < z0 || z > z1) continue;
        item.second.wantedVersion = state.collisionEdits;
        swap.blocks.push_back(Int3{x, y, z});
        if (!item.second.building) startCollisionJob(state, Int3{x, y, z}, true);
    }
    // A dig next to the bubble where no block was built yet still lands in one tick when it is built
    if (!swap.blocks.empty()) state.swaps.push_back(std::move(swap));
}

void VoxelSystem::applyCollisionSwaps(VolumeState& state){
    auto physics = scene->findSystem<PhysicsSystem>();
    if (!physics || state.swaps.empty()) return;
    JPH::BodyInterface& bodies = physics->getBodyInterface3D();
    const double voxelSize = state.store->desc().voxelSize;
    while (!state.swaps.empty()){
        VoxelCollisionSwap& swap = state.swaps.front();
        // R23: the first tick at or after the render activation, all of the set at once
        if (state.scheduler.lastActivatedSet() < swap.renderSet) break;
        bool ready = true;
        for (const Int3& key : swap.blocks){
            const auto found = state.collision.find(BlockTuple(key.x, key.y, key.z));
            if (found != state.collision.end() && (!found->second.built || found->second.adding || found->second.version != found->second.wantedVersion)) ready = false;
        }
        if (!ready) break;

        std::vector<JPH::BodyID> added;
        for (const Int3& key : swap.blocks){
            const auto found = state.collision.find(BlockTuple(key.x, key.y, key.z));
            if (found == state.collision.end()) continue;
            VoxelCollisionBlock& block = found->second;
            // an earlier swap over this block applied its newest shape already
            if (block.appliedVersion == block.version && !block.pendingShape) continue;
            block.appliedVersion = block.version;
            if (block.pendingEmpty || !block.pendingShape){
                if (block.inWorld){
                    if (block.entity != NULL_ENTITY && scene->isEntityCreated(block.entity)){
                        generatedOwners.erase(block.entity);
                        physics->removeRigidbody(block.entity);
                        scene->destroyEntity(block.entity);
                    }
                    block.entity = NULL_ENTITY;
                    block.inWorld = false;
                    state.bodies--;
                }
                block.pendingShape = nullptr;
                continue;
            }
            if (block.inWorld){
                bodies.SetShape(block.body, block.pendingShape.GetPtr(), false, JPH::EActivation::DontActivate);
            }else{
                const Entity entity = block.entity != NULL_ENTITY ? block.entity : scene->createEntity();
                if (block.entity == NULL_ENTITY){
                    scene->addComponent<Transform>(entity);
                    Transform& transform = scene->getComponent<Transform>(entity);
                    transform.position = state.worldPosition;
                    transform.rotation = state.worldRotation;
                    transform.worldPosition = state.worldPosition;
                    transform.worldRotation = state.worldRotation;
                    transform.modelMatrix = state.model;
                    scene->setEntityLayer(entity, scene->getEntityLayer(state.owner));
                    physics->createRigidbody(entity);
                    generatedOwners[entity] = state.owner;
                    block.entity = entity;
                }
                block.body = physics->createStaticBodyForShape(entity, block.pendingShape.GetPtr());
                if (!block.body.IsInvalid()){
                    added.push_back(block.body);
                    block.inWorld = true;
                    state.bodies++;
                }
            }
            block.pendingShape = nullptr;
        }
        if (!added.empty()){
            const JPH::BodyInterface::AddState add = physics->prepareBodies(added);
            physics->finalizeBodies(added, add);
        }

        // R23: wakes what the change touches and nothing else
        const float size = static_cast<float>(voxelSize);
        const AABB local(swap.changed.min.x * size, swap.changed.min.y * size, swap.changed.min.z * size,
                         (swap.changed.max.x + 1) * size, (swap.changed.max.y + 1) * size, (swap.changed.max.z + 1) * size);
        const AABB world = state.model * local;
        JPH::AABox box(JPH::Vec3(world.getMinimum().x, world.getMinimum().y, world.getMinimum().z), JPH::Vec3(world.getMaximum().x, world.getMaximum().y, world.getMaximum().z));
        box.ExpandBy(JPH::Vec3::sReplicate(0.1f));
        bodies.ActivateBodiesInAABox(box, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
        state.collisionEvents.push_back(world);
        if (swap.editFrame != 0) state.lastLatencyFrames = static_cast<int>(frameCounter - swap.editFrame);
        state.swaps.pop_front();
    }
}

void VoxelSystem::addNewBodies(VolumeState& state){
    auto physics = scene->findSystem<PhysicsSystem>();
    if (!physics) return;
    // What was prepared last tick lands before this step (V8)
    for (size_t i = 0; i < state.pendingAdds.size();){
        VolumeState::PendingAdd& add = state.pendingAdds[i];
        if (add.hasTask && !add.task.isReady()){
            i++;
            continue;
        }
        if (add.hasTask) add.state = add.task.get();
        physics->finalizeBodies(*add.ids, add.state);
        for (const BlockTuple& key : add.keys){
            const auto found = state.collision.find(key);
            if (found != state.collision.end()) found->second.adding = false;
        }
        state.pendingAdds.erase(state.pendingAdds.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (state.readyBodies.empty()) return;
    const int batch = voxelTierSettings(state.tier).bodyBatch;
    VolumeState::PendingAdd pending;
    pending.ids = std::make_shared<std::vector<JPH::BodyID>>();
    while (!state.readyBodies.empty() && static_cast<int>(pending.ids->size()) < batch){
        const BlockTuple key = state.readyBodies.front();
        state.readyBodies.pop_front();
        const auto found = state.collision.find(key);
        if (found == state.collision.end() || !found->second.pendingShape || found->second.inWorld) continue;
        VoxelCollisionBlock& block = found->second;
        const Entity entity = scene->createEntity();
        scene->addComponent<Transform>(entity);
        Transform& transform = scene->getComponent<Transform>(entity);
        transform.position = state.worldPosition;
        transform.rotation = state.worldRotation;
        transform.worldPosition = state.worldPosition;
        transform.worldRotation = state.worldRotation;
        transform.modelMatrix = state.model;
        scene->setEntityLayer(entity, scene->getEntityLayer(state.owner));
        physics->createRigidbody(entity);
        generatedOwners[entity] = state.owner;
        if (const RigidbodyComponent* ownerBody = scene->findComponent<RigidbodyComponent>(state.owner)){
            RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(entity);
            body.friction = ownerBody->friction;
            body.restitution = ownerBody->restitution;
            body.material = ownerBody->material;
        }
        block.entity = entity;
        block.body = physics->createStaticBodyForShape(entity, block.pendingShape.GetPtr());
        block.pendingShape = nullptr;
        block.appliedVersion = block.version;
        if (block.body.IsInvalid()) continue;
        block.inWorld = true;
        state.bodies++;
        block.adding = true;
        pending.ids->push_back(block.body);
        pending.keys.push_back(key);
    }
    if (pending.ids->empty()) return;
    // Jolt documents prepare as thread-safe (BodyInterface.h); with 0 workers a job would wait for
    // the pump, so it runs here
    if (jobs::system().isSingleThreaded()){
        pending.state = physics->prepareBodies(*pending.ids);
        physics->finalizeBodies(*pending.ids, pending.state);
        for (const BlockTuple& key : pending.keys) state.collision[key].adding = false;
        return;
    }
    state.pendingAdds.push_back(std::move(pending));
    VolumeState::PendingAdd& stored = state.pendingAdds.back();
    PhysicsSystem* system = physics.get();
    std::shared_ptr<std::vector<JPH::BodyID>> ids = stored.ids;
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "voxel-add-bodies";
    stored.task = jobs::submit([system, ids]{ return system->prepareBodies(*ids); }, options);
    stored.hasTask = true;
}

void VoxelSystem::evictBodies(VolumeState& state){
    auto physics = scene->findSystem<PhysicsSystem>();
    if (!physics) return;
    const int cap = voxelTierSettings(state.tier).bodyCap;
    if (state.bodies <= cap) return;
    // The least recently needed block with no body nearby goes first (R22)
    std::vector<std::pair<uint64_t, BlockTuple>> candidates;
    for (const auto& item : state.collision) {
        if (item.second.inWorld && !item.second.adding) candidates.emplace_back(item.second.lastNeededTick, item.first);
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& candidate : candidates){
        if (state.bodies <= cap) break;
        if (candidate.first + 1 >= fixedTick) continue;
        auto found = state.collision.find(candidate.second);
        VoxelCollisionBlock& block = found->second;
        if (block.entity != NULL_ENTITY && scene->isEntityCreated(block.entity)){
            generatedOwners.erase(block.entity);
            physics->removeRigidbody(block.entity);
            scene->destroyEntity(block.entity);
        }
        state.collision.erase(found);
        state.bodies--;
    }
}

void VoxelSystem::updateCollisionWanted(VolumeState& state, VoxelVolumeComponent& component){
    const VoxelTierSettings settings = voxelTierSettings(state.tier);
    const int radius = component.collisionRadius > 0 ? component.collisionRadius : settings.collisionRadius;
    const Int3 count = state.store->blockCount(0);
    const auto want = [&](int x, int y, int z){
        if (x < 0 || y < 0 || z < 0 || x >= count.x || y >= count.y || z >= count.z) return;
        const BlockTuple key(x, y, z);
        auto found = state.collision.find(key);
        if (found == state.collision.end()){
            VoxelCollisionBlock& block = state.collision[key];
            block.wantedVersion = state.collisionEdits;
            block.version = state.collisionEdits;
            startCollisionJob(state, Int3{x, y, z}, false);
        }else{
            found->second.lastNeededTick = fixedTick;
        }
    };
    const auto around = [&](const Vector3& world, int blocks){
        const Vec3d p = worldVoxels(state, world);
        const int cx = static_cast<int>(std::floor(p.x)) >> BLOCK_SHIFT, cy = static_cast<int>(std::floor(p.y)) >> BLOCK_SHIFT, cz = static_cast<int>(std::floor(p.z)) >> BLOCK_SHIFT;
        for (int z = cz - blocks; z <= cz + blocks; z++){
            for (int y = cy - blocks; y <= cy + blocks; y++){
                for (int x = cx - blocks; x <= cx + blocks; x++) want(x, y, z);
            }
        }
    };
    around(focusPositionWorld(component), radius);
    const Vec3d dim{static_cast<double>(state.store->desc().dimensions.x), static_cast<double>(state.store->desc().dimensions.y), static_cast<double>(state.store->desc().dimensions.z)};
    const auto inside = [&](const Vector3& world){
        const Vec3d p = worldVoxels(state, world);
        const double margin = BLOCK_SIZE;
        return p.x >= -margin && p.y >= -margin && p.z >= -margin && p.x <= dim.x + margin && p.y <= dim.y + margin && p.z <= dim.z + margin;
    };
    auto controllers = scene->getComponentArray<CharacterControllerComponent>();
    for (size_t i = 0; i < controllers->size(); i++){
        const Transform* transform = scene->findComponent<Transform>(controllers->getEntity(i));
        if (transform && inside(transform->worldPosition)) around(transform->worldPosition, radius);
    }
    // Awake dynamic bodies: the blocks their box grown by one block overlaps, up to 64 a tick (R22)
    auto rigidbodies = scene->getComponentArray<RigidbodyComponent>();
    auto physics = scene->findSystem<PhysicsSystem>();
    if (!physics) return;
    JPH::BodyInterface& bodies = physics->getBodyInterface3D();
    int requests = 0;
    for (size_t i = 0; i < rigidbodies->size() && requests < 64; i++){
        const RigidbodyComponent& body = rigidbodies->getComponentFromIndex(i);
        if (body.type != BodyType::DYNAMIC || body.body.IsInvalid() || !bodies.IsAdded(body.body) || !bodies.IsActive(body.body)) continue;
        const Transform* transform = scene->findComponent<Transform>(rigidbodies->getEntity(i));
        if (!transform || !inside(transform->worldPosition)) continue;
        around(transform->worldPosition, 1);
        requests++;
    }
    evictBodies(state);
}

void VoxelSystem::rescueBodies(VolumeState& state, VoxelVolumeComponent& component, double dt){
    if (!component.rescueBodies) return;
    auto physics = scene->findSystem<PhysicsSystem>();
    if (!physics) return;
    JPH::BodyInterface& bodies = physics->getBodyInterface3D();
    const VoxelTierSettings settings = voxelTierSettings(state.tier);
    const double voxelSize = state.store->desc().voxelSize;
    auto rigidbodies = scene->getComponentArray<RigidbodyComponent>();
    Store& store = *state.store;
    const Int3 dim = store.desc().dimensions;

    const auto inVolume = [&](const Vec3d& p){
        return p.x >= 0.0 && p.y >= 0.0 && p.z >= 0.0 && p.x <= dim.x * voxelSize && p.y <= dim.y * voxelSize && p.z <= dim.z * voxelSize;
    };

    // Fast bodies: LinearCast until they slow down, the authored field untouched (R24). Every
    // dynamic body in the bounds; a cheap pass over few bodies.
    for (size_t i = 0; i < rigidbodies->size(); i++){
        const RigidbodyComponent& body = rigidbodies->getComponentFromIndex(i);
        if (body.type != BodyType::DYNAMIC || body.body.IsInvalid() || !bodies.IsAdded(body.body)) continue;
        const Entity entity = rigidbodies->getEntity(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) continue;
        if (!inVolume(toVec3d(state.inverseModel * transform->worldPosition))) continue;
        const JPH::Vec3 velocity = bodies.GetLinearVelocity(body.body);
        const JPH::AABox local = bodies.GetShape(body.body)->GetLocalBounds();
        const JPH::Vec3 size = local.GetSize();
        const float smallest = std::min(size.GetX(), std::min(size.GetY(), size.GetZ()));
        const bool fast = velocity.Length() * static_cast<float>(dt) > smallest * 0.5f;
        VolumeState::BodyWatch& watch = state.watches[entity];
        if (fast && !watch.fast){
            bodies.SetMotionQuality(body.body, JPH::EMotionQuality::LinearCast);
            watch.fast = true;
        }else if (!fast && watch.fast){
            bodies.SetMotionQuality(body.body, body.motionQuality == RigidbodyMotionQuality::LINEAR_CAST ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
            watch.fast = false;
        }
    }

    // The inside-solid rescue, round robin (R24)
    struct Candidate{
        Entity entity;
        bool character;
    };
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < rigidbodies->size(); i++){
        const RigidbodyComponent& body = rigidbodies->getComponentFromIndex(i);
        if (body.type == BodyType::DYNAMIC && !body.sensor && !body.body.IsInvalid()) candidates.push_back({rigidbodies->getEntity(i), false});
    }
    auto controllers = scene->getComponentArray<CharacterControllerComponent>();
    for (size_t i = 0; i < controllers->size(); i++) candidates.push_back({controllers->getEntity(i), true});
    if (candidates.empty()) return;
    const int budget = std::min<int>(settings.rescueBodiesPerTick, static_cast<int>(candidates.size()));
    for (int n = 0; n < budget; n++){
        const Candidate candidate = candidates[state.rescueCursor++ % candidates.size()];
        Transform* transform = scene->findComponent<Transform>(candidate.entity);
        if (!transform) continue;
        const Vector3 localCentre = state.inverseModel * transform->worldPosition;
        const Vec3d centre = toVec3d(localCentre);
        if (!inVolume(centre)) continue;
        VolumeState::BodyWatch& watch = state.watches[candidate.entity];
        const double value = valueAt(store, centre);
        if (value > 0.0){
            watch.lastFree = localCentre;
            watch.hasFree = true;
        }
        if (value >= -1.5 * voxelSize){
            watch.deepChecks = 0;
            continue;
        }
        if (++watch.deepChecks < 2) continue;
        watch.deepChecks = 0;
        // The first air straight up, skipping uniform blocks
        float halfHeight = 0.5f;
        if (candidate.character){
            if (const CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(candidate.entity)) halfHeight = controller->height * 0.5f;
        }else{
            const RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(candidate.entity);
            if (bodies.IsAdded(body.body)) halfHeight = bodies.GetShape(body.body)->GetLocalBounds().GetExtent().GetY();
        }
        const int ix = std::clamp(static_cast<int>(std::floor(centre.x / voxelSize + 0.5)), 0, dim.x - 1);
        const int iz = std::clamp(static_cast<int>(std::floor(centre.z / voxelSize + 0.5)), 0, dim.z - 1);
        int iy = std::max(static_cast<int>(std::floor(centre.y / voxelSize)), 0);
        bool found = false;
        while (iy < dim.y){
            const BlockContent content = store.content(BlockKey{0, ix >> BLOCK_SHIFT, iy >> BLOCK_SHIFT, std::min(iz >> BLOCK_SHIFT, store.blockCount(0).z - 1)});
            if (content.sdfIsUniform() && content.sdfValueIfUniform() < 0){
                iy = ((iy >> BLOCK_SHIFT) + 1) << BLOCK_SHIFT;
                continue;
            }
            if (store.sdfAt(0, ix, iy, iz) >= 0){
                found = true;
                break;
            }
            iy++;
        }
        Vector3 target;
        if (found){
            target = Vector3(localCentre.x, static_cast<float>(iy * voxelSize) + halfHeight, localCentre.z);
        }else if (watch.hasFree){
            target = watch.lastFree;
        }else{
            continue;
        }
        const Vector3 world = state.model * target;
        transform->position = world;
        transform->worldPosition = world;
        transform->needUpdate = true;
        if (!candidate.character){
            const RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(candidate.entity);
            bodies.SetPosition(body.body, JPH::Vec3(world.x, world.y, world.z), JPH::EActivation::Activate);
            JPH::Vec3 velocity = bodies.GetLinearVelocity(body.body);
            if (velocity.GetY() < 0.0f) bodies.SetLinearVelocity(body.body, JPH::Vec3(velocity.GetX(), 0.0f, velocity.GetZ()));
        }
        state.rescues++;
        state.rescuedEvents.push_back(candidate.entity);
        if (!watch.rescuedOnce){
            watch.rescuedOnce = true;
            Log::warn("Voxel Volume '%s' moved '%s' out of solid ground", scene->getEntityName(state.owner).c_str(), scene->getEntityName(candidate.entity).c_str());
        }
    }
}

void VoxelSystem::buildCollisionBlocking(VolumeState& state, const VoxelVolumeComponent&, const IntBox& blocks){
    auto physics = scene->findSystem<PhysicsSystem>();
    if (!physics) return;
    struct Item{
        Int3 key;
        std::shared_ptr<const ReadView> view;
        JPH::ShapeRefC shape;
        bool empty = false;
    };
    const Int3 count = state.store->blockCount(0);
    std::vector<Item> items;
    for (int z = std::max(blocks.min.z, 0); z <= std::min(blocks.max.z, count.z - 1); z++){
        for (int y = std::max(blocks.min.y, 0); y <= std::min(blocks.max.y, count.y - 1); y++){
            for (int x = std::max(blocks.min.x, 0); x <= std::min(blocks.max.x, count.x - 1); x++){
                const auto found = state.collision.find(BlockTuple(x, y, z));
                if (found != state.collision.end() && found->second.inWorld && !found->second.building) continue;
                if (found != state.collision.end() && found->second.built && found->second.pendingEmpty) continue;
                Item item;
                item.key = Int3{x, y, z};
                item.view = state.store->readView(BlockKey{0, x, y, z}, 0);
                items.push_back(std::move(item));
            }
        }
    }
    if (items.empty()) return;
    const float voxelSize = state.store->desc().voxelSize;
    const Coverage* coverage = state.coverage.get();
    jobs::parallelFor(items.size(), [&items, voxelSize, coverage](size_t index){
        Item& item = items[index];
        buildCollisionShape(*item.view, BlockKey{0, item.key.x, item.key.y, item.key.z}, coverage, voxelSize, item.shape, item.empty);
    });
    std::vector<JPH::BodyID> added;
    for (Item& item : items){
        state.store->publish(item.view->takeResolved());
        VoxelCollisionBlock& block = state.collision[BlockTuple(item.key.x, item.key.y, item.key.z)];
        block.built = true;
        block.building = false;
        block.lastNeededTick = fixedTick;
        block.pendingEmpty = item.empty;
        block.wantedVersion = state.collisionEdits;
        block.version = state.collisionEdits;
        block.appliedVersion = state.collisionEdits;
        if (item.empty || block.inWorld) continue;
        const Entity entity = scene->createEntity();
        scene->addComponent<Transform>(entity);
        Transform& transform = scene->getComponent<Transform>(entity);
        transform.position = state.worldPosition;
        transform.rotation = state.worldRotation;
        transform.worldPosition = state.worldPosition;
        transform.worldRotation = state.worldRotation;
        transform.modelMatrix = state.model;
        scene->setEntityLayer(entity, scene->getEntityLayer(state.owner));
        physics->createRigidbody(entity);
        generatedOwners[entity] = state.owner;
        block.entity = entity;
        block.body = physics->createStaticBodyForShape(entity, item.shape.GetPtr());
        if (block.body.IsInvalid()) continue;
        block.inWorld = true;
        state.bodies++;
        added.push_back(block.body);
    }
    if (!added.empty()){
        const JPH::BodyInterface::AddState add = physics->prepareBodies(added);
        physics->finalizeBodies(added, add);
    }
}

void VoxelSystem::ensureCollision(Entity owner, const AABB& worldBounds){
    VolumeState* state = stateOf(owner);
    VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
    if (!state || !component || !state->store || !component->collision) return;
    const float voxelSize = state->store->desc().voxelSize;
    const AABB local = state->inverseModel * worldBounds;
    IntBox blocks;
    blocks.min = Int3{static_cast<int>(std::floor(local.getMinimum().x / voxelSize)) >> BLOCK_SHIFT, static_cast<int>(std::floor(local.getMinimum().y / voxelSize)) >> BLOCK_SHIFT, static_cast<int>(std::floor(local.getMinimum().z / voxelSize)) >> BLOCK_SHIFT};
    blocks.max = Int3{static_cast<int>(std::floor(local.getMaximum().x / voxelSize)) >> BLOCK_SHIFT, static_cast<int>(std::floor(local.getMaximum().y / voxelSize)) >> BLOCK_SHIFT, static_cast<int>(std::floor(local.getMaximum().z / voxelSize)) >> BLOCK_SHIFT};
    buildCollisionBlocking(*state, *component, blocks);
}

bool VoxelSystem::isCollisionReady(Entity owner, const AABB& worldBounds) const{
    const VolumeState* state = stateOf(owner);
    if (!state || !state->store) return false;
    const float voxelSize = state->store->desc().voxelSize;
    const AABB local = state->inverseModel * worldBounds;
    const Int3 count = state->store->blockCount(0);
    const int x0 = std::max(0, static_cast<int>(std::floor(local.getMinimum().x / voxelSize)) >> BLOCK_SHIFT), x1 = std::min(count.x - 1, static_cast<int>(std::floor(local.getMaximum().x / voxelSize)) >> BLOCK_SHIFT);
    const int y0 = std::max(0, static_cast<int>(std::floor(local.getMinimum().y / voxelSize)) >> BLOCK_SHIFT), y1 = std::min(count.y - 1, static_cast<int>(std::floor(local.getMaximum().y / voxelSize)) >> BLOCK_SHIFT);
    const int z0 = std::max(0, static_cast<int>(std::floor(local.getMinimum().z / voxelSize)) >> BLOCK_SHIFT), z1 = std::min(count.z - 1, static_cast<int>(std::floor(local.getMaximum().z / voxelSize)) >> BLOCK_SHIFT);
    for (int z = z0; z <= z1; z++){
        for (int y = y0; y <= y1; y++){
            for (int x = x0; x <= x1; x++){
                const auto found = state->collision.find(BlockTuple(x, y, z));
                if (found == state->collision.end() || !found->second.built) return false;
                if (!found->second.pendingEmpty && !found->second.inWorld) return false;
            }
        }
    }
    return true;
}

void VoxelSystem::destroyCollision(VolumeState& state){
    auto physics = scene->findSystem<PhysicsSystem>();
    for (auto& add : state.pendingAdds){
        if (add.hasTask) add.state = add.task.get();
        if (physics) physics->finalizeBodies(*add.ids, add.state);
    }
    state.pendingAdds.clear();
    for (auto& item : state.collision){
        const Entity entity = item.second.entity;
        if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)) continue;
        generatedOwners.erase(entity);
        if (physics) physics->removeRigidbody(entity);
        scene->destroyEntity(entity);
    }
    state.collision.clear();
    state.readyBodies.clear();
    state.swaps.clear();
    state.bodies = 0;
}

void VoxelSystem::fixedUpdate(double dt){
    auto volumes = scene->getComponentArray<VoxelVolumeComponent>();
    if (volumes->size() == 0 && states.empty()) return;
    if (paused) return;
    fixedTick++;
    auto physics = scene->findSystem<PhysicsSystem>();
    if (!physics) return;
    if (states.empty()) syncVolumes();
    for (Entity owner : stateOwners()){
        VolumeState* found = stateOf(owner);
        VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
        if (!found || !component) continue;
        VolumeState& state = *found;
        if (!state.store && !ensureLoaded(owner, *component, state)) continue;
        if (!component->collision){
            if (!state.collision.empty()) destroyCollision(state);
            continue;
        }
        if (state.model == Matrix4()){
            if (const Transform* transform = scene->findComponent<Transform>(owner)){
                state.model = transform->modelMatrix;
                state.inverseModel = state.model.inverse();
                state.worldPosition = transform->worldPosition;
                state.worldRotation = transform->worldRotation;
            }
        }
        // R20: a player does not fall through at start
        if (!state.collisionStarted){
            state.collisionStarted = true;
            const VoxelTierSettings settings = voxelTierSettings(resolveTier(*component));
            const int radius = component->collisionRadius > 0 ? component->collisionRadius : settings.collisionRadius;
            const Vec3d p = worldVoxels(state, focusPositionWorld(*component));
            const Int3 centre{static_cast<int>(std::floor(p.x)) >> BLOCK_SHIFT, static_cast<int>(std::floor(p.y)) >> BLOCK_SHIFT, static_cast<int>(std::floor(p.z)) >> BLOCK_SHIFT};
            IntBox blocks;
            blocks.min = Int3{centre.x - radius, centre.y - radius, centre.z - radius};
            blocks.max = Int3{centre.x + radius, centre.y + radius, centre.z + radius};
            buildCollisionBlocking(state, *component, blocks);
            // and so do the bodies that start in the bounds
            auto rigidbodies = scene->getComponentArray<RigidbodyComponent>();
            for (size_t i = 0; i < rigidbodies->size(); i++){
                const RigidbodyComponent& body = rigidbodies->getComponentFromIndex(i);
                if (body.type != BodyType::DYNAMIC) continue;
                const Transform* transform = scene->findComponent<Transform>(rigidbodies->getEntity(i));
                if (!transform) continue;
                const Vec3d q = worldVoxels(state, transform->worldPosition);
                const Int3 c{static_cast<int>(std::floor(q.x)) >> BLOCK_SHIFT, static_cast<int>(std::floor(q.y)) >> BLOCK_SHIFT, static_cast<int>(std::floor(q.z)) >> BLOCK_SHIFT};
                IntBox around;
                around.min = Int3{c.x - 1, c.y - 1, c.z - 1};
                around.max = Int3{c.x + 1, c.y + 1, c.z + 1};
                buildCollisionBlocking(state, *component, around);
            }
        }
        applyCollisionSwaps(state);
        updateCollisionWanted(state, *component);
        addNewBodies(state);
        rescueBodies(state, *component, dt);
    }
}

#else

void VoxelSystem::fixedUpdate(double){}
void VoxelSystem::startCollisionJob(VolumeState&, const Int3&, bool){}
void VoxelSystem::onCollisionJobDone(VolumeState&, Job&){}
void VoxelSystem::onCollisionEdited(VolumeState&, const IntBox&, uint64_t){}
void VoxelSystem::applyCollisionSwaps(VolumeState&){}
void VoxelSystem::addNewBodies(VolumeState&){}
void VoxelSystem::evictBodies(VolumeState&){}
void VoxelSystem::updateCollisionWanted(VolumeState&, VoxelVolumeComponent&){}
void VoxelSystem::rescueBodies(VolumeState&, VoxelVolumeComponent&, double){}
void VoxelSystem::buildCollisionBlocking(VolumeState&, const VoxelVolumeComponent&, const IntBox&){}
void VoxelSystem::ensureCollision(Entity, const AABB&){}
bool VoxelSystem::isCollisionReady(Entity, const AABB&) const{ return false; }
void VoxelSystem::destroyCollision(VolumeState&){}

#endif
