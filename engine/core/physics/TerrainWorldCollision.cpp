// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "physics/TerrainWorldCollision.h"

#include "Engine.h"
#include "Scene.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "terrain/world/WorldChunkSamples.h"
#include "Log.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "component/CharacterControllerComponent.h"
#include "component/RigidbodyComponent.h"
#include "subsystem/PhysicsSystem.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyInterface.h"
#include "Jolt/Physics/Collision/Shape/EmptyShape.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"
#include "Jolt/Physics/Collision/Shape/StaticCompoundShape.h"
#endif

#include <algorithm>
#include <cmath>
#include <set>

using namespace jasperengine;
using namespace jasperengine::terrain;

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    uint64_t chunkKey(TileCoord tile, int chunkX, int chunkZ) {
        return (static_cast<uint64_t>(tile.key()) << 32) | (static_cast<uint64_t>(static_cast<uint16_t>(chunkX)) << 16) |
               static_cast<uint16_t>(chunkZ);
    }

    TileCoord tileOfKey(uint64_t key) { return TileCoord::fromKey(static_cast<uint32_t>(key >> 32)); }
    int chunkXOfKey(uint64_t key) { return static_cast<int>((key >> 16) & 0xFFFF); }
    int chunkZOfKey(uint64_t key) { return static_cast<int>(key & 0xFFFF); }

    // seconds a chunk stays after no subject wants it, so a body at a border does not churn it
    constexpr double CHUNK_LINGER_SECONDS = 1.0;
    // R23: the error Jolt's compression may add, in metres
    constexpr float CHUNK_MAX_ERROR = 0.01f;

}

namespace {

    JPH::ShapeRefC buildHeightField(const std::vector<float>& samples, int side, float spacing) {
        JPH::HeightFieldShapeSettings settings(samples.data(), JPH::Vec3::sZero(), JPH::Vec3(spacing, 1.0f, spacing), static_cast<JPH::uint32>(side));
        // R23: 1 cm, at most 8 bits a sample
        settings.mBitsPerSample = std::min<JPH::uint32>(8u, std::max<JPH::uint32>(1u, settings.CalculateBitsPerSampleForError(CHUNK_MAX_ERROR)));
        JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (!result.IsValid()) return nullptr;
        return result.Get();
    }

}

struct TerrainWorldCollision::Impl {
    struct Chunk {
        uint32_t revision = 0;
        bool building = false;
        jobs::Task<JPH::ShapeRefC> task;
        JPH::ShapeRefC shape;
        float originX = 0.0f, originZ = 0.0f;
        double lastWanted = 0.0;
    };

    struct Coarse {
        JPH::ShapeRefC shape;
        float originX = 0.0f, originZ = 0.0f;
    };

    std::map<uint64_t, Chunk> chunks;
    std::map<uint32_t, Coarse> coarse;
    // the fine chunks each tile's subjects want, last pump
    std::map<uint32_t, std::set<uint64_t>> wantedByTile;
    bool dirty = false;
    bool applied = false;
    double clock = 0.0;
};

TerrainWorldCollision::TerrainWorldCollision(TerrainWorldRuntime& runtime): impl(std::make_unique<Impl>()), runtime(runtime) {}

TerrainWorldCollision::~TerrainWorldCollision() {
    clear();
}

void TerrainWorldCollision::clear() {
    for (auto& [key, chunk] : impl->chunks) {
        if (chunk.building) chunk.task.cancel();
    }
    impl->chunks.clear();
    impl->coarse.clear();
    impl->wantedByTile.clear();
    fineCount = 0;
    coarseCount = 0;
    if (impl->applied) {
        Scene* scene = runtime.getScene();
        std::shared_ptr<PhysicsSystem> physics = scene ? scene->getSystem<PhysicsSystem>() : nullptr;
        if (physics && scene->findComponent<RigidbodyComponent>(runtime.getEntity())) {
            physics->setPrebuiltBodyShape3D(runtime.getEntity(), new JPH::EmptyShape());
        }
        impl->applied = false;
    }
}

bool TerrainWorldCollision::fineActive(TileCoord tile) const {
    const auto it = impl->wantedByTile.find(tile.key());
    if (it == impl->wantedByTile.end() || it->second.empty()) return runtime.resident(tile) != nullptr;
    for (uint64_t key : it->second) {
        const auto chunk = impl->chunks.find(key);
        if (chunk == impl->chunks.end() || !chunk->second.shape || chunk->second.revision != runtime.tileRevision(tile)) return false;
    }
    return true;
}

