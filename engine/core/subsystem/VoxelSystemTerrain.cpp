// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R35, R38-R46: a volume attached to a Terrain cuts its own holes and meets the ground with no
// seam. The terrain stays only where the volume agrees with it (R39), the volume extends a ring under
// each hole's edge and draws nothing beyond it (R41), and a hole opens only after what shows the
// cavity is on screen (R44).

#include "subsystem/VoxelSystem.h"

#include "subsystem/VoxelSystemState.h"

#include "Log.h"
#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "object/TerrainEdit.h"
#include "terrain/TerrainHoles.h"
#include "voxel/VoxelCoverage.h"
#include "voxel/VoxelSample.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::voxel;

namespace {

    const terrain::MapId HOLE_MAP{terrain::MapKind::Hole, 0};
    const terrain::MapId HEIGHT_MAP{terrain::MapKind::Height, 0};

    // T1's query over copied texels, so a mesh job may read it while the map changes (S53 R93)
    class SnapshotTerrainSource final : public TerrainSource {
    public:
        SnapshotTerrainSource(TerrainComponent& terrain, const Vector3& volumeToTerrain, std::vector<int> layerSlots): offset(volumeToTerrain), slots(std::move(layerSlots)) {
            snapshotTerrainSurface(terrain, snapshot);
            size = snapshot.view.size;
            for (int m = 0; m < static_cast<int>(terrain.blendMaps.size()); m++) {
                TextureData& data = terrain.blendMaps[static_cast<size_t>(m)].getData();
                BlendMap map;
                map.width = data.getWidth();
                map.height = data.getHeight();
                map.channels = data.getChannels();
                if (data.getData() && map.width > 0 && map.height > 0) {
                    const auto* bytes = static_cast<const uint8_t*>(data.getData());
                    map.pixels.assign(bytes, bytes + static_cast<size_t>(map.width) * map.height * map.channels);
                }
                blends.push_back(std::move(map));
            }
            this->layerCount = static_cast<int>(terrain.surfaceLayers.size());
        }

        float height(float x, float z) const override {
            return terrain::sampleHeight(snapshot.view, x + offset.x, z + offset.z) - offset.y;
        }

        void gradient(float x, float z, float& dhdx, float& dhdz) const override {
            const terrain::SurfaceSample sample = terrain::sampleSurface(snapshot.view, x + offset.x, z + offset.z);
            const float ny = sample.normal.y > 0.001f ? sample.normal.y : 0.001f;
            dhdx = -sample.normal.x / ny;
            dhdz = -sample.normal.z / ny;
        }

        TerrainLayerSample layers(float x, float z) const override {
            TerrainLayerSample out;
            int best = -1, second = -1;
            float bestWeight = 0.0f, secondWeight = 0.0f;
            const float u = (x + offset.x + size * 0.5f) / size;
            const float v = (z + offset.z + size * 0.5f) / size;
            for (int m = 0; m < static_cast<int>(blends.size()); m++) {
                const BlendMap& map = blends[static_cast<size_t>(m)];
                if (map.pixels.empty() || map.channels < 3) continue;
                const int tx = std::clamp(static_cast<int>(u * static_cast<float>(map.width)), 0, map.width - 1);
                const int ty = std::clamp(static_cast<int>(v * static_cast<float>(map.height)), 0, map.height - 1);
                const uint8_t* texel = &map.pixels[(static_cast<size_t>(ty) * map.width + tx) * map.channels];
                for (int c = 0; c < 3; c++) {
                    const float weight = static_cast<float>(texel[c]);
                    const int layer = m * 3 + c;
                    if (layer >= layerCount) continue;
                    if (weight > bestWeight) {
                        second = best;
                        secondWeight = bestWeight;
                        best = layer;
                        bestWeight = weight;
                    } else if (weight > secondWeight) {
                        second = layer;
                        secondWeight = weight;
                    }
                }
            }
            const auto slotOf = [this](int layer) {
                return layer >= 0 && layer < static_cast<int>(slots.size()) ? slots[static_cast<size_t>(layer)] : 0;
            };
            out.slot = slotOf(best);
            out.second = slotOf(second);
            const float total = bestWeight + secondWeight;
            out.secondWeight = total > 0.0f ? static_cast<int>(secondWeight / total * 255.0f + 0.5f) : 0;
            return out;
        }