bool TerrainWorldCollision::ensureAt(float localX, float localZ) {
    const WorldLattice& lattice = runtime.lattice();
    const float size = lattice.tileSize();
    const TileCoord tile(tileOfLocal(localX, size), tileOfLocal(localZ, size));
    if (!tile.addressable() || !runtime.occupancy().present(tile)) return false;
    runtime.tiles().demand(tile, TilePriority::Critical);
    if (fineActive(tile) && runtime.resident(tile)) return true;
    if (!impl->coarse.count(tile.key())) {
        std::vector<float> samples;
        if (!worldCoarseSamples(runtime.getOverview(), runtime.maxHeight(), tile, samples)) return false;
        Impl::Coarse entry;
        entry.shape = buildHeightField(samples, lattice.overviewSide(), size / static_cast<float>(lattice.overviewStride));
        entry.originX = static_cast<float>(tile.x) * size;
        entry.originZ = static_cast<float>(tile.z) * size;
        if (!entry.shape) return false;
        impl->coarse[tile.key()] = entry;
        impl->dirty = true;
    }
    // now, not at the next pump: the caller teleports next
    pump(-1.0f, 0);
    return true;
}

void TerrainWorldCollision::pump(float collisionRadius, int chunkQuads) {
    Scene* scene = runtime.getScene();
    if (!scene) return;
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    const Entity worldEntity = runtime.getEntity();
    RigidbodyComponent* worldBody = scene->findComponent<RigidbodyComponent>(worldEntity);
    // R20: physics is enabled by a Rigidbody on the World, and runs while the scene simulates
    if (!physics || physics->isPaused() || !worldBody || !runtime.started()) {
        if (!impl->chunks.empty() || !impl->coarse.empty()) clear();
        return;
    }
    const bool applyOnly = collisionRadius < 0.0f;
    impl->clock = Engine::getRealtimeSinceStartup();
    const WorldLattice& lattice = runtime.lattice();
    const float size = lattice.tileSize();
    const float maxHeight = runtime.maxHeight();

    if (!applyOnly) {
        const int quads = std::clamp(isPowerOfTwo(chunkQuads) ? chunkQuads : 64, 32, lattice.tileQuads);
        const int chunksPerTile = lattice.tileQuads / quads;
        const float chunkSize = static_cast<float>(quads) * lattice.spacing;
        Vector3 origin;
        if (const Transform* transform = scene->findComponent<Transform>(worldEntity)) origin = transform->worldPosition;

        std::set<uint64_t> wanted;
        std::set<uint32_t> coarseWanted;
        std::map<uint32_t, std::set<uint64_t>> byTile;
        auto wantChunk = [&](int globalChunkX, int globalChunkZ) {
            const TileCoord tile(floorDiv(globalChunkX, chunksPerTile), floorDiv(globalChunkZ, chunksPerTile));
            if (!tile.addressable() || !runtime.occupancy().present(tile)) return;
            const int cx = globalChunkX - tile.x * chunksPerTile, cz = globalChunkZ - tile.z * chunksPerTile;
            const uint64_t key = chunkKey(tile, cx, cz);
            wanted.insert(key);
            byTile[tile.key()].insert(key);
        };
        auto wantCircle = [&](float x, float z, float radius) {
            const int lowX = static_cast<int>(std::floor((x - radius) / chunkSize)), highX = static_cast<int>(std::floor((x + radius) / chunkSize));
            const int lowZ = static_cast<int>(std::floor((z - radius) / chunkSize)), highZ = static_cast<int>(std::floor((z + radius) / chunkSize));
            for (int gz = lowZ; gz <= highZ; gz++) {
                for (int gx = lowX; gx <= highX; gx++) {
                    const float minX = static_cast<float>(gx) * chunkSize, minZ = static_cast<float>(gz) * chunkSize;
                    const float dx = x < minX ? minX - x : (x > minX + chunkSize ? x - minX - chunkSize : 0.0f);
                    const float dz = z < minZ ? minZ - z : (z > minZ + chunkSize ? z - minZ - chunkSize : 0.0f);
                    if (dx * dx + dz * dz <= radius * radius) wantChunk(gx, gz);
                }
            }
        };
        auto wantRing = [&](float x, float z) {
            const int gx = static_cast<int>(std::floor(x / chunkSize)), gz = static_cast<int>(std::floor(z / chunkSize));
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) wantChunk(gx + dx, gz + dz);
            }
        };
        // a subject over a present tile that is not in memory gets the coarse tile at once (R22)
        auto standOn = [&](float x, float z) {
            const TileCoord tile(tileOfLocal(x, size), tileOfLocal(z, size));
            if (!tile.addressable() || !runtime.occupancy().present(tile)) return;
            if (!runtime.resident(tile)) {
                coarseWanted.insert(tile.key());
                runtime.tiles().demand(tile, TilePriority::Critical);
            }
        };

        // R21: the focuses, at the collision radius
        for (const StreamFocus& focus : runtime.tiles().focuses()) {
            wantCircle(focus.x, focus.z, collisionRadius * focus.radiusScale);
            standOn(focus.x, focus.z);
        }
        // R21: what moves, nearest the focuses first, at most MAX_TRANSIENT_SUBJECTS
        struct Subject {
            float x, z, px, pz;
            float distance;
            JPH::BodyID body;
        };
        std::vector<Subject> subjects;
        JPH::BodyInterface& bodies = physics->getBodyInterface3D();
        auto nearestFocus = [&](float x, float z) {
            float best = 1.0e30f;
            for (const StreamFocus& focus : runtime.tiles().focuses()) best = std::min(best, (focus.x - x) * (focus.x - x) + (focus.z - z) * (focus.z - z));
            return best;
        };
        auto rigidbodies = scene->getComponentArray<RigidbodyComponent>();
        for (int i = 0; i < rigidbodies->size(); i++) {
            const RigidbodyComponent& body = rigidbodies->getComponentFromIndex(i);
            if (body.type != BodyType::DYNAMIC || body.body.IsInvalid() || !bodies.IsActive(body.body)) continue;
            const JPH::RVec3 position = bodies.GetPosition(body.body);
            const JPH::Vec3 velocity = bodies.GetLinearVelocity(body.body);
            const float x = static_cast<float>(position.GetX()) - origin.x, z = static_cast<float>(position.GetZ()) - origin.z;
            subjects.push_back(Subject{x, z, x + velocity.GetX() * 0.25f, z + velocity.GetZ() * 0.25f, nearestFocus(x, z), body.body});
        }
        auto characters = scene->getComponentArray<CharacterControllerComponent>();
        for (int i = 0; i < characters->size(); i++) {
            const Transform* transform = scene->findComponent<Transform>(characters->getEntity(i));
            if (!transform) continue;
            const float x = transform->worldPosition.x - origin.x, z = transform->worldPosition.z - origin.z;
            subjects.push_back(Subject{x, z, x, z, nearestFocus(x, z), JPH::BodyID()});
        }
        std::sort(subjects.begin(), subjects.end(), [](const Subject& a, const Subject& b) { return a.distance < b.distance; });
        int frozen = 0;
        for (size_t i = 0; i < subjects.size(); i++) {
            const Subject& subject = subjects[i];
            if (static_cast<int>(i) >= MAX_TRANSIENT_SUBJECTS) {
                // past the cap the farthest sleep, so none falls through unwatched
                if (!subject.body.IsInvalid()) {
                    bodies.DeactivateBody(subject.body);
                    frozen++;
                }
                continue;
            }
            wantRing(subject.x, subject.z);
            wantRing(subject.px, subject.pz);
            standOn(subject.x, subject.z);
        }
        if (frozen > 0 && frozenCount == 0) {
            Log::warn("Terrain World: more than %i moving bodies need collision; the farthest %i were put to sleep.", MAX_TRANSIENT_SUBJECTS, frozen);
        }
        frozenCount = frozen;

        // poll the chunk jobs
        for (auto& [key, chunk] : impl->chunks) {
            if (!chunk.building || !chunk.task.isReady()) continue;
            chunk.building = false;
            if (chunk.task.status() == jobs::TaskStatus::Completed && chunk.task.get()) {
                chunk.shape = chunk.task.get();
                impl->dirty = true;
            }
            chunk.task = jobs::Task<JPH::ShapeRefC>();
        }

        // start the wanted chunks whose tile is in memory; at most MAX_CHUNKS_PER_FRAME a pump
        int started = 0;
        for (uint64_t key : wanted) {
            const TileCoord tile = tileOfKey(key);
            Impl::Chunk& chunk = impl->chunks[key];
            chunk.lastWanted = impl->clock;
            const uint32_t revision = runtime.tileRevision(tile);
            if (chunk.building || (chunk.shape && chunk.revision == revision)) continue;
            if (started >= MAX_CHUNKS_PER_FRAME) continue;
            std::shared_ptr<const WorldTile> data = runtime.resident(tile);
            if (!data) continue;
            TileNeighbourhood near;
            std::shared_ptr<const WorldTile> keep[3][3];
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    keep[dz + 1][dx + 1] = runtime.resident(TileCoord(tile.x + dx, tile.z + dz));
                    near.tiles[dz + 1][dx + 1] = keep[dz + 1][dx + 1].get();
                }
            }
            // samples here, from the tiles as they are now; the shape on a job
            auto samples = std::make_shared<std::vector<float>>();
            worldChunkSamples(near, lattice, maxHeight, quads, chunkXOfKey(key), chunkZOfKey(key), *samples);
            const int side = quads + 1;
            const float spacing = lattice.spacing;
            chunk.revision = revision;
            chunk.originX = static_cast<float>(tile.x) * size + static_cast<float>(chunkXOfKey(key)) * chunkSize;
            chunk.originZ = static_cast<float>(tile.z) * size + static_cast<float>(chunkZOfKey(key)) * chunkSize;
            if (jobs::hasSystem()) {
                chunk.task = jobs::submit([samples, side, spacing]() { return buildHeightField(*samples, side, spacing); },
                                          {jobs::Lane::Background, jobs::GateClass::Default, "terrain-world-collision"});
                chunk.building = true;
            } else {
                chunk.shape = buildHeightField(*samples, side, spacing);
                impl->dirty = true;
            }
            started++;
        }

        // R22: coarse tiles under subjects, at most MAX_COARSE_PER_FRAME new ones a pump
        int coarseBuilt = 0;
        for (uint32_t key : coarseWanted) {
            if (impl->coarse.count(key) || coarseBuilt >= MAX_COARSE_PER_FRAME) continue;
            const TileCoord tile = TileCoord::fromKey(key);
            std::vector<float> samples;
            if (!worldCoarseSamples(runtime.getOverview(), maxHeight, tile, samples)) continue;
            Impl::Coarse entry;
            entry.shape = buildHeightField(samples, lattice.overviewSide(), size / static_cast<float>(lattice.overviewStride));
            entry.originX = static_cast<float>(tile.x) * size;
            entry.originZ = static_cast<float>(tile.z) * size;
            if (!entry.shape) continue;
            impl->coarse[key] = entry;
            impl->dirty = true;
            coarseBuilt++;
        }
        impl->wantedByTile = std::move(byTile);

        // a coarse tile goes in the same swap as the fine chunks that replace it
        for (auto it = impl->coarse.begin(); it != impl->coarse.end();) {
            const TileCoord tile = TileCoord::fromKey(it->first);
            const bool absent = !runtime.occupancy().present(tile);
            const bool replaced = runtime.resident(tile) && fineActive(tile) && !coarseWanted.count(it->first);
            if (absent || replaced) {
                it = impl->coarse.erase(it);
                impl->dirty = true;
            } else {
                ++it;
            }
        }
        // chunks nobody wants any more, of a tile gone from memory, or stale past their linger
        for (auto it = impl->chunks.begin(); it != impl->chunks.end();) {
            const TileCoord tile = tileOfKey(it->first);
            const bool gone = !runtime.resident(tile);
            const bool idle = impl->clock - it->second.lastWanted > CHUNK_LINGER_SECONDS;
            if ((gone || idle) && !it->second.building) {
                if (it->second.shape) impl->dirty = true;
                it = impl->chunks.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (!impl->dirty) return;
    // one shape: every coarse tile and fine chunk, in key order so a swap keeps sub-shape order
    JPH::StaticCompoundShapeSettings compound;
    int fine = 0, coarseShapes = 0;
    for (const auto& [key, entry] : impl->coarse) {
        compound.AddShape(JPH::Vec3(entry.originX, 0.0f, entry.originZ), JPH::Quat::sIdentity(), entry.shape);
        coarseShapes++;
    }
    for (const auto& [key, chunk] : impl->chunks) {
        if (!chunk.shape) continue;
        compound.AddShape(JPH::Vec3(chunk.originX, 0.0f, chunk.originZ), JPH::Quat::sIdentity(), chunk.shape);
        fine++;
    }
    JPH::ShapeRefC shape;
    if (fine + coarseShapes == 0) {
        shape = new JPH::EmptyShape();
    } else {
        JPH::ShapeSettings::ShapeResult result = compound.Create();
        if (!result.IsValid()) {
            Log::error("Terrain World: its collision could not be built: %s", result.GetError().c_str());
            return;
        }
        shape = result.Get();
    }
    if (!physics->setPrebuiltBodyShape3D(worldEntity, shape)) return;   // inside a step: the next pump
    impl->dirty = false;
    impl->applied = true;
    fineCount = fine;
    coarseCount = coarseShapes;
    swaps++;
}

#else

struct TerrainWorldCollision::Impl {};

TerrainWorldCollision::TerrainWorldCollision(TerrainWorldRuntime& runtime): impl(std::make_unique<Impl>()), runtime(runtime) {}
TerrainWorldCollision::~TerrainWorldCollision() = default;
void TerrainWorldCollision::clear() {}
bool TerrainWorldCollision::fineActive(TileCoord tile) const { return runtime.resident(tile) != nullptr; }
bool TerrainWorldCollision::ensureAt(float, float) { return false; }
void TerrainWorldCollision::pump(float, int) {}

#endif