    private:
        struct BlendMap {
            int width = 0, height = 0, channels = 0;
            std::vector<uint8_t> pixels;
        };
        TerrainSurfaceSnapshot snapshot;
        std::vector<BlendMap> blends;
        Vector3 offset;
        std::vector<int> slots;
        float size = 1.0f;
        int layerCount = 0;
    };

    // The volume's corner in the terrain's frame: R1 says an attached volume has no rotation
    Vector3 volumeToTerrain(const Transform& volume, const Transform& terrain) {
        return Vector3(volume.worldPosition.x - terrain.worldPosition.x, volume.worldPosition.y - terrain.worldPosition.y, volume.worldPosition.z - terrain.worldPosition.z);
    }

}

namespace {

    float resolvedOverlap(const VoxelVolumeComponent& component, float voxelSize) {
        return component.overlapDepth >= 0.0f ? component.overlapDepth : 0.02f * voxelSize;
    }

    float resolvedTolerance(const VoxelVolumeComponent& component, float voxelSize) {
        return component.cutTolerance >= 0.0f ? component.cutTolerance : 0.25f * voxelSize;
    }

}

std::shared_ptr<const BaseSource> VoxelSystem::makeTerrainBaseFor(Entity owner, Entity terrainEntity){
    const VolumeState* state = stateOf(owner);
    VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
    TerrainComponent* terrain = terrainEntity != NULL_ENTITY && scene->isEntityCreated(terrainEntity) ? scene->findComponent<TerrainComponent>(terrainEntity) : nullptr;
    const Transform* terrainTransform = terrain ? scene->findComponent<Transform>(terrainEntity) : nullptr;
    const Transform* volumeTransform = scene->findComponent<Transform>(owner);
    if (!state || !state->store || !component || !terrainTransform || !volumeTransform) return nullptr;
    std::vector<int> slots(terrain->surfaceLayers.size(), 0);
    for (size_t i = 0; i < component->materialSlots.size(); i++){
        const int layer = component->materialSlots[i].linkedTerrainLayer;
        if (layer >= 0 && static_cast<size_t>(layer) < slots.size()) slots[static_cast<size_t>(layer)] = static_cast<int>(i);
    }
    TerrainBaseOptions options;
    options.voxelSize = state->store->desc().voxelSize;
    options.overlapDepth = resolvedOverlap(*component, static_cast<float>(options.voxelSize));
    return makeTerrainBase(std::make_shared<SnapshotTerrainSource>(*terrain, volumeToTerrain(*volumeTransform, *terrainTransform), std::move(slots)), options);
}

void VoxelSystem::attachTerrain(VolumeState& state, VoxelVolumeComponent& component){
    const Entity wanted = (component.terrain != NULL_ENTITY && scene->isEntityCreated(component.terrain) && scene->findComponent<TerrainComponent>(component.terrain))
        ? component.terrain : NULL_ENTITY;
    if (wanted != state.attachedTerrain){
        detachTerrain(state, component);
        state.attachedTerrain = wanted;
        if (wanted == NULL_ENTITY) return;
        TerrainComponent& terrain = scene->getComponent<TerrainComponent>(wanted);
        // The map's texels, kept on the CPU: R39 reads them, and T1's query answers from them (T3 R52)
        prepareTerrainHoleMap(terrain);
        const Transform* terrainTransform = scene->findComponent<Transform>(wanted);
        const Transform* volumeTransform = scene->findComponent<Transform>(state.owner);
        if (volumeTransform && terrainTransform && state.asset && state.asset->generator.kind == "terrain"){
            state.store->setBase(makeTerrainBaseFor(state.owner, wanted));
            state.asset->baseResolved = true;
        }
        if (component.terrainCut == VoxelTerrainCut::Automatic && !state.holdingTerrainMap){
            // R40: a first in-game dig must not reload the terrain's variant (T3 R29)
            terrain.holeMapHolds++;
            state.holdingTerrainMap = true;
        }
        // R46: a sculpt under the volume regenerates its unmodified base and re-evaluates the cut.
        // Filtered to our terrain here: the handler only notes the rectangle.
        VolumeState* self = &state;
        state.terrainEdits = TerrainEditEvents::subscribe(scene, [self, wanted](Entity edited, terrain::MapId map, terrain::TexelRect rect){
            if (edited == wanted && map.kind == terrain::MapKind::Height) self->terrainDirty.push_back(rect);
        });
        // The mask starts as the file left it, then the whole footprint is evaluated
        if (state.asset && !state.asset->terrainCut.empty()){
            state.cutMask = state.asset->terrainCut.toMask();
            state.cutWidth = state.asset->terrainCut.width;
            state.cutHeight = state.asset->terrainCut.height;
        }
        IntBox all;
        all.include(Int3{0, 0, 0});
        all.include(Int3{state.store->desc().dimensions.x - 1, state.store->desc().dimensions.y - 1, state.store->desc().dimensions.z - 1});
        updateTerrainCut(state, component, all);
    }
    if (wanted == NULL_ENTITY) return;
    if (component.terrainCut == VoxelTerrainCut::Off && !state.cutMask.empty()){
        // Changing to Off fills exactly what it cut, and holes a person painted stay (R40)
        detachTerrain(state, component);
        state.attachedTerrain = wanted;
    }
    if (!state.terrainDirty.empty()){
        std::vector<terrain::TexelRect> rects;
        rects.swap(state.terrainDirty);
        TerrainComponent& terrain = scene->getComponent<TerrainComponent>(wanted);
        const Transform* terrainTransform = scene->findComponent<Transform>(wanted);
        const Transform* volumeTransform = scene->findComponent<Transform>(state.owner);
        if (terrainTransform && volumeTransform && state.store){
            TerrainComponentStore holes(scene, wanted, terrain);
            const Vector3 offset = volumeToTerrain(*volumeTransform, *terrainTransform);
            const float voxelSize = state.store->desc().voxelSize;
            const Int3 dim = state.store->desc().dimensions;
            IntBox box;
            for (const terrain::TexelRect& rect : rects){
                const Vector2 low = holes.worldOfTexel(HEIGHT_MAP, rect.x, rect.y);
                const Vector2 high = holes.worldOfTexel(HEIGHT_MAP, rect.right(), rect.bottom());
                box.include(Int3{static_cast<int>(std::floor((low.x - offset.x) / voxelSize)) - 1, 0, static_cast<int>(std::floor((low.y - offset.z) / voxelSize)) - 1});
                box.include(Int3{static_cast<int>(std::ceil((high.x - offset.x) / voxelSize)) + 1, dim.y - 1, static_cast<int>(std::ceil((high.y - offset.z) / voxelSize)) + 1});
            }
            box.min = Int3{std::max(box.min.x, 0), 0, std::max(box.min.z, 0)};
            box.max = Int3{std::min(box.max.x, dim.x - 1), dim.y - 1, std::min(box.max.z, dim.z - 1)};
            if (!box.empty()){
                // The base reads a copy of the terrain, taken again for the new heights
                if (state.asset && state.asset->generator.kind == "terrain"){
                    state.store->replaceBase(makeTerrainBaseFor(state.owner, wanted));
                    for (int level = 0; level < state.store->levels(); level++){
                        const int span = BLOCK_SIZE << level;
                        for (int z = box.min.z / span; z <= box.max.z / span; z++){
                            for (int y = 0; y < state.store->blockCount(level).y; y++){
                                for (int x = box.min.x / span; x <= box.max.x / span; x++){
                                    state.store->resetBlockToBase(BlockKey{level, x, y, z});
                                }
                            }
                        }
                    }
                }
                notifyChanged(state.store.get(), box);
            }
        }
    }
    // R44: a hole opens only once the meshes showing the cavity and their collider are live
    if (!state.pendingOpens.empty() && state.scheduler.lastActivatedSet() >= state.pendingOpenSet && state.swaps.empty()){
        TerrainComponent& terrain = scene->getComponent<TerrainComponent>(wanted);
        TerrainComponentStore holes(scene, wanted, terrain);
        const float size = terrain.terrainSize;
        int width = state.cutWidth;
        for (const auto& run : state.pendingOpens){
            terrain::Shape shape;
            shape.type = terrain::ShapeType::Rect;
            const Vector2 first = holes.worldOfTexel(state.holeLattice, run[1], run[0]);
            const Vector2 last = holes.worldOfTexel(state.holeLattice, run[1] + run[2] - 1, run[0]);
            const float halfTexel = size / static_cast<float>(width) * 0.25f;
            shape.min = Vector2(first.x - halfTexel, first.y - halfTexel);
            shape.max = Vector2(last.x + halfTexel, last.y + halfTexel);
            terrain::rasterizeRegion(holes, shape, terrain::HOLE_VALUE, nullptr);
        }
        state.pendingOpens.clear();
    }
}

void VoxelSystem::updateTerrainCut(VolumeState& state, VoxelVolumeComponent& component, const IntBox& changed){
    state.lastHolesCut = 0;
    state.lastHolesFilled = 0;
    state.lastEntranceNarrow = false;
    const Entity owner = state.attachedTerrain;
    if (owner == NULL_ENTITY || !state.store || state.store->desc().bounds != BoundsMode::Open) return;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(owner);
    const Transform* terrainTransform = scene->findComponent<Transform>(owner);
    const Transform* volumeTransform = scene->findComponent<Transform>(state.owner);
    if (!terrainTransform || !volumeTransform) return;

    Store& store = *state.store;
    const float voxelSize = store.desc().voxelSize;
    const Vector3 offset = volumeToTerrain(*volumeTransform, *terrainTransform);
    const float overlap = resolvedOverlap(component, voxelSize);
    const float tolerance = resolvedTolerance(component, voxelSize);
    const Int3 dim = store.desc().dimensions;

    // R39 at a position in the volume's frame: is the volume's surface the terrain's there?
    const auto flushAt = [&](float vx, float vz){
        const terrain::SurfaceSample sample = querySurface(terrain, vx + offset.x, vz + offset.z);
        const float h = sample.height - offset.y;
        const double d = valueAt(store, Vec3d{vx, h, vz});
        const float ny = sample.normal.y > 0.001f ? sample.normal.y : 0.001f;
        const double c = slopeFactor(-sample.normal.x / ny, -sample.normal.z / ny);
        return std::fabs(d - overlap * c) <= tolerance;
    };

    // ---- R41's coverage: a column at a time, over what changed
    if (state.columnFlush.empty()){
        state.columnFlush.assign(static_cast<size_t>(dim.x) * dim.z, 1);
    }
    const int cx0 = std::max(changed.min.x, 0), cx1 = std::min(changed.max.x, dim.x - 1);
    const int cz0 = std::max(changed.min.z, 0), cz1 = std::min(changed.max.z, dim.z - 1);
    for (int z = cz0; z <= cz1; z++){
        for (int x = cx0; x <= cx1; x++){
            state.columnFlush[static_cast<size_t>(x) + static_cast<size_t>(dim.x) * z] = flushAt((static_cast<float>(x) + 0.5f) * voxelSize, (static_cast<float>(z) + 0.5f) * voxelSize) ? 1 : 0;
        }
    }
    auto coverage = std::make_shared<Coverage>();
    coverage->sizeX = dim.x;
    coverage->sizeZ = dim.z;
    coverage->seedHeight.resize(static_cast<size_t>(dim.x) * dim.z);
    for (int z = 0; z < dim.z; z++){
        for (int x = 0; x < dim.x; x++){
            const terrain::SurfaceSample sample = querySurface(terrain, (static_cast<float>(x) + 0.5f) * voxelSize + offset.x, (static_cast<float>(z) + 0.5f) * voxelSize + offset.z);
            coverage->seedHeight[static_cast<size_t>(x) + static_cast<size_t>(dim.x) * z] = (sample.height - offset.y - overlap) / voxelSize;
        }
    }

    // ---- R39-R40: the hole map, texel by texel
    TerrainComponentStore holes(scene, owner, terrain);
    const terrain::MapFormat holeFormat = holes.format(HOLE_MAP);
    const terrain::MapFormat latticeFormat = holeFormat.exists ? holeFormat : holes.format(HEIGHT_MAP);
    const terrain::MapId latticeMap = holeFormat.exists ? HOLE_MAP : HEIGHT_MAP;
    state.holeLattice = latticeMap;
    if (latticeFormat.exists && component.terrainCut == VoxelTerrainCut::Automatic){
        const int width = latticeFormat.bounds.w, height = latticeFormat.bounds.h;
        if (state.cutWidth != width || state.cutHeight != height || state.cutMask.empty()){
            state.cutMask.assign(static_cast<size_t>(width) * height, 0);
            state.cutWidth = width;
            state.cutHeight = height;
        }
        std::vector<uint8_t> holeBytes;
        if (holeFormat.exists){
            holeBytes.resize(static_cast<size_t>(width) * height);
            holes.readBytes(HOLE_MAP, holeFormat.bounds, holeBytes);
        }
        const auto isHoleNow = [&](int tx, int ty){ return !holeBytes.empty() && holeBytes[static_cast<size_t>(ty) * width + tx] < 128; };
        // Only texels inside the changed bounds are evaluated (R40)
        const float metresX0 = changed.min.x * voxelSize + offset.x, metresX1 = (changed.max.x + 1) * voxelSize + offset.x;
        const float metresZ0 = changed.min.z * voxelSize + offset.z, metresZ1 = (changed.max.z + 1) * voxelSize + offset.z;
        const Vector2 lowTexel = holes.texelOfWorld(latticeMap, metresX0, metresZ0);
        const Vector2 highTexel = holes.texelOfWorld(latticeMap, metresX1, metresZ1);
        const int tx0 = std::max(static_cast<int>(std::floor(lowTexel.x)), 0), tx1 = std::min(static_cast<int>(std::ceil(highTexel.x)), width - 1);
        const int ty0 = std::max(static_cast<int>(std::floor(lowTexel.y)), 0), ty1 = std::min(static_cast<int>(std::ceil(highTexel.y)), height - 1);
        std::vector<std::array<int, 3>> fills;
        std::vector<std::array<int, 3>> opens;
        for (int ty = ty0; ty <= ty1; ty++){
            int runStart = -1;
            int fillStart = -1;
            for (int tx = tx0; tx <= tx1 + 1; tx++){
                bool cut = false;
                bool fill = false;
                if (tx <= tx1){
                    const Vector2 local = holes.worldOfTexel(latticeMap, tx, ty);
                    const float vx = local.x - offset.x, vz = local.y - offset.z;
                    const bool inFootprint = vx >= 0.0f && vz >= 0.0f && vx <= dim.x * voxelSize && vz <= dim.z * voxelSize;
                    if (inFootprint){
                        const bool flush = flushAt(vx, vz);
                        uint8_t& mine = state.cutMask[static_cast<size_t>(ty) * width + tx];
                        if (!flush && !mine && !isHoleNow(tx, ty)){
                            mine = 1;
                            cut = true;
                            state.lastHolesCut++;
                        }else if (flush && mine){
                            mine = 0;
                            fill = true;
                            state.lastHolesFilled++;
                        }
                    }
                }
                if (cut && runStart < 0) runStart = tx;
                if (!cut && runStart >= 0){
                    opens.push_back({ty, runStart, tx - runStart});
                    runStart = -1;
                }
                if (fill && fillStart < 0) fillStart = tx;
                if (!fill && fillStart >= 0){
                    fills.push_back({ty, fillStart, tx - fillStart});
                    fillStart = -1;
                }
            }
        }
        // R44: a fill closes the hole first, then the voxels go; a cut waits for the voxels
        for (const auto& run : fills){
            terrain::Shape shape;
            shape.type = terrain::ShapeType::Rect;
            const Vector2 first = holes.worldOfTexel(latticeMap, run[1], run[0]);
            const Vector2 last = holes.worldOfTexel(latticeMap, run[1] + run[2] - 1, run[0]);
            const float halfTexel = terrain.terrainSize / static_cast<float>(width) * 0.25f;
            shape.min = Vector2(first.x - halfTexel, first.y - halfTexel);
            shape.max = Vector2(last.x + halfTexel, last.y + halfTexel);
            terrain::rasterizeRegion(holes, shape, terrain::SOLID_VALUE, nullptr);
        }
        // R40: the terrain's collider keeps a rim of one collision cell inside a drawn hole, so an
        // entrance narrower than 3 cells has nothing to walk through (T3 R22)
        if (!opens.empty()){
            int widest = 0;
            for (const auto& run : opens) widest = std::max(widest, run[2]);
            const float cellMetres = terrain.terrainSize / static_cast<float>(width);
            state.lastEntranceNarrow = static_cast<float>(widest) * (terrain.terrainSize / static_cast<float>(width)) < 3.0f * cellMetres;
        }
        state.pendingOpens.insert(state.pendingOpens.end(), opens.begin(), opens.end());
    }

    // ---- which columns the mesher may drop: flush, and more than the ring from any cut (R41)
    const float holeTexelVoxels = latticeFormat.exists ? terrain.terrainSize / static_cast<float>(latticeFormat.bounds.w) / voxelSize : 1.0f;
    const int ringWidth = std::max(2, static_cast<int>(std::ceil(holeTexelVoxels)));
    buildCoverageMask(*coverage, state.columnFlush, ringWidth);
    state.coverage = coverage;
    state.coverageRing = ringWidth;
}

void VoxelSystem::detachTerrain(VolumeState& state, VoxelVolumeComponent&){
    const Entity owner = state.attachedTerrain;
    state.attachedTerrain = NULL_ENTITY;
    state.terrainEdits = Subscription();
    state.terrainDirty.clear();
    state.pendingOpens.clear();
    state.coverage.reset();
    state.columnFlush.clear();
    if (owner == NULL_ENTITY || !scene->isEntityCreated(owner)) return;
    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(owner);
    if (!terrain) return;
    // R40: fills exactly the texels this volume cut, and holes a person painted stay
    if (!state.cutMask.empty() && state.cutWidth > 0){
        TerrainComponentStore holes(scene, owner, *terrain);
        const TerrainCutRuns runs = TerrainCutRuns::fromMask(state.cutMask.data(), state.cutWidth, state.cutHeight);
        const float halfTexel = terrain->terrainSize / static_cast<float>(state.cutWidth) * 0.25f;
        for (const TerrainCutRuns::Run& run : runs.runs){
            terrain::Shape shape;
            shape.type = terrain::ShapeType::Rect;
            const Vector2 first = holes.worldOfTexel(HOLE_MAP, run.start, run.row);
            const Vector2 last = holes.worldOfTexel(HOLE_MAP, run.start + run.length - 1, run.row);
            shape.min = Vector2(first.x - halfTexel, first.y - halfTexel);
            shape.max = Vector2(last.x + halfTexel, last.y + halfTexel);
            terrain::rasterizeRegion(holes, shape, terrain::SOLID_VALUE, nullptr);
        }
        state.cutMask.clear();
    }
    if (state.holdingTerrainMap){
        terrain->holeMapHolds--;
        state.holdingTerrainMap = false;
    }
}
