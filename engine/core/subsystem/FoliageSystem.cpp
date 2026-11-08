// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "subsystem/FoliageSystem.h"
#include "component/TerrainComponent.h"
#include "memory/MemoryStats.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"
#include "subsystem/FoliagePhysics.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "terrain/FoliageOps.h"
#include "terrain/FoliageRules.h"
#include "terrain/TerrainDeterministic.h"
#include "terrain/TerrainMapStore.h"
#include "util/Fnv.h"

#include "zstd.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    constexpr int FOLIAGE_NO_CELL = INT_MIN;

    // A big .jetrees is zstd: every player links zstd_decompress (TreeStore names no library)
    bool treeFileDecompress(const void* input, size_t inputBytes, void* output, size_t outputBytes) {
        const size_t written = ZSTD_decompress(output, outputBytes, input, inputBytes);
        return !ZSTD_isError(written) && written == outputBytes;
    }

    double foliageNowMs() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int foliageWrap(int value, int side) {
        return ((value % side) + side) % side;
    }

    // R3: in-flight jobs per Terrain
    int foliageJobCap() {
        const Platform platform = Engine::getPlatform();
        if (platform == Platform::Web) return 4;
        if (platform == Platform::Android || platform == Platform::iOS) return 8;
        return 32;
    }

    // R14: tree cell buffers created or updated per frame
    int foliageBufferCap() {
        const Platform platform = Engine::getPlatform();
        return platform == Platform::Web || platform == Platform::Android || platform == Platform::iOS ? 4 : 16;
    }

    FoliageTier foliageTierOf(FoliageQuality quality) {
        switch (quality) {
            case FoliageQuality::LOW: return FoliageTier::Low;
            case FoliageQuality::MEDIUM: return FoliageTier::Medium;
            case FoliageQuality::HIGH: return FoliageTier::High;
        }
        return FoliageTier::High;
    }

    inline uint8_t foliageColorByte(float linear) {
        return static_cast<uint8_t>(std::clamp(std::floor(linear * 255.0f + 0.5f), 0.0f, 255.0f));
    }

    // Offsets of a ring in nearest-first order, the same for every target and camera
    const std::vector<std::pair<int, int>>& foliageRingOrder(int side) {
        static std::map<int, std::vector<std::pair<int, int>>> orders;
        auto it = orders.find(side);
        if (it != orders.end()) return it->second;
        std::vector<std::pair<int, int>> offsets;
        offsets.reserve(static_cast<size_t>(side) * side);
        const int low = -(side / 2);
        for (int z = low; z < low + side; z++) {
            for (int x = low; x < low + side; x++) offsets.emplace_back(x, z);
        }
        std::stable_sort(offsets.begin(), offsets.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
            return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
        });
        return orders.emplace(side, std::move(offsets)).first->second;
    }

    inline int64_t foliageTreeCellKey(int x, int z) {
        return (static_cast<int64_t>(x) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(z));
    }

    // T5 ships this one: one Terrain, one region
    class TerrainFoliageRegion final : public FoliageRegion {
    public:
        TerrainFoliageRegion(Scene* scene, Entity entity): scene(scene), entity(entity) {}

        uint32_t regionKey() const override { return 0; }

        TerrainMapStore& maps() override {
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
            if (!store || component != &terrain) {
                store = std::make_unique<TerrainComponentStore>(scene, entity, terrain);
                component = &terrain;
            }
            return *store;
        }

        const TerrainSurfaceQuery& surface() const override {
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
            if (!query || queryComponent != &terrain) {
                query = std::make_unique<TerrainComponentQuery>(terrain);
                queryComponent = &terrain;
            }
            return *query;
        }

        std::shared_ptr<TreeStore> trees() override { return nullptr; }
        void cellOffset(int& x, int& z) const override { x = 0; z = 0; }

        Matrix4 localToWorld() const override {
            const Transform* transform = scene->findComponent<Transform>(entity);
            return transform ? transform->modelMatrix : Matrix4();
        }

    private:
        Scene* scene;
        Entity entity;
        TerrainComponent* component = nullptr;
        std::unique_ptr<TerrainComponentStore> store;
        mutable TerrainComponent* queryComponent = nullptr;
        mutable std::unique_ptr<TerrainComponentQuery> query;
    };

    // One cell of a job: copies of the texels it reads, and what it leaves
    struct DetailCellJob {
        FoliageCellInput input;
        std::vector<uint8_t> heightTexels;
        std::vector<uint8_t> densityTexels;
        std::shared_ptr<const std::vector<uint8_t>> holeTexels;
        struct Disc { float x, z, radius; };
        std::vector<Disc> discs;

        std::vector<FoliageInstance> result;
        float minY = 0.0f, maxY = 0.0f;

        void run() {
            scatterFoliageCell(input, result);
            if (!discs.empty()) {
                result.erase(std::remove_if(result.begin(), result.end(), [&](const FoliageInstance& instance) {
                    for (const Disc& disc : discs) {
                        const float dx = instance.position[0] - disc.x;
                        const float dz = instance.position[2] - disc.z;
                        if (dx * dx + dz * dz <= disc.radius * disc.radius) return true;
                    }
                    return false;
                }), result.end());
            }
            minY = 1e30f;
            maxY = -1e30f;
            for (const FoliageInstance& instance : result) {
                minY = std::min(minY, instance.position[1]);
                maxY = std::max(maxY, instance.position[1]);
            }
        }
    };

    struct DetailSlot {
        int x = FOLIAGE_NO_CELL, z = FOLIAGE_NO_CELL;
        bool assigned = false;
        float minY = 0.0f, maxY = 0.0f;
        std::shared_ptr<const std::vector<FoliageInstance>> instances;
        std::shared_ptr<DetailCellJob> job;
        jobs::Task<void> task;
    };

    struct DetailLayerState {
        Entity entity = NULL_ENTITY;
        uint32_t slot = 0;
        std::string loadedPath;
        std::string requestedPath;
        bool meshReady = false;
        bool loadFailed = false;
        Vector3 center;
        float radius = 0.0f;
        std::vector<DetailSlot> ring;
        int side = 0;
        int viewCellX = FOLIAGE_NO_CELL, viewCellZ = FOLIAGE_NO_CELL;
        bool needScan = true;
        bool waitingForMap = false;
        std::vector<int> pending;   // slots with a job
        FoliageLayerParams params;
        float distance = 0.0f;
        bool boundsDirty = true;
        uint64_t resident = 0;
    };

    struct TreeProtoRange {
        int prototype = 0;
        uint32_t first = 0;
        uint32_t count = 0;
        float maxScale = 1.0f;
    };

    struct TreeCell {
        int x = 0, z = 0;
        std::vector<uint32_t> ids;
        bool resident = false;
        bool cpuDirty = true;
        std::vector<FoliageInstance> instances;
        std::vector<uint32_t> slotIds;
        std::vector<TreeProtoRange> ranges;
        float boxMin[3] = {0.0f, 0.0f, 0.0f};
        float boxMax[3] = {0.0f, 0.0f, 0.0f};
        std::shared_ptr<ExternalBuffer> gpu;
        size_t gpuCapacity = 0;
        bool gpuDirty = false;
        bool wantsGpu = false;
        bool bodyDirty = true;
        foliagephysics::CellBody body;
    };

    using TreeCellMap = std::unordered_map<int64_t, TreeCell>;

    struct TreePrototypeState {
        uint32_t id = 0;
        // LOD 0, and any explicit LODs
        std::vector<Entity> lods;
        std::vector<uint32_t> slots;
        std::vector<std::string> paths;
        std::vector<char> ready;
        std::vector<char> failed;
        Vector3 center;
        float radius = 0.0f;
        Entity impostor = NULL_ENTITY;
        uint32_t impostorSlot = 0;
        bool impostorReady = false;
        std::string impostorAlbedo;
        std::string impostorNormal;
    };

    struct TreeRuntimeState {
        std::unordered_set<uint32_t> removed;
        std::vector<TreeRecord> added;
        uint32_t nextAdded = TREE_RUNTIME_ID_BASE;
        struct Disc { float x, z, radius; int layer; };
        std::vector<Disc> discs;
    };

    struct RegionState {
        Entity owner = NULL_ENTITY;
        std::shared_ptr<FoliageRegion> region;
        std::vector<DetailLayerState> layers;
        std::vector<TreePrototypeState> prototypes;

        std::shared_ptr<TreeStore> store;
        bool storeShared = false;       // a clone's: copied before its first write
        bool storeDirty = false;
        bool storeRefused = false;
        bool storeLoading = false;
        bool storeOverwriteAllowed = false;
        std::string storeError;
        std::string storePath;
        jobs::Task<std::shared_ptr<TreeStore>> storeTask;
        std::shared_ptr<std::string> storeTaskError;

        TreeCellMap cells;
        bool cellsReady = false;
        jobs::Task<std::shared_ptr<TreeCellMap>> indexTask;
        std::shared_ptr<TreeStore> indexing;
        std::vector<int64_t> residentOrder;     // resident cells, for the views
        bool residentDirty = true;
        int treeViewCellX = FOLIAGE_NO_CELL, treeViewCellZ = FOLIAGE_NO_CELL;
        float residencyDistance = -1.0f;

        TreeRuntimeState runtime;

        std::shared_ptr<const std::vector<uint8_t>> holeCopy;
        bool holeCopyValid = false;

        // what edits left, in terrain-local units, until the debounce lets them through
        bool heightDirty = false;
        float heightDirtyMin[2] = {0.0f, 0.0f};
        float heightDirtyMax[2] = {0.0f, 0.0f};
        float heightDirtyAge = 0.0f;
        std::vector<std::pair<int, TexelRect>> densityDirty;

        bool refill = true;
        bool active = true;
        bool hadBodies = false;

        std::function<void(const TreeHitEvent&)> hitHandler;
        std::function<void(TreeId, const FoliageInstanceInfo&)> removedHandler;
        Vector3 wind;
        bool windActive = false;

        // the instance space this frame
        Matrix4 model;
        Matrix4 inverseModel;
        float terrainScale = 1.0f;
        Vector3 viewLocal;
        float terrainSize = 0.0f;
        float terrainMaxHeight = 0.0f;

        FoliageStats stats;
        Subscription events;
    };

    struct PrototypeRef {
        RegionState* region = nullptr;
        int prototype = -1;
        int layer = -1;
        int lod = 0;
        bool impostor = false;
    };

}

struct FoliageSystem::Impl {
    FoliageSystem* self;
    Scene* scene;
    Entity previewEntity = NULL_ENTITY;
    std::vector<std::unique_ptr<RegionState>> regions;
    std::vector<std::unique_ptr<RegionState>> dying;
    std::unordered_map<Entity, PrototypeRef> prototypeEntities;
    std::vector<uint32_t> freeSlots;
    // a cell body's entity: its terrain, for a hit
    std::unordered_map<Entity, std::pair<Entity, const std::vector<uint32_t>*>> bodyCells;
    double clock = 0.0;
    Subscription contactSubscription;

    explicit Impl(FoliageSystem* self, Scene* scene): self(self), scene(scene) {}

    RegionState* regionOf(Entity owner, uint32_t key = 0) const {
        for (const std::unique_ptr<RegionState>& region : regions) {
            if (region->owner == owner && region->region->regionKey() == key) return region.get();
        }
        return nullptr;
    }

    uint32_t takeSlot() {
        if (!freeSlots.empty()) {
            const uint32_t slot = freeSlots.back();
            freeSlots.pop_back();
            return slot;
        }
        return self->nextSlot++;
    }

    Entity createPrototypeEntity(RegionState& state, uint32_t& slot, PrototypeRef ref) {
        Entity entity = scene->createEntity();
        scene->addComponent<Transform>(entity);
        scene->addComponent<MeshComponent>(entity);
        scene->addComponent<ModelComponent>(entity);
        scene->addComponent<InstancedMeshComponent>(entity);

        // A merged root owns all the geometry, so one instanced draw covers an .obj and a glTF alike.
        scene->getComponent<ModelComponent>(entity).mergeStaticMeshes = true;
        InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(entity);
        slot = takeSlot();
        instmesh.foliage = true;
        instmesh.foliageSlot = slot;
        instmesh.needUpdateInstances = false;
        instmesh.maxInstances = 1;
        instmesh.cullInstances = false;
        setCompactLayout(instmesh);

        ref.region = &state;
        prototypeEntities[entity] = ref;
        return entity;
    }

    static void setCompactLayout(InstancedMeshComponent& instmesh) {
        ExternalBuffer& buffer = instmesh.buffer;
        buffer.clear();
        const auto add = [&](AttributeType type, AttributeDataType dataType, size_t offset, bool normalized) {
            Attribute attribute(dataType, buffer.getName(), 4, offset, 0, normalized, true);
            buffer.addAttribute(type, attribute);
        };
        add(AttributeType::INSTANCEPACKED0, AttributeDataType::FLOAT, offsetof(FoliageInstance, position), false);
        add(AttributeType::INSTANCEPACKED1, AttributeDataType::SHORT, offsetof(FoliageInstance, rotation), true);
        add(AttributeType::INSTANCEPACKED2, AttributeDataType::UNSIGNED_BYTE, offsetof(FoliageInstance, color), true);
        add(AttributeType::INSTANCEPACKED3, AttributeDataType::UNSIGNED_BYTE, offsetof(FoliageInstance, heightRatio), true);
        buffer.setStride(sizeof(FoliageInstance));
        buffer.setRenderAttributes(true);
        buffer.setInstanceBuffer(true);
        buffer.setUsage(BufferUsage::STREAM);
    }

    static void setExpandedLayout(InstancedMeshComponent& instmesh) {
        ExternalBuffer& buffer = instmesh.buffer;
        buffer.clear();
        buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL1, 4, 0, true);
        buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL2, 4, 4 * sizeof(float), true);
        buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL3, 4, 8 * sizeof(float), true);
        buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL4, 4, 12 * sizeof(float), true);
        buffer.addAttribute(AttributeType::INSTANCECOLOR, 4, 16 * sizeof(float), true);
        buffer.addAttribute(AttributeType::INSTANCETEXTURERECT, 4, 20 * sizeof(float), true);
        buffer.setStride(24 * sizeof(float));
    }

    void destroyPrototypeEntity(Entity& entity, uint32_t slot) {
        if (entity != NULL_ENTITY) {
            prototypeEntities.erase(entity);
            freeSlots.push_back(slot);
            if (scene->isEntityCreated(entity)) scene->destroyEntity(entity);
        }
        entity = NULL_ENTITY;
    }

    void cancelSlot(DetailSlot& slot) {
        if (slot.job) {
            slot.task.cancel();
            slot.job.reset();
            slot.task = jobs::Task<void>();
        }
    }

    void destroyLayer(DetailLayerState& layer) {
        for (DetailSlot& slot : layer.ring) cancelSlot(slot);
        destroyPrototypeEntity(layer.entity, layer.slot);
        layer = DetailLayerState();
    }

    void destroyPrototype(TreePrototypeState& prototype) {
        for (size_t i = 0; i < prototype.lods.size(); i++) destroyPrototypeEntity(prototype.lods[i], prototype.slots[i]);
        destroyPrototypeEntity(prototype.impostor, prototype.impostorSlot);
        prototype = TreePrototypeState();
    }

    void releaseCell(TreeCell& cell) {
        if (cell.gpu) {
            cell.gpu->getRender()->destroyBuffer();
            cell.gpu.reset();
            cell.gpuCapacity = 0;
        }
        if (cell.body.entity != NULL_ENTITY) {
            bodyCells.erase(cell.body.entity);
            foliagephysics::destroyCellBody(*scene, cell.body);
        }
    }

    void destroyRegion(RegionState& state) {
        for (DetailLayerState& layer : state.layers) destroyLayer(layer);
        state.layers.clear();
        for (TreePrototypeState& prototype : state.prototypes) destroyPrototype(prototype);
        state.prototypes.clear();
        if (state.storeLoading) state.storeTask.cancel();
        state.indexTask.cancel();
        for (auto& entry : state.cells) releaseCell(entry.second);
        state.cells.clear();
    }

    // -------------------------------------------------------------------------------- settings

    static float scaledDistance(float distance, bool preview) {
        return preview ? distance : distance * Engine::getFoliageDistanceScale();
    }

    // R20: min(drawDistance, detailDistance), ignoring zero, times Foliage Distance
    static float effectiveDetailDistance(const TerrainComponent& terrain, const TerrainFoliageLayer& layer, bool preview) {
        float distance = std::max(0.0f, layer.drawDistance);
        if (terrain.detailDistance > 0.0f) {
            distance = distance > 0.0f ? std::min(distance, terrain.detailDistance) : terrain.detailDistance;
        }
        return std::max(0.0f, scaledDistance(distance, preview));
    }

    static float treeDistanceOf(const TerrainComponent& terrain) {
        return scaledDistance(std::max(0.0f, terrain.treeDistance), false);
    }

    static FoliageLayerParams paramsOf(const TerrainFoliageLayer& layer) {
        FoliageLayerParams params;
        params.seed = layer.seed;
        params.density = layer.density;
        params.minScale = layer.minScale;
        params.maxScale = layer.maxScale;
        params.rotationJitter = layer.rotationJitter;
        params.alignToNormal = layer.alignToNormal;
        params.minSlope = layer.minSlope;
        params.maxSlope = layer.maxSlope;
        params.minHeight = layer.minHeight;
        params.maxHeight = layer.maxHeight;
        params.minHeightRatio = layer.minHeightRatio;
        params.maxHeightRatio = layer.maxHeightRatio;
        for (int c = 0; c < 4; c++) {
            params.healthyColor[c] = foliageColorByte(layer.healthyColor[c]);
            params.dryColor[c] = foliageColorByte(layer.dryColor[c]);
        }
        params.noiseSpread = layer.noiseSpread;
        params.windBend = static_cast<uint8_t>(std::clamp(std::floor(layer.windBend * 255.0f + 0.5f), 0.0f, 255.0f));
        params.algorithmVersion = layer.algorithmVersion;
        return params;
    }

    static bool sameParams(const FoliageLayerParams& a, const FoliageLayerParams& b) {
        return a.seed == b.seed && a.density == b.density && a.minScale == b.minScale && a.maxScale == b.maxScale &&
               a.rotationJitter == b.rotationJitter && a.alignToNormal == b.alignToNormal && a.minSlope == b.minSlope &&
               a.maxSlope == b.maxSlope && a.minHeight == b.minHeight && a.maxHeight == b.maxHeight &&
               a.minHeightRatio == b.minHeightRatio && a.maxHeightRatio == b.maxHeightRatio &&
               std::equal(a.healthyColor, a.healthyColor + 4, b.healthyColor) && std::equal(a.dryColor, a.dryColor + 4, b.dryColor) &&
               a.noiseSpread == b.noiseSpread && a.windBend == b.windBend && a.algorithmVersion == b.algorithmVersion;
    }

    // -------------------------------------------------------------------------------- meshes

    bool loadPrototypeMesh(Entity entity, const std::string& path) {
        return scene->getSystem<MeshSystem>()->loadFoliageMesh(entity, path);
    }

    // R6: one mesh load per layer, on the Background path; true once it is drawn
    void pollMesh(Entity entity, const std::string& path, std::string& requested, bool& ready, bool& failed, Vector3& center, float& radius, uint32_t& meshLoads) {
        if (ready || failed) return;

        if (requested != path) {
            requested = path;
            meshLoads++;
        }
        if (!loadPrototypeMesh(entity, path)) {
            if (!scene->getSystem<MeshSystem>()->isAsyncModelLoadPending(entity, path)) {
                failed = true;
                Log::error("Foliage mesh could not be loaded: %s", path.c_str());
            }
            return;
        }

        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        if (mesh.numSubmeshes == 0) {
            Log::error("Foliage mesh has no geometry on its root and cannot be instanced: %s", path.c_str());
            failed = true;
            return;
        }

        mesh.needReload = true;
        if (!mesh.aabb.isNull() && !mesh.aabb.isInfinite()) {
            center = mesh.aabb.getCenter();
            radius = mesh.aabb.getHalfSize().length();
        }
        ready = true;
    }

    bool programNeedsExpanded(Entity entity) {
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        for (unsigned int i = 0; i < mesh.numSubmeshes; i++) {
            const std::string& shader = RenderSystem::getSubmeshShader(mesh, mesh.submeshes[i], scene->getDefaultCustomShader(ShaderType::MESH));
            if (!RenderSystem::shaderSupportsCompactInstances(shader)) return true;
        }
        return false;
    }

    // -------------------------------------------------------------------------------- details

    void ensureHoleCopy(RegionState& state) {
        if (state.holeCopyValid) return;
        state.holeCopyValid = true;
        state.holeCopy.reset();
        TerrainMapStore& maps = state.region->maps();
        const MapId holes{MapKind::Hole, 0};
        const MapFormat format = maps.format(holes);
        if (!format.exists || format.bounds.empty()) return;
        auto copy = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(format.bounds.area()) * bytesPerTexel(format));
        if (maps.readBytes(holes, format.bounds, *copy)) {
            state.holeCopy = std::move(copy);
        }
    }

    bool fillCellInput(RegionState& state, DetailLayerState& layerState, int layerIndex, int cellX, int cellZ, DetailCellJob& job) {
        TerrainMapStore& maps = state.region->maps();
        const MapId heightId{MapKind::Height, 0};
        const MapId densityId{MapKind::Density, layerIndex};
        const MapFormat heightFormat = maps.format(heightId);
        const MapFormat densityFormat = maps.format(densityId);
        if (!heightFormat.exists || !densityFormat.exists) return false;

        const float size = state.terrainSize;
        FoliageCellInput& input = job.input;
        input.layer = layerState.params;
        input.cellX = cellX;
        input.cellZ = cellZ;
        state.region->cellOffset(input.regionOffsetX, input.regionOffsetZ);
        input.terrainSize = size;
        input.terrainMaxHeight = state.terrainMaxHeight;

        const FoliagePatchRect heightRect = foliageHeightPatchRect(cellX, cellZ, size, heightFormat.bounds.w, heightFormat.bounds.h);
        job.heightTexels.resize(static_cast<size_t>(heightRect.width) * heightRect.height * bytesPerTexel(heightFormat));
        if (!maps.readBytes(heightId, TexelRect{heightRect.x, heightRect.y, heightRect.width, heightRect.height}, job.heightTexels)) return false;
        input.heightMap.texels = job.heightTexels.data();
        input.heightMap.mapWidth = heightFormat.bounds.w;
        input.heightMap.mapHeight = heightFormat.bounds.h;
        input.heightMap.originX = heightRect.x;
        input.heightMap.originY = heightRect.y;
        input.heightMap.width = heightRect.width;
        input.heightMap.height = heightRect.height;
        input.heightMap.channels = heightFormat.channels;
        input.heightMap.bytesPerChannel = heightFormat.bitsPerChannel >= 16 ? 2 : 1;

        const FoliagePatchRect densityRect = foliageDensityPatchRect(cellX, cellZ, size, densityFormat.bounds.w, densityFormat.bounds.h);
        job.densityTexels.resize(static_cast<size_t>(densityRect.width) * densityRect.height * bytesPerTexel(densityFormat));
        if (!maps.readBytes(densityId, TexelRect{densityRect.x, densityRect.y, densityRect.width, densityRect.height}, job.densityTexels)) return false;
        input.densityMap.texels = job.densityTexels.data();
        input.densityMap.mapWidth = densityFormat.bounds.w;
        input.densityMap.mapHeight = densityFormat.bounds.h;
        input.densityMap.originX = densityRect.x;
        input.densityMap.originY = densityRect.y;
        input.densityMap.width = densityRect.width;
        input.densityMap.height = densityRect.height;
        input.densityMap.channels = densityFormat.channels;
        input.densityMap.bytesPerChannel = densityFormat.bitsPerChannel >= 16 ? 2 : 1;

        ensureHoleCopy(state);
        if (state.holeCopy) {
            job.holeTexels = state.holeCopy;
            const MapFormat holes = maps.format(MapId{MapKind::Hole, 0});
            input.holes = HoleMapView{state.holeCopy->data(), holes.bounds.w, holes.bounds.h};
        }

        const float originX = static_cast<float>(cellX) * FOLIAGE_DETAIL_CELL_SIZE;
        const float originZ = static_cast<float>(cellZ) * FOLIAGE_DETAIL_CELL_SIZE;
        for (const TreeRuntimeState::Disc& disc : state.runtime.discs) {
            if (disc.layer >= 0 && disc.layer != layerIndex) continue;
            const float nearestX = std::clamp(disc.x, originX, originX + FOLIAGE_DETAIL_CELL_SIZE);
            const float nearestZ = std::clamp(disc.z, originZ, originZ + FOLIAGE_DETAIL_CELL_SIZE);
            const float dx = disc.x - nearestX;
            const float dz = disc.z - nearestZ;
            if (dx * dx + dz * dz <= disc.radius * disc.radius) {
                job.discs.push_back(DetailCellJob::Disc{disc.x, disc.z, disc.radius});
            }
        }
        return true;
    }

    void applyJob(RegionState& state, DetailLayerState& layer, DetailSlot& slot) {
        std::shared_ptr<DetailCellJob> job = slot.job;
        if (slot.instances) {
            layer.resident -= slot.instances->size();
        }
        slot.instances = std::make_shared<const std::vector<FoliageInstance>>(std::move(job->result));
        layer.resident += slot.instances->size();
        slot.minY = job->minY;
        slot.maxY = job->maxY;
        slot.job.reset();
        slot.task = jobs::Task<void>();
        layer.boundsDirty = true;
        state.stats.detailCellsPlaced++;
        state.stats.applied++;
        self->residencyRevision++;
    }

    // Polls and submits within the budget (R3, R5). Returns false when out of budget.
    bool updateDetailLayer(RegionState& state, DetailLayerState& layerState, int layerIndex, bool preview, double startMs, int& inFlight) {
        for (size_t i = 0; i < layerState.pending.size();) {
            DetailSlot& slot = layerState.ring[layerState.pending[i]];
            if (!slot.job) {
                layerState.pending[i] = layerState.pending.back();
                layerState.pending.pop_back();
                continue;
            }
            if (!slot.task.isReady()) {
                i++;
                continue;
            }
            if (foliageNowMs() - startMs > APPLY_BUDGET_MS) return false;
            applyJob(state, layerState, slot);
            layerState.pending[i] = layerState.pending.back();
            layerState.pending.pop_back();
        }
        inFlight += static_cast<int>(layerState.pending.size());

        // R66: the side comes from the draw distance, never the view span, so walking fills only
        // the cells that enter, and a slot outside the span stays as it is
        const float distance = layerState.distance;
        const int side = preview
            ? std::max(1, static_cast<int>(std::ceil(state.terrainSize / FOLIAGE_DETAIL_CELL_SIZE)) + 1)
            : std::max(1, static_cast<int>(std::ceil(2.0f * distance / FOLIAGE_DETAIL_CELL_SIZE)) + 1);
        if (side != layerState.side) {
            for (DetailSlot& slot : layerState.ring) cancelSlot(slot);
            layerState.pending.clear();
            layerState.ring.clear();
            layerState.ring.resize(static_cast<size_t>(side) * side);
            layerState.side = side;
            layerState.resident = 0;
            layerState.needScan = true;
            layerState.boundsDirty = true;
            self->residencyRevision++;
        }

        const int viewCellX = preview ? 0 : static_cast<int>(std::floor(state.viewLocal.x / FOLIAGE_DETAIL_CELL_SIZE));
        const int viewCellZ = preview ? 0 : static_cast<int>(std::floor(state.viewLocal.z / FOLIAGE_DETAIL_CELL_SIZE));
        if (viewCellX != layerState.viewCellX || viewCellZ != layerState.viewCellZ) {
            layerState.viewCellX = viewCellX;
            layerState.viewCellZ = viewCellZ;
            layerState.needScan = true;
        }
        if (!layerState.needScan) return true;

        const int jobCap = foliageJobCap();
        const float half = state.terrainSize * 0.5f;
        const int lastCell = static_cast<int>(std::ceil(half / FOLIAGE_DETAIL_CELL_SIZE));
        bool complete = true;
        for (const std::pair<int, int>& offset : foliageRingOrder(side)) {
            const int cellX = viewCellX + offset.first;
            const int cellZ = viewCellZ + offset.second;
            if (cellX < -lastCell || cellX >= lastCell || cellZ < -lastCell || cellZ >= lastCell) continue;

            const int index = foliageWrap(cellZ, side) * side + foliageWrap(cellX, side);
            DetailSlot& slot = layerState.ring[index];
            if (slot.assigned && slot.x == cellX && slot.z == cellZ) continue;

            if (preview && state.stats.detailResident > static_cast<uint64_t>(MAX_PREVIEW_INSTANCES)) continue;
            if (inFlight >= jobCap || foliageNowMs() - startMs > APPLY_BUDGET_MS) {
                complete = false;
                break;
            }

            cancelSlot(slot);
            auto job = std::make_shared<DetailCellJob>();
            if (!fillCellInput(state, layerState, layerIndex, cellX, cellZ, *job)) {
                complete = false;
                break;
            }
            slot.x = cellX;
            slot.z = cellZ;
            slot.assigned = true;
            slot.job = job;
            if (jobs::hasSystem()) {
                DetailCellJob* raw = job.get();
                slot.task = jobs::submit([job, raw]() { raw->run(); }, {jobs::Lane::Background, jobs::GateClass::Default, "foliage-detail-cell"});
                layerState.pending.push_back(index);
                inFlight++;
            } else {
                job->run();
                applyJob(state, layerState, slot);
            }
        }
        layerState.needScan = !complete;
        return complete;
    }

    void markDetailRectDirty(DetailLayerState& layer, float minX, float minZ, float maxX, float maxZ) {
        for (DetailSlot& slot : layer.ring) {
            if (!slot.assigned) continue;
            const float originX = static_cast<float>(slot.x) * FOLIAGE_DETAIL_CELL_SIZE;
            const float originZ = static_cast<float>(slot.z) * FOLIAGE_DETAIL_CELL_SIZE;
            if (originX > maxX || originX + FOLIAGE_DETAIL_CELL_SIZE < minX || originZ > maxZ || originZ + FOLIAGE_DETAIL_CELL_SIZE < minZ) continue;
            cancelSlot(slot);
            slot.assigned = false;
            layer.needScan = true;
        }
    }

    void updateLayerBounds(DetailLayerState& layer) {
        if (!layer.boundsDirty || layer.entity == NULL_ENTITY || !layer.meshReady) return;
        layer.boundsDirty = false;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(layer.entity);
        AABB box;
        box.setNull();
        const float reach = (layer.radius + layer.center.length()) * std::max(layer.params.maxScale, 0.001f) * std::max(1.0f, layer.params.maxHeightRatio);
        for (const DetailSlot& slot : layer.ring) {
            if (!slot.instances || slot.instances->empty()) continue;
            const float x = static_cast<float>(slot.x) * FOLIAGE_DETAIL_CELL_SIZE;
            const float z = static_cast<float>(slot.z) * FOLIAGE_DETAIL_CELL_SIZE;
            box.merge(AABB(Vector3(x - reach, slot.minY - reach, z - reach),
                           Vector3(x + FOLIAGE_DETAIL_CELL_SIZE + reach, slot.maxY + reach, z + FOLIAGE_DETAIL_CELL_SIZE + reach)));
        }
        mesh.aabb = box;
        mesh.needUpdateAABB = true;
        // S10's shadow cache: what it casts changed
        mesh.shadowCasterRevision++;
    }

    void syncPrototypeTransform(RegionState& state, Entity entity) {
        const Transform& terrainTransform = scene->getComponent<Transform>(state.owner);
        Transform& transform = scene->getComponent<Transform>(entity);
        if (transform.position != terrainTransform.worldPosition ||
            transform.rotation != terrainTransform.worldRotation ||
            transform.scale != terrainTransform.worldScale) {
            transform.position = terrainTransform.worldPosition;
            transform.rotation = terrainTransform.worldRotation;
            transform.scale = terrainTransform.worldScale;
            transform.needUpdate = true;
        }
    }

    // -------------------------------------------------------------------------------- trees

    void startStoreLoad(RegionState& state, const std::string& path) {
        state.storeLoading = true;
        state.storeRefused = false;
        state.storeOverwriteAllowed = false;
        state.storeError.clear();
        auto error = std::make_shared<std::string>();
        state.storeTaskError = error;

        const auto decode = [error](std::vector<uint8_t>&& bytes) -> std::shared_ptr<TreeStore> {
            auto store = std::make_shared<TreeStore>();
            std::string message;
            if (!store->decode(bytes, &message, treeFileDecompress)) {
                *error = message;
                return nullptr;
            }
            return store;
        };

        AssetIOService::ReadRequest request;
        request.path = path;
        request.priority = IOPriority::Normal;
        request.name = "foliage-tree-file";
        state.storeTask = AssetIOService::instance().submitRead(request, decode);
    }

    void pollStoreLoad(RegionState& state) {
        if (!state.storeLoading || !state.storeTask.isReady()) return;
        state.storeLoading = false;
        std::shared_ptr<TreeStore> loaded;
        if (state.storeTask.status() == jobs::TaskStatus::Completed) loaded = state.storeTask.get();
        state.storeTask = jobs::Task<std::shared_ptr<TreeStore>>();
        if (!loaded) {
            state.storeRefused = true;
            state.storeError = state.storeTaskError && !state.storeTaskError->empty() ? *state.storeTaskError : "the file could not be read";
            Log::error("Tree data %s was refused: %s. The Terrain draws no trees, and this file will not be overwritten by a save.", state.storePath.c_str(), state.storeError.c_str());
            state.store.reset();
            for (auto& entry : state.cells) releaseCell(entry.second);
            state.cells.clear();
            state.cellsReady = true;
            state.residentDirty = true;
            return;
        }
        state.store = std::move(loaded);
        state.storeShared = false;
        startIndex(state);
    }

    static void addToIndex(TreeCellMap& cells, const TreeRecord& record, float size) {
        const int x = static_cast<int>(std::floor(foliageTreeLocalX(record, size) / FOLIAGE_TREE_CELL_SIZE));
        const int z = static_cast<int>(std::floor(foliageTreeLocalZ(record, size) / FOLIAGE_TREE_CELL_SIZE));
        TreeCell& cell = cells[foliageTreeCellKey(x, z)];
        cell.x = x;
        cell.z = z;
        cell.ids.push_back(record.id);
    }

    // F9: cells are derived at load on a job
    void startIndex(RegionState& state) {
        state.cellsReady = false;
        for (auto& entry : state.cells) releaseCell(entry.second);
        state.cells.clear();
        state.residentDirty = true;
        std::shared_ptr<TreeStore> store = state.store;
        state.indexing = store;
        const float size = state.terrainSize;
        std::vector<TreeRecord> added = state.runtime.added;
        const auto build = [store, size, added]() {
            auto cells = std::make_shared<TreeCellMap>();
            if (store) {
                for (const TreeRecord& record : store->records()) addToIndex(*cells, record, size);
            }
            for (const TreeRecord& record : added) addToIndex(*cells, record, size);
            return cells;
        };
        if (jobs::hasSystem()) {
            state.indexTask = jobs::submit(build, {jobs::Lane::Background, jobs::GateClass::Default, "foliage-tree-index"});
        } else {
            std::shared_ptr<TreeCellMap> cells = build();
            state.cells = std::move(*cells);
            state.cellsReady = true;
        }
    }

    void pollIndex(RegionState& state) {
        if (state.cellsReady || !state.indexTask.isReady()) return;
        if (state.indexTask.status() != jobs::TaskStatus::Completed) {
            state.indexTask = jobs::Task<std::shared_ptr<TreeCellMap>>();
            return;
        }
        std::shared_ptr<TreeCellMap> cells = state.indexTask.get();
        state.indexTask = jobs::Task<std::shared_ptr<TreeCellMap>>();
        if (state.indexing != state.store) {
            startIndex(state);
            return;
        }
        state.cells = std::move(*cells);
        state.cellsReady = true;
        state.residentDirty = true;
        state.treeViewCellX = FOLIAGE_NO_CELL;
    }

    const TreeRecord* findRecord(const RegionState& state, uint32_t id) const {
        if (id >= TREE_RUNTIME_ID_BASE) {
            for (const TreeRecord& record : state.runtime.added) {
                if (record.id == id) return &record;
            }
            return nullptr;
        }
        return state.store ? state.store->find(id) : nullptr;
    }

    static int prototypeIndexOf(const TerrainComponent& terrain, uint16_t id) {
        for (size_t i = 0; i < terrain.treePrototypes.size(); i++) {
            if (terrain.treePrototypes[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }

    // A cell's instance array, sorted by prototype: from store plus overlay (F10)
    void buildCell(RegionState& state, TreeCell& cell, const TerrainComponent& terrain) {
        cell.cpuDirty = false;
        cell.instances.clear();
        cell.slotIds.clear();
        cell.ranges.clear();

        struct Entry { int prototype; uint32_t id; };
        std::vector<Entry> entries;
        entries.reserve(cell.ids.size());
        for (uint32_t id : cell.ids) {
            if (state.runtime.removed.count(id)) continue;
            const TreeRecord* record = findRecord(state, id);
            if (!record || (record->flags & TREE_HIDDEN_BY_HOLE)) continue;
            const int prototype = prototypeIndexOf(terrain, record->prototypeId);
            if (prototype < 0) continue;
            entries.push_back(Entry{prototype, id});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.prototype != b.prototype ? a.prototype < b.prototype : a.id < b.id;
        });

        const float size = state.terrainSize;
        cell.boxMin[0] = static_cast<float>(cell.x) * FOLIAGE_TREE_CELL_SIZE;
        cell.boxMin[2] = static_cast<float>(cell.z) * FOLIAGE_TREE_CELL_SIZE;
        cell.boxMax[0] = cell.boxMin[0] + FOLIAGE_TREE_CELL_SIZE;
        cell.boxMax[2] = cell.boxMin[2] + FOLIAGE_TREE_CELL_SIZE;
        float minY = 1e30f, maxY = -1e30f;

        cell.instances.resize(entries.size());
        cell.slotIds.resize(entries.size());
        for (size_t i = 0; i < entries.size(); i++) {
            const TreeRecord* record = findRecord(state, entries[i].id);
            const TerrainTreePrototype& prototype = terrain.treePrototypes[entries[i].prototype];
            float normal[3] = {0.0f, 1.0f, 0.0f};
            const bool leans = (record->flags & TREE_ALIGN_TO_NORMAL) != 0;
            if (leans) {
                const SurfaceSample sample = state.region->surface().sample(foliageTreeLocalX(*record, size), foliageTreeLocalZ(*record, size));
                normal[0] = sample.normal.x;
                normal[1] = sample.normal.y;
                normal[2] = sample.normal.z;
            }
            const uint8_t bend = static_cast<uint8_t>(std::clamp(std::floor(prototype.bendFactor * 255.0f + 0.5f), 0.0f, 255.0f));
            foliageInstanceFromTree(*record, size, state.terrainMaxHeight, bend, leans ? normal : nullptr, cell.instances[i]);
            cell.slotIds[i] = entries[i].id;
            minY = std::min(minY, cell.instances[i].position[1]);
            maxY = std::max(maxY, cell.instances[i].position[1]);

            if (cell.ranges.empty() || cell.ranges.back().prototype != entries[i].prototype) {
                TreeProtoRange range;
                range.prototype = entries[i].prototype;
                range.first = static_cast<uint32_t>(i);
                range.maxScale = 0.0f;
                cell.ranges.push_back(range);
            }
            TreeProtoRange& range = cell.ranges.back();
            range.count++;
            range.maxScale = std::max(range.maxScale, cell.instances[i].scale * std::max(1.0f, static_cast<float>(cell.instances[i].heightRatio) / 128.0f));
        }
        if (entries.empty()) {
            minY = 0.0f;
            maxY = 0.0f;
        }
        cell.boxMin[1] = minY;
        cell.boxMax[1] = maxY;
        cell.gpuDirty = true;
        cell.bodyDirty = true;
        state.stats.treeCellsRebuilt++;
        self->residencyRevision++;
    }

    // Which cells hold CPU arrays: within Tree Distance plus one cell (R14)
    void updateTreeResidency(RegionState& state, const TerrainComponent& terrain, double startMs) {
        const float distance = treeDistanceOf(terrain) + FOLIAGE_TREE_CELL_SIZE;
        const int viewX = static_cast<int>(std::floor(state.viewLocal.x / FOLIAGE_TREE_CELL_SIZE));
        const int viewZ = static_cast<int>(std::floor(state.viewLocal.z / FOLIAGE_TREE_CELL_SIZE));
        if (viewX != state.treeViewCellX || viewZ != state.treeViewCellZ || distance != state.residencyDistance) {
            state.treeViewCellX = viewX;
            state.treeViewCellZ = viewZ;
            state.residencyDistance = distance;
            state.residentDirty = true;
        }
        if (state.residentDirty) {
            state.residentDirty = false;
            state.residentOrder.clear();
            const float distanceSquared = distance * distance;
            for (auto& entry : state.cells) {
                TreeCell& cell = entry.second;
                const float centerX = (static_cast<float>(cell.x) + 0.5f) * FOLIAGE_TREE_CELL_SIZE;
                const float centerZ = (static_cast<float>(cell.z) + 0.5f) * FOLIAGE_TREE_CELL_SIZE;
                const float dx = std::max(std::abs(centerX - state.viewLocal.x) - FOLIAGE_TREE_CELL_SIZE * 0.5f, 0.0f);
                const float dz = std::max(std::abs(centerZ - state.viewLocal.z) - FOLIAGE_TREE_CELL_SIZE * 0.5f, 0.0f);
                const bool inRange = dx * dx + dz * dz <= distanceSquared;
                if (inRange) {
                    if (!cell.resident) {
                        cell.resident = true;
                        cell.cpuDirty = true;
                    }
                    state.residentOrder.push_back(entry.first);
                } else if (cell.resident) {
                    cell.resident = false;
                    std::vector<FoliageInstance>().swap(cell.instances);
                    std::vector<uint32_t>().swap(cell.slotIds);
                    cell.ranges.clear();
                    if (cell.gpu) {
                        cell.gpu->getRender()->destroyBuffer();
                        cell.gpu.reset();
                        cell.gpuCapacity = 0;
                    }
                    cell.wantsGpu = false;
                    self->residencyRevision++;
                }
            }
            // canonical, so a view's nearest-first ties come out the same
            std::sort(state.residentOrder.begin(), state.residentOrder.end());
        }

        for (int64_t key : state.residentOrder) {
            TreeCell& cell = state.cells[key];
            if (!cell.cpuDirty) continue;
            if (foliageNowMs() - startMs > APPLY_BUDGET_MS) {
                state.stats.pendingCells++;
                continue;
            }
            buildCell(state, cell, terrain);
        }
    }

    void prepareWritableStore(RegionState& state) {
        if (!state.store) state.store = std::make_shared<TreeStore>();
        if (state.storeShared) {
            state.store = std::make_shared<TreeStore>(*state.store);
            state.storeShared = false;
            state.indexing = state.store;
        }
    }

    void markTreeCellDirtyForRecord(RegionState& state, const TreeRecord& record, bool add, bool remove) {
        const float size = state.terrainSize;
        const int x = static_cast<int>(std::floor(foliageTreeLocalX(record, size) / FOLIAGE_TREE_CELL_SIZE));
        const int z = static_cast<int>(std::floor(foliageTreeLocalZ(record, size) / FOLIAGE_TREE_CELL_SIZE));
        const int64_t key = foliageTreeCellKey(x, z);
        auto it = state.cells.find(key);
        if (it == state.cells.end()) {
            if (!add) return;
            TreeCell& cell = state.cells[key];
            cell.x = x;
            cell.z = z;
            state.residentDirty = true;
            it = state.cells.find(key);
        }
        TreeCell& cell = it->second;
        if (remove) {
            cell.ids.erase(std::remove(cell.ids.begin(), cell.ids.end(), record.id), cell.ids.end());
        }
        if (add && std::find(cell.ids.begin(), cell.ids.end(), record.id) == cell.ids.end()) {
            cell.ids.push_back(record.id);
        }
        cell.cpuDirty = true;
    }

    // R25: touched cells' records take the surface's height again; a hole hides without deleting
    void resnapTrees(RegionState& state, float minX, float minZ, float maxX, float maxZ) {
        if (!state.cellsReady) return;
        const TerrainSurfaceQuery& surface = state.region->surface();
        const float size = state.terrainSize;
        const float maxHeight = state.terrainMaxHeight;
        const int firstX = static_cast<int>(std::floor((minX - 1.0f) / FOLIAGE_TREE_CELL_SIZE));
        const int lastX = static_cast<int>(std::floor((maxX + 1.0f) / FOLIAGE_TREE_CELL_SIZE));
        const int firstZ = static_cast<int>(std::floor((minZ - 1.0f) / FOLIAGE_TREE_CELL_SIZE));
        const int lastZ = static_cast<int>(std::floor((maxZ + 1.0f) / FOLIAGE_TREE_CELL_SIZE));

        std::vector<TreeRecord> changed;
        bool storeWrites = false;
        for (int z = firstZ; z <= lastZ; z++) {
            for (int x = firstX; x <= lastX; x++) {
                const auto it = state.cells.find(foliageTreeCellKey(x, z));
                if (it == state.cells.end()) continue;
                TreeCell& cell = it->second;
                for (uint32_t id : cell.ids) {
                    const TreeRecord* record = findRecord(state, id);
                    if (!record) continue;
                    const SurfaceSample sample = surface.sample(foliageTreeLocalX(*record, size), foliageTreeLocalZ(*record, size));
                    TreeRecord next = *record;
                    if (record->flags & TREE_SNAP_TO_TERRAIN) {
                        next.height = maxHeight != 0.0f ? sample.height / maxHeight : 0.0f;
                    }
                    if (sample.hole) next.flags |= TREE_HIDDEN_BY_HOLE;
                    else next.flags &= static_cast<uint8_t>(~TREE_HIDDEN_BY_HOLE);
                    if (std::memcmp(&next, record, sizeof(TreeRecord)) != 0) {
                        changed.push_back(next);
                        storeWrites = storeWrites || next.id < TREE_RUNTIME_ID_BASE;
                    }
                }
                cell.cpuDirty = true;
            }
        }
        if (changed.empty()) return;

        if (storeWrites) prepareWritableStore(state);
        for (const TreeRecord& record : changed) {
            if (record.id >= TREE_RUNTIME_ID_BASE) {
                for (TreeRecord& added : state.runtime.added) {
                    if (added.id == record.id) added = record;
                }
            } else if (state.store->update(record)) {
                state.storeDirty = true;
            }
        }
    }

    // Takes what events left: partial rebuilds (R25, R26)
    void applyDirty(RegionState& state, float dt) {
        if (!state.densityDirty.empty()) {
            TerrainMapStore& maps = state.region->maps();
            for (const auto& [layerIndex, rect] : state.densityDirty) {
                if (layerIndex < 0 || layerIndex >= static_cast<int>(state.layers.size())) continue;
                const MapFormat format = maps.format(MapId{MapKind::Density, layerIndex});
                if (!format.exists) continue;
                const float size = state.terrainSize;
                const float half = size * 0.5f;
                const float toX = size / static_cast<float>(format.bounds.w);
                const float toZ = size / static_cast<float>(format.bounds.h);
                markDetailRectDirty(state.layers[layerIndex],
                                    -half + static_cast<float>(rect.x) * toX, -half + static_cast<float>(rect.y) * toZ,
                                    -half + static_cast<float>(rect.right()) * toX, -half + static_cast<float>(rect.bottom()) * toZ);
            }
            state.densityDirty.clear();
        }

        if (state.heightDirty) {
            state.heightDirtyAge += dt;
            if (state.heightDirtyAge >= RESNAP_DEBOUNCE_SECONDS) {
                const float minX = state.heightDirtyMin[0];
                const float minZ = state.heightDirtyMin[1];
                const float maxX = state.heightDirtyMax[0];
                const float maxZ = state.heightDirtyMax[1];
                state.heightDirty = false;
                state.heightDirtyAge = 0.0f;
                for (DetailLayerState& layer : state.layers) {
                    markDetailRectDirty(layer, minX, minZ, maxX, maxZ);
                }
                resnapTrees(state, minX, minZ, maxX, maxZ);
            }
        }
    }

    void onEdit(Entity owner, MapId map, TexelRect rect) {
        RegionState* state = regionOf(owner);
        if (!state) return;
        if (map.kind == MapKind::Density) {
            state->densityDirty.emplace_back(map.index, rect);
            return;
        }
        if (map.kind != MapKind::Height && map.kind != MapKind::Hole) return;
        if (map.kind == MapKind::Hole) state->holeCopyValid = false;

        const MapFormat format = state->region->maps().format(map);
        const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(owner);
        const float size = terrain ? terrain->terrainSize : state->terrainSize;
        const float half = size * 0.5f;
        const float toX = format.bounds.w > 0 ? size / static_cast<float>(format.bounds.w) : 0.0f;
        const float toZ = format.bounds.h > 0 ? size / static_cast<float>(format.bounds.h) : 0.0f;
        const float minX = -half + static_cast<float>(rect.x) * toX;
        const float minZ = -half + static_cast<float>(rect.y) * toZ;
        const float maxX = -half + static_cast<float>(rect.right()) * toX;
        const float maxZ = -half + static_cast<float>(rect.bottom()) * toZ;
        if (!state->heightDirty) {
            state->heightDirty = true;
            state->heightDirtyMin[0] = minX;
            state->heightDirtyMin[1] = minZ;
            state->heightDirtyMax[0] = maxX;
            state->heightDirtyMax[1] = maxZ;
        } else {
            state->heightDirtyMin[0] = std::min(state->heightDirtyMin[0], minX);
            state->heightDirtyMin[1] = std::min(state->heightDirtyMin[1], minZ);
            state->heightDirtyMax[0] = std::max(state->heightDirtyMax[0], maxX);
            state->heightDirtyMax[1] = std::max(state->heightDirtyMax[1], maxZ);
        }
        // a later edit starts the wait over: a drag settles first
        state->heightDirtyAge = 0.0f;
    }

    void updatePrototypes(RegionState& state, TerrainComponent& terrain) {
        while (state.prototypes.size() > terrain.treePrototypes.size()) {
            destroyPrototype(state.prototypes.back());
            state.prototypes.pop_back();
            state.residentDirty = true;
            self->residencyRevision++;
        }
        state.prototypes.resize(terrain.treePrototypes.size());

        for (size_t i = 0; i < terrain.treePrototypes.size(); i++) {
            const TerrainTreePrototype& authored = terrain.treePrototypes[i];
            TreePrototypeState& prototype = state.prototypes[i];

            std::vector<std::string> wanted;
            if (!authored.meshPath.empty()) wanted.push_back(authored.meshPath);
            for (const AssetRef& lod : authored.lodMeshes) {
                if (!lod.empty() && wanted.size() < MAX_MESH_LODS) wanted.push_back(lod);
            }

            if (prototype.id != authored.id || prototype.paths != wanted) {
                for (size_t l = 0; l < prototype.lods.size(); l++) destroyPrototypeEntity(prototype.lods[l], prototype.slots[l]);
                prototype.lods.assign(wanted.size(), NULL_ENTITY);
                prototype.slots.assign(wanted.size(), 0);
                prototype.paths = wanted;
                prototype.ready.assign(wanted.size(), 0);
                prototype.failed.assign(wanted.size(), 0);
                prototype.id = authored.id;
                // the cells' ranges point at prototypes by index
                for (auto& entry : state.cells) entry.second.cpuDirty = true;
                self->residencyRevision++;
            }

            for (size_t l = 0; l < wanted.size(); l++) {
                if (prototype.lods[l] == NULL_ENTITY) {
                    PrototypeRef ref;
                    ref.prototype = static_cast<int>(i);
                    ref.lod = static_cast<int>(l);
                    uint32_t slot = 0;
                    prototype.lods[l] = createPrototypeEntity(state, slot, ref);
                    prototype.slots[l] = slot;
                    InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(prototype.lods[l]);
                    instmesh.foliageDither = true;
                }
                // the variant follows Bend Factor: an inspector edit reloads the prototype
                InstancedMeshComponent& protoInstmesh = scene->getComponent<InstancedMeshComponent>(prototype.lods[l]);
                const bool wantsWind = authored.bendFactor > 0.0f;
                if (protoInstmesh.foliageWind != wantsWind) {
                    protoInstmesh.foliageWind = wantsWind;
                    scene->getComponent<MeshComponent>(prototype.lods[l]).needReload = true;
                }
                syncPrototypeTransform(state, prototype.lods[l]);
                if (!prototype.ready[l] && !prototype.failed[l]) {
                    bool ready = false;
                    bool failed = false;
                    Vector3 center;
                    float radius = 0.0f;
                    std::string requested;
                    pollMesh(prototype.lods[l], wanted[l], requested, ready, failed, center, radius, state.stats.meshLoads);
                    prototype.ready[l] = ready;
                    prototype.failed[l] = failed;
                    if (ready) {
                        if (l == 0) {
                            prototype.center = center;
                            prototype.radius = radius;
                        }
                        self->residencyRevision++;
                    }
                }
            }

            updateImpostor(state, prototype, authored, static_cast<int>(i));
        }
    }

    void updateImpostor(RegionState& state, TreePrototypeState& prototype, const TerrainTreePrototype& authored, int index) {
        const bool wanted = !authored.impostorAlbedo.empty() && authored.impostorViews > 0;
        if (!wanted) {
            if (prototype.impostor != NULL_ENTITY) {
                destroyPrototypeEntity(prototype.impostor, prototype.impostorSlot);
                prototype.impostorReady = false;
                self->residencyRevision++;
            }
            return;
        }

        const std::string albedo = authored.impostorAlbedo.getPath(0);
        const std::string normal = authored.impostorNormal.getPath(0);
        if (prototype.impostor != NULL_ENTITY && (prototype.impostorAlbedo != albedo || prototype.impostorNormal != normal)) {
            destroyPrototypeEntity(prototype.impostor, prototype.impostorSlot);
            prototype.impostorReady = false;
        }

        if (prototype.impostor == NULL_ENTITY) {
            PrototypeRef ref;
            ref.prototype = index;
            ref.impostor = true;
            prototype.impostor = createPrototypeEntity(state, prototype.impostorSlot, ref);
            prototype.impostorAlbedo = albedo;
            prototype.impostorNormal = normal;
            MeshSystem* meshSystem = scene->getSystem<MeshSystem>().get();
            MeshComponent& mesh = scene->getComponent<MeshComponent>(prototype.impostor);
            // a unit quad: the vertex program sizes and faces it
            meshSystem->createWall(mesh, 1.0f, 1.0f, 1);
            Submesh& submesh = mesh.submeshes[0];
            submesh.material.shader = "library:impostor";
            submesh.material.shaderTextures.emplace_back("u_baseTexture", authored.impostorAlbedo);
            submesh.material.shaderTextures.emplace_back("u_normalTexture", authored.impostorNormal);
            InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(prototype.impostor);
            instmesh.foliageWind = true;
            instmesh.foliageDither = true;
            prototype.impostorReady = true;
            self->residencyRevision++;
        }
        syncPrototypeTransform(state, prototype.impostor);
    }

    // -------------------------------------------------------------------------------- collision

    void rebuildBody(RegionState& state, TreeCell& cell, const TerrainComponent& terrain) {
        cell.bodyDirty = false;
        std::vector<foliagephysics::TreeCapsule> capsules;
        for (const TreeProtoRange& range : cell.ranges) {
            if (range.prototype >= static_cast<int>(terrain.treePrototypes.size())) continue;
            const TerrainTreePrototype& prototype = terrain.treePrototypes[range.prototype];
            if (prototype.collision != TerrainTreeCollision::Capsule) continue;
            for (uint32_t i = range.first; i < range.first + range.count; i++) {
                const FoliageInstance& instance = cell.instances[i];
                foliagephysics::TreeCapsule capsule;
                capsule.id = cell.slotIds[i];
                capsule.base = Vector3(instance.position[0], instance.position[1], instance.position[2]);
                capsule.radius = prototype.collisionRadius * instance.scale;
                capsule.height = prototype.collisionHeight * instance.scale * static_cast<float>(instance.heightRatio) / 128.0f;
                capsules.push_back(capsule);
            }
        }
        const Entity before = cell.body.entity;
        foliagephysics::setCellBody(*scene, state.owner, cell.body, capsules);
        if (before != NULL_ENTITY && before != cell.body.entity) bodyCells.erase(before);
        if (cell.body.entity != NULL_ENTITY) bodyCells[cell.body.entity] = {state.owner, &cell.body.ids};
    }

    // R18: no prototype with collision: no body and no Jolt work. Compound shapes are made in the
    // frame, at most two a frame (T5 Deviation 3).
    void syncBodies(RegionState& state, const TerrainComponent& terrain) {
        bool collides = false;
        for (const TerrainTreePrototype& prototype : terrain.treePrototypes) {
            collides = collides || prototype.collision == TerrainTreeCollision::Capsule;
        }
        const bool wanted = collides && foliagephysics::simulating(*scene);
        if (!wanted && !state.hadBodies) return;
        if (!wanted) {
            for (auto& entry : state.cells) {
                if (entry.second.body.entity != NULL_ENTITY) {
                    bodyCells.erase(entry.second.body.entity);
                    foliagephysics::destroyCellBody(*scene, entry.second.body);
                }
                entry.second.bodyDirty = true;
            }
            state.hadBodies = false;
            return;
        }
        state.hadBodies = true;
        int budget = 2;
        for (int64_t key : state.residentOrder) {
            const auto it = state.cells.find(key);
            if (it == state.cells.end() || it->second.cpuDirty || !it->second.bodyDirty) continue;
            if (budget-- <= 0) {
                state.stats.pendingCells++;
                return;
            }
            rebuildBody(state, it->second, terrain);
        }
    }

    // -------------------------------------------------------------------------------- one Terrain

    void disableRegion(RegionState& state) {
        if (!state.active) return;
        state.active = false;
        for (DetailLayerState& layer : state.layers) {
            for (DetailSlot& slot : layer.ring) {
                if (slot.job) {
                    cancelSlot(slot);
                    slot.assigned = false;
                }
            }
            layer.pending.clear();
            layer.needScan = true;
        }
        self->residencyRevision++;
    }

    void updateRegion(RegionState& state, TerrainComponent& terrain, Transform& transform, float dt) {
        const double startMs = foliageNowMs();
        state.stats.applied = 0;
        state.stats.pendingCells = 0;

        const bool enabled = terrain.enabled && scene->isEntityActiveInHierarchy(state.owner);
        if (!enabled) {
            disableRegion(state);
            return;
        }
        if (!state.active) {
            state.active = true;
            self->residencyRevision++;
        }

        // Wait for the heightmap and terrain rebuild before placing instances.
        if (!terrain.heightMapLoaded || terrain.needUpdateTerrain) {
            return;
        }

        // tree cells are in terrain units: built before the size was known, or from another size,
        // they are in the wrong place
        if (state.terrainSize != terrain.terrainSize) {
            state.cellsReady = false;
            state.indexing.reset();
            state.indexTask = jobs::Task<std::shared_ptr<TreeCellMap>>();
        }
        state.terrainSize = terrain.terrainSize;
        state.terrainMaxHeight = terrain.maxHeight;
        state.model = transform.modelMatrix;
        state.inverseModel = transform.modelMatrix.inverse();
        if (!state.inverseModel.isValid()) return;
        state.terrainScale = std::max(std::abs(transform.worldScale.x), std::max(std::abs(transform.worldScale.y), std::abs(transform.worldScale.z)));

        Transform* cameraTransform = scene->findComponent<Transform>(scene->getCamera());
        if (!cameraTransform) return;
        state.viewLocal = state.inverseModel * cameraTransform->worldPosition;

        const bool preview = state.owner == previewEntity;
        const bool refillAll = state.refill || terrain.needUpdateFoliage;
        // a setting no cell's bytes show (a shadow mask): the views still have to look again
        if (terrain.needUpdateFoliage) self->residencyRevision++;
        if (terrain.needUpdateFoliage) state.holeCopyValid = false;
        state.refill = false;
        terrain.needUpdateFoliage = false;

        applyDirty(state, dt);

        // ---- details
        while (state.layers.size() > terrain.foliageLayers.size()) {
            destroyLayer(state.layers.back());
            state.layers.pop_back();
            self->residencyRevision++;
        }
        state.layers.resize(terrain.foliageLayers.size());

        const FoliageTierLimits limits = foliageTierLimits(foliageTierOf(Engine::getFoliageQuality()));
        const bool uncapped = Engine::isFoliageQualityUncapped();

        int inFlight = 0;
        bool budgetLeft = true;
        for (size_t i = 0; i < terrain.foliageLayers.size(); i++) {
            TerrainFoliageLayer& layer = terrain.foliageLayers[i];
            DetailLayerState& layerState = state.layers[i];

            if (layer.meshPath.empty() || layer.densityMap.empty()) {
                if (layerState.entity != NULL_ENTITY || !layerState.ring.empty()) {
                    destroyLayer(layerState);
                    self->residencyRevision++;
                }
                continue;
            }

            if (layerState.entity == NULL_ENTITY) {
                PrototypeRef ref;
                ref.layer = static_cast<int>(i);
                layerState.entity = createPrototypeEntity(state, layerState.slot, ref);
                layerState.needScan = true;
            }
            syncPrototypeTransform(state, layerState.entity);

            // the mesh: one load per layer, a failure retried by a terrain change
            const std::string path = layer.meshPath;
            if (layerState.loadedPath != path || (layerState.loadFailed && refillAll)) {
                layerState.loadedPath = path;
                layerState.loadFailed = false;
                layerState.meshReady = false;
                layerState.requestedPath.clear();
            }
            if (!layerState.meshReady && !layerState.loadFailed) {
                pollMesh(layerState.entity, path, layerState.requestedPath, layerState.meshReady, layerState.loadFailed,
                         layerState.center, layerState.radius, state.stats.meshLoads);
                if (layerState.meshReady) {
                    // R37: a custom vertex program with no compact variant draws from a 96-byte stream
                    if (programNeedsExpanded(layerState.entity)) {
                        InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(layerState.entity);
                        instmesh.foliageExpanded = true;
                        setExpandedLayout(instmesh);
                    }
                    layerState.boundsDirty = true;
                    self->residencyRevision++;
                }
            }

            InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(layerState.entity);
            MeshComponent& mesh = scene->getComponent<MeshComponent>(layerState.entity);
            const bool wantWind = layer.windBend > 0.0f;
            if (instmesh.foliageWind != wantWind) {
                instmesh.foliageWind = wantWind;
                if (mesh.loaded) mesh.needReload = true;
            }
            mesh.castShadows = layer.castShadows && (uncapped || limits.detailShadows);

            // what placement depends on
            const FoliageLayerParams params = paramsOf(layer);
            const float distance = effectiveDetailDistance(terrain, layer, preview);
            const bool paramsChanged = !sameParams(params, layerState.params);
            layerState.params = params;
            const bool distanceChanged = distance != layerState.distance;
            layerState.distance = distance;
            if (distanceChanged) self->residencyRevision++;

            if (distance <= 0.0f && !preview) {
                // Foliage Distance 0, or a layer with no reach: nothing is placed or held
                if (!layerState.ring.empty()) {
                    for (DetailSlot& slot : layerState.ring) cancelSlot(slot);
                    layerState.ring.clear();
                    layerState.side = 0;
                    layerState.pending.clear();
                    layerState.resident = 0;
                    layerState.boundsDirty = true;
                    self->residencyRevision++;
                }
                layerState.needScan = false;
                updateLayerBounds(layerState);
                continue;
            }

            // A density map restored from a scene holds only a path, and the resolve reads it on the CPU.
            layer.densityMap.setReleaseDataAfterLoad(false);
            if (layer.densityMap.load().state == ResourceLoadState::Loading) {
                layerState.waitingForMap = true;
                state.stats.pendingCells++;
                continue;
            }
            if (layerState.waitingForMap) {
                layerState.waitingForMap = false;
                layerState.needScan = true;
            }

            if (refillAll || paramsChanged) {
                for (DetailSlot& slot : layerState.ring) {
                    cancelSlot(slot);
                    slot.assigned = false;
                }
                layerState.pending.clear();
                layerState.needScan = true;
            }

            if (budgetLeft) {
                budgetLeft = updateDetailLayer(state, layerState, static_cast<int>(i), preview, startMs, inFlight);
            }
            state.stats.pendingCells += static_cast<uint32_t>(layerState.pending.size());
            if (layerState.needScan) state.stats.pendingCells++;
            updateLayerBounds(layerState);
        }

        // ---- trees
        if (!terrain.treePrototypes.empty() || !terrain.treeData.empty() || state.store) {
            updatePrototypes(state, terrain);

            const std::string path = terrain.treeData;
            if (path != state.storePath && !state.storeShared) {
                if (state.storeLoading) {
                    state.storeTask.cancel();
                    state.storeLoading = false;
                }
                // (treeFileWritten keeps the two paths equal when a save gave this store its file)
                state.storePath = path;
                state.store.reset();
                for (auto& entry : state.cells) releaseCell(entry.second);
                state.cells.clear();
                state.cellsReady = false;
                if (!path.empty()) startStoreLoad(state, path);
            }
            pollStoreLoad(state);
            if (state.store && !state.cellsReady && !state.indexTask.isValid()) startIndex(state);
            if (!state.store && !state.storeLoading && !state.cellsReady && !state.runtime.added.empty()) startIndex(state);
            pollIndex(state);
            if (state.storeLoading || (state.store && !state.cellsReady)) state.stats.pendingCells++;

            if (state.cellsReady) {
                updateTreeResidency(state, terrain, startMs);
            }
        }

        syncBodies(state, terrain);

        // R20: read once a frame, and only if something bends
        state.windActive = false;
        for (const TerrainFoliageLayer& layer : terrain.foliageLayers) state.windActive = state.windActive || layer.windBend > 0.0f;
        for (const TerrainTreePrototype& prototype : terrain.treePrototypes) state.windActive = state.windActive || prototype.bendFactor > 0.0f;
        if (state.windActive) {
            clock += dt;
            state.wind = self->getWindVector(terrain, clock);
        }

        // ---- stats
        uint64_t detail = 0;
        for (const DetailLayerState& layer : state.layers) detail += layer.resident;
        state.stats.detailResident = detail;
        state.stats.treesTotal = (state.store ? state.store->size() : 0) + state.runtime.added.size();
        uint64_t resident = 0;
        uint32_t cells = 0;
        for (int64_t key : state.residentOrder) {
            const auto it = state.cells.find(key);
            if (it != state.cells.end()) resident += it->second.instances.size();
            cells++;
        }
        for (const DetailLayerState& layer : state.layers) {
            for (const DetailSlot& slot : layer.ring) {
                if (slot.instances) cells++;
            }
        }
        state.stats.cells = cells;
        state.stats.treesResident = resident;
        state.stats.cpuBytes = (detail + resident) * sizeof(FoliageInstance) + state.stats.treesTotal * sizeof(TreeRecord);
        state.stats.mainThreadMs = foliageNowMs() - startMs;
    }

    // -------------------------------------------------------------------------------- views

    static void localFrustum(const Plane* planes, float farClip, const Matrix4& model, FoliageFrustum& frustum) {
        frustum.count = 0;
        for (int p = 0; p < 6; p++) {
            if (p == FRUSTUM_PLANE_FAR && farClip == 0.0f) continue;
            const Vector3& n = planes[p].normal;
            // (M p) . n + d = p . (M^T n) + d + t . n
            const float x = model[0][0] * n.x + model[0][1] * n.y + model[0][2] * n.z;
            const float y = model[1][0] * n.x + model[1][1] * n.y + model[1][2] * n.z;
            const float z = model[2][0] * n.x + model[2][1] * n.y + model[2][2] * n.z;
            const float d = planes[p].d + model[3][0] * n.x + model[3][1] * n.y + model[3][2] * n.z;
            const float length = std::sqrt(x * x + y * y + z * z);
            const float scale = length > 0.0f ? 1.0f / length : 0.0f;
            float* out = frustum.planes[frustum.count++];
            out[0] = x * scale;
            out[1] = y * scale;
            out[2] = z * scale;
            out[3] = d * scale;
        }
    }

    FoliageLodTable lodTableOf(const MeshComponent& mesh, const FoliageViewRequest& request) const {
        FoliageLodTable table;
        table.enabled = request.lodEnabled && mesh.lodEnabled && mesh.numLods > 1;
        table.numLods = mesh.numLods;
        for (unsigned int l = 0; l < MAX_MESH_LODS && l < FOLIAGE_MAX_LODS; l++) table.error[l] = mesh.lodError[l];
        table.threshold = request.lodThreshold;
        table.perUnit = request.lodPerUnit;
        table.constant = request.lodConstant;
        table.bias = mesh.lodBias;
        return table;
    }

    // F4: cells on the main thread, survivors written by jobs into ranges reserved by prefix sum,
    // then compacted and sorted into their LOD buckets on the main thread
    struct DetailCandidate {
        const FoliageInstance* instances;
        size_t count;
        FoliageCellVisibility visibility;
        float nearest;
        size_t reserved;    // first in scratch
        size_t kept;
        const FoliageDetailCull* cull;
        const FoliageFrustum* frustum;
    };

    struct LayerView {
        DetailLayerState* state;
        FoliageDetailCull cull;
        size_t firstCandidate = 0;
        size_t candidateCount = 0;
        bool transparent = false;
        unsigned int submeshes = 1;
    };

    void buildDetailDraws(FoliageViewList& list, const FoliageViewRequest& request, const FoliageTierLimits& limits, bool uncapped) {
        static thread_local std::vector<LayerView> layerViews;
        static thread_local std::vector<DetailCandidate> candidates;
        static thread_local std::vector<FoliageFrustum> frusta;
        layerViews.clear();
        candidates.clear();
        frusta.clear();
        frusta.reserve(regions.size());

        size_t reservedTotal = 0;
        for (const std::unique_ptr<RegionState>& regionPtr : regions) {
            RegionState& region = *regionPtr;
            if (!region.active || region.layers.empty()) continue;
            TerrainComponent* terrain = scene->findComponent<TerrainComponent>(region.owner);
            if (!terrain) continue;
            frusta.emplace_back();
            localFrustum(request.planes, request.farClip, region.model, frusta.back());
            const FoliageFrustum& frustum = frusta.back();

            const bool preview = region.owner == previewEntity;
            const Vector3 eye = region.inverseModel * request.eye;
            const Vector3 lodOrigin = region.inverseModel * request.lodOrigin;

            for (size_t l = 0; l < region.layers.size() && l < terrain->foliageLayers.size(); l++) {
                DetailLayerState& state = region.layers[l];
                if (!state.meshReady || state.entity == NULL_ENTITY || state.ring.empty()) continue;
                const TerrainFoliageLayer& layer = terrain->foliageLayers[l];
                const MeshComponent& mesh = scene->getComponent<MeshComponent>(state.entity);
                if (!mesh.loaded) continue;
                if (request.shadow && !mesh.castShadows) continue;

                layerViews.emplace_back();
                LayerView& view = layerViews.back();
                view.state = &state;
                view.transparent = mesh.isTransparent();
                view.submeshes = std::max(mesh.numSubmeshes, 1u);
                view.firstCandidate = candidates.size();

                FoliageDetailCull& cull = view.cull;
                cull.eye[0] = eye.x; cull.eye[1] = eye.y; cull.eye[2] = eye.z;
                cull.lodOrigin[0] = lodOrigin.x; cull.lodOrigin[1] = lodOrigin.y; cull.lodOrigin[2] = lodOrigin.z;
                cull.terrainScale = region.terrainScale;
                cull.limitDistance = !preview;
                cull.drawDistance = state.distance;
                cull.fadeStart = state.distance * FOLIAGE_FADE_START;
                const float scale = preview ? 1.0f : Engine::getFoliageDistanceScale();
                cull.minDistance = preview ? 0.0f : std::clamp(layer.minDrawDistance * scale, 0.0f, std::max(state.distance, 0.0f));
                cull.nearFadeStart = cull.minDistance * FOLIAGE_FADE_START;
                cull.density = terrain->detailDensity * layer.densityScale * (uncapped ? 1.0f : limits.detailDensity);
                cull.falloffStart = layer.densityFalloffStart * state.distance;
                cull.falloffEnd = state.distance;
                cull.farDensity = layer.farDensity;
                cull.meshCenter[0] = state.center.x; cull.meshCenter[1] = state.center.y; cull.meshCenter[2] = state.center.z;
                cull.meshRadius = state.radius;
                cull.lod = lodTableOf(mesh, request);

                const float reach = (state.radius + state.center.length()) * std::max(state.params.maxScale, 0.001f) * std::max(1.0f, state.params.maxHeightRatio);
                for (const DetailSlot& slot : state.ring) {
                    if (!slot.instances || slot.instances->empty()) continue;
                    const float boxMin[3] = {static_cast<float>(slot.x) * FOLIAGE_DETAIL_CELL_SIZE, slot.minY, static_cast<float>(slot.z) * FOLIAGE_DETAIL_CELL_SIZE};
                    const float boxMax[3] = {boxMin[0] + FOLIAGE_DETAIL_CELL_SIZE, slot.maxY, boxMin[2] + FOLIAGE_DETAIL_CELL_SIZE};
                    if (!foliageCellInDistance(cull, boxMin, boxMax)) continue;
                    const FoliageCellVisibility visibility = classifyFoliageBox(frustum, boxMin, boxMax, reach);
                    if (visibility == FoliageCellVisibility::Outside) continue;

                    float nearest, farthest;
                    foliageBoxDistancesXZ(cull.eye, boxMin, boxMax, nearest, farthest);
                    DetailCandidate candidate;
                    candidate.instances = slot.instances->data();
                    candidate.count = slot.instances->size();
                    candidate.visibility = visibility;
                    candidate.nearest = nearest;
                    candidate.reserved = reservedTotal;
                    candidate.kept = 0;
                    candidate.cull = nullptr;
                    candidate.frustum = &frustum;
                    reservedTotal += candidate.count;
                    candidates.push_back(candidate);
                }
                view.candidateCount = candidates.size() - view.firstCandidate;
            }
        }

        // the cull params live in layerViews, which no longer grows
        for (const LayerView& view : layerViews) {
            for (size_t c = 0; c < view.candidateCount; c++) candidates[view.firstCandidate + c].cull = &view.cull;
        }

        if (list.scratch.size() < reservedTotal) {
            list.scratch.resize(reservedTotal + reservedTotal / 4);
            list.scratchLods.resize(list.scratch.size());
        }
        FoliageInstance* scratch = list.scratch.data();
        uint8_t* scratchLods = list.scratchLods.data();
        DetailCandidate* candidateData = candidates.data();
        const auto cullOne = [scratch, scratchLods, candidateData](size_t index) {
            DetailCandidate& candidate = candidateData[index];
            candidate.kept = cullFoliageCell(candidate.instances, candidate.count, *candidate.cull, *candidate.frustum,
                                             candidate.visibility, scratch + candidate.reserved, scratchLods + candidate.reserved);
        };
        // the output does not depend on who ran which unit: each writes its own range
        if (jobs::hasSystem() && candidates.size() > 16) {
            jobs::parallelFor(candidates.size(), cullOne);
        } else {
            for (size_t i = 0; i < candidates.size(); i++) cullOne(i);
        }

        // R21's caps: the farthest cells go first
        static thread_local std::vector<FoliageCapCandidate> caps;
        caps.clear();
        caps.reserve(candidates.size());
        for (size_t i = 0; i < candidates.size(); i++) {
            FoliageCapCandidate cap;
            cap.distance = candidates[i].nearest;
            cap.order = static_cast<uint32_t>(i);
            cap.instances = static_cast<uint32_t>(candidates[i].kept);
            cap.draws = 0;
            caps.push_back(cap);
        }
        const uint32_t maxInstances = uncapped ? UINT32_MAX : limits.maxDetailInstances;
        if (applyFoliageCaps(caps, maxInstances, UINT32_MAX)) list.capped = true;

        // compact sequentially, sorted by LOD, per layer
        static thread_local std::vector<uint32_t> byDistance;
        for (const LayerView& view : layerViews) {
            unsigned int lodCounts[FOLIAGE_MAX_LODS] = {};
            for (size_t c = 0; c < view.candidateCount; c++) {
                const size_t index = view.firstCandidate + c;
                if (!caps[index].kept) continue;
                const DetailCandidate& candidate = candidates[index];
                for (size_t i = 0; i < candidate.kept; i++) {
                    lodCounts[std::min<unsigned>(scratchLods[candidate.reserved + i], FOLIAGE_MAX_LODS - 1)]++;
                }
            }
            uint32_t total = 0;
            for (unsigned int count : lodCounts) total += count;
            if (total == 0) continue;

            // nearest cell first; blended layers farthest first
            byDistance.resize(view.candidateCount);
            for (uint32_t c = 0; c < view.candidateCount; c++) byDistance[c] = static_cast<uint32_t>(view.firstCandidate + c);
            const bool farFirst = view.transparent;
            std::sort(byDistance.begin(), byDistance.end(), [&](uint32_t a, uint32_t b) {
                if (candidates[a].nearest != candidates[b].nearest) {
                    return farFirst ? candidates[a].nearest > candidates[b].nearest : candidates[a].nearest < candidates[b].nearest;
                }
                return a < b;
            });

            const InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(view.state->entity);
            const bool expanded = instmesh.foliageExpanded;
            uint32_t bucketFirst[FOLIAGE_MAX_LODS];
            uint32_t cursor[FOLIAGE_MAX_LODS];
            uint32_t running = static_cast<uint32_t>(expanded ? list.expanded.size() : list.stream.size());
            for (int l = 0; l < FOLIAGE_MAX_LODS; l++) {
                bucketFirst[l] = running;
                cursor[l] = running;
                running += lodCounts[l];
            }
            if (expanded) {
                list.expanded.resize(running);
            } else {
                list.stream.resize(running);
            }
            for (uint32_t index : byDistance) {
                if (!caps[index].kept) continue;
                const DetailCandidate& candidate = candidates[index];
                for (size_t i = 0; i < candidate.kept; i++) {
                    const unsigned int lod = std::min<unsigned>(scratchLods[candidate.reserved + i], FOLIAGE_MAX_LODS - 1);
                    const FoliageInstance& instance = scratch[candidate.reserved + i];
                    if (expanded) {
                        expandInstance(instance, list.expanded[cursor[lod]++]);
                    } else {
                        list.stream[cursor[lod]++] = instance;
                    }
                }
            }

            const uint32_t firstItem = static_cast<uint32_t>(list.items.size());
            for (int l = 0; l < FOLIAGE_MAX_LODS; l++) {
                if (lodCounts[l] == 0) continue;
                FoliageDrawItem item;
                item.buffer = expanded ? list.expandedBuffer.getRender() : list.buffer.getRender();
                item.first = bucketFirst[l];
                item.count = lodCounts[l];
                item.level = static_cast<uint32_t>(l);
                list.items.push_back(item);
                list.draws += view.submeshes;
            }
            list.ranges[view.state->slot] = {firstItem, static_cast<uint32_t>(list.items.size()) - firstItem};
            list.detailVisible += total;
        }
    }

    static void expandInstance(const FoliageInstance& instance, InstanceRenderData& out) {
        const float x = instance.rotation[0] / 32767.0f;
        const float y = instance.rotation[1] / 32767.0f;
        const float z = instance.rotation[2] / 32767.0f;
        const float w = instance.rotation[3] / 32767.0f;
        const float length = std::sqrt(x * x + y * y + z * z + w * w);
        const float s = length > 0.0f ? 1.0f / length : 0.0f;
        const float qx = x * s, qy = y * s, qz = z * s, qw = w * s;
        const float sx = instance.scale;
        const float sy = instance.scale * static_cast<float>(instance.heightRatio) / 128.0f;
        Matrix4 m;
        m[0][0] = (1.0f - 2.0f * (qy * qy + qz * qz)) * sx;
        m[0][1] = 2.0f * (qx * qy + qz * qw) * sx;
        m[0][2] = 2.0f * (qx * qz - qy * qw) * sx;
        m[0][3] = 0.0f;
        m[1][0] = 2.0f * (qx * qy - qz * qw) * sy;
        m[1][1] = (1.0f - 2.0f * (qx * qx + qz * qz)) * sy;
        m[1][2] = 2.0f * (qy * qz + qx * qw) * sy;
        m[1][3] = 0.0f;
        m[2][0] = 2.0f * (qx * qz + qy * qw) * sx;
        m[2][1] = 2.0f * (qy * qz - qx * qw) * sx;
        m[2][2] = (1.0f - 2.0f * (qx * qx + qy * qy)) * sx;
        m[2][3] = 0.0f;
        m[3][0] = instance.position[0];
        m[3][1] = instance.position[1];
        m[3][2] = instance.position[2];
        m[3][3] = 1.0f;
        out.instanceMatrix = m;
        out.color = Vector4(instance.color[0] / 255.0f, instance.color[1] / 255.0f, instance.color[2] / 255.0f, instance.color[3] / 255.0f);
        out.textureRect = Rect(0.0f, 0.0f, 1.0f, 1.0f);
    }

    void buildTreeDraws(FoliageViewList& list, const FoliageViewRequest& request, const FoliageTierLimits& limits, bool uncapped) {
        for (const std::unique_ptr<RegionState>& regionPtr : regions) {
            RegionState& region = *regionPtr;
            if (!region.active || region.residentOrder.empty() || region.prototypes.empty()) continue;
            TerrainComponent* terrain = scene->findComponent<TerrainComponent>(region.owner);
            if (!terrain || terrain->treePrototypes.empty()) continue;

            FoliageFrustum frustum;
            localFrustum(request.planes, request.farClip, region.model, frustum);
            const Vector3 eye = region.inverseModel * request.eye;
            const float eyeArray[3] = {eye.x, eye.y, eye.z};

            const float billboard = scaledDistance(terrain->billboardStart, false);
            const float treeDistance = treeDistanceOf(*terrain);
            const float fade = terrain->fadeLength * (uncapped ? 1.0f : limits.fadeLength);
            const uint64_t authoredMesh = static_cast<uint64_t>(std::max(terrain->maxMeshTrees, 0));
            const uint64_t maxMesh = uncapped ? authoredMesh : std::min<uint64_t>(authoredMesh, limits.maxMeshTreesCap);
            const uint64_t maxVisible = uncapped ? UINT64_MAX : limits.maxVisibleTrees;
            const uint32_t maxDraws = uncapped ? UINT32_MAX : limits.maxDraws;

            // nearest first
            struct Visible { TreeCell* cell; float nearest; float farthest; };
            static thread_local std::vector<Visible> visible;
            visible.clear();
            for (int64_t key : region.residentOrder) {
                const auto it = region.cells.find(key);
                if (it == region.cells.end()) continue;
                TreeCell& cell = it->second;
                if (cell.instances.empty()) continue;
                float nearest, farthest;
                foliageBoxDistancesXZ(eyeArray, cell.boxMin, cell.boxMax, nearest, farthest);
                if (nearest >= treeDistance) continue;
                float reach = 0.0f;
                for (const TreeProtoRange& range : cell.ranges) {
                    if (range.prototype >= static_cast<int>(region.prototypes.size())) continue;
                    const TreePrototypeState& prototype = region.prototypes[range.prototype];
                    reach = std::max(reach, (prototype.radius + prototype.center.length()) * range.maxScale);
                }
                if (classifyFoliageBox(frustum, cell.boxMin, cell.boxMax, reach) == FoliageCellVisibility::Outside) continue;
                visible.push_back(Visible{&cell, nearest, farthest});
            }
            std::stable_sort(visible.begin(), visible.end(), [](const Visible& a, const Visible& b) { return a.nearest < b.nearest; });

            // per entity draws, then copied in order: the items of one entity stay together
            static thread_local std::vector<std::vector<FoliageDrawItem>> entityDraws;
            if (entityDraws.size() < self->nextSlot) entityDraws.resize(self->nextSlot);
            for (std::vector<FoliageDrawItem>& draws : entityDraws) draws.clear();

            uint64_t meshTrees = 0;
            uint64_t visibleTrees = 0;

            // the impostor band: whole visible cells, one draw per prototype
            struct ImpostorPiece { const TreeCell* cell; const TreeProtoRange* range; };
            static thread_local std::vector<std::vector<ImpostorPiece>> impostorPieces;
            impostorPieces.resize(region.prototypes.size());
            for (std::vector<ImpostorPiece>& pieces : impostorPieces) pieces.clear();

            for (const Visible& entry : visible) {
                TreeCell& cell = *entry.cell;
                for (const TreeProtoRange& range : cell.ranges) {
                    if (range.prototype >= static_cast<int>(region.prototypes.size())) continue;
                    // R21: the farthest cells are the ones past the cap
                    if (visibleTrees + range.count > maxVisible) {
                        list.capped = true;
                        continue;
                    }
                    const TerrainTreePrototype& authored = terrain->treePrototypes[range.prototype];
                    const TreePrototypeState& prototype = region.prototypes[range.prototype];
                    if (prototype.lods.empty() || !prototype.ready[0]) continue;

                    const MeshComponent& mesh = scene->getComponent<MeshComponent>(prototype.lods[0]);
                    FoliageTreeBandSettings settings;
                    settings.treeDistance = treeDistance;
                    settings.billboardStart = billboard;
                    settings.fadeLength = fade;
                    settings.hasImpostor = prototype.impostorReady;
                    settings.explicitLods = static_cast<unsigned int>(prototype.lods.size()) - 1;
                    settings.lodDistanceCount = std::min<unsigned int>(static_cast<unsigned int>(authored.lodDistances.size()), FOLIAGE_MAX_EXPLICIT_LODS);
                    for (unsigned int l = 0; l < settings.lodDistanceCount; l++) settings.lodDistances[l] = scaledDistance(authored.lodDistances[l], false);
                    settings.lod = lodTableOf(mesh, request);
                    settings.prototypeError = range.maxScale * region.terrainScale;

                    const unsigned int submeshes = std::max(mesh.numSubmeshes, 1u);
                    const bool overDraws = list.draws + submeshes > maxDraws;
                    if (overDraws) list.capped = true;
                    // R15: past Max Mesh Trees a cell draws impostors; with none baked its trees stay meshes,
                    // at their lowest LOD, to Tree Distance. The panel warns of what that costs.
                    const bool overMeshCap = meshTrees + range.count > maxMesh;
                    if (overMeshCap && !settings.hasImpostor) list.capped = true;
                    const bool allowMesh = !overDraws && (!overMeshCap || !settings.hasImpostor);
                    FoliageTreeBands bands = selectFoliageTreeBands(settings, entry.nearest, entry.farthest, allowMesh);

                    if (request.shadow) {
                        // R17: a band whose bit is clear casts nothing
                        const unsigned int lodBit = 1u << std::min(bands.meshLod, 6u);
                        if (bands.mesh && !(authored.shadowLodMask & lodBit)) bands.mesh = false;
                        if (bands.impostor && !(authored.shadowLodMask & 0x80u)) bands.impostor = false;
                    }

                    if (bands.mesh) {
                        // an explicit LOD has a mesh of its own, else the generated levels of LOD 0
                        const bool explicitLod = settings.explicitLods > 0;
                        const unsigned int entityIndex = explicitLod ? std::min<unsigned int>(bands.meshLod, static_cast<unsigned int>(prototype.lods.size()) - 1) : 0;
                        if (prototype.ready[entityIndex]) {
                            cell.wantsGpu = true;
                            if (cell.gpu && cell.gpu->getRender()->isCreated() && !cell.gpuDirty) {
                                const MeshComponent& lodMesh = scene->getComponent<MeshComponent>(prototype.lods[entityIndex]);
                                if (lodMesh.loaded) {
                                    FoliageDrawItem item;
                                    item.buffer = cell.gpu->getRender();
                                    item.first = range.first;
                                    item.count = range.count;
                                    item.level = explicitLod ? 0 : bands.meshLod;
                                    entityDraws[prototype.slots[entityIndex]].push_back(item);
                                    list.draws += std::max(lodMesh.numSubmeshes, 1u);
                                    meshTrees += range.count;
                                }
                            }
                        }
                    }
                    if (bands.impostor) {
                        impostorPieces[range.prototype].push_back(ImpostorPiece{&cell, &range});
                    }
                    if (bands.mesh || bands.impostor) visibleTrees += range.count;
                }
            }

            for (size_t p = 0; p < region.prototypes.size(); p++) {
                TreePrototypeState& prototype = region.prototypes[p];
                if (impostorPieces[p].empty() || prototype.impostor == NULL_ENTITY) continue;
                const MeshComponent& mesh = scene->getComponent<MeshComponent>(prototype.impostor);
                if (!mesh.loaded) continue;
                const uint32_t first = static_cast<uint32_t>(list.stream.size());
                for (const ImpostorPiece& piece : impostorPieces[p]) {
                    list.stream.insert(list.stream.end(), piece.cell->instances.begin() + piece.range->first,
                                       piece.cell->instances.begin() + piece.range->first + piece.range->count);
                }
                FoliageDrawItem item;
                item.buffer = list.buffer.getRender();
                item.first = first;
                item.count = static_cast<uint32_t>(list.stream.size()) - first;
                entityDraws[prototype.impostorSlot].push_back(item);
                list.draws++;
            }

            // into the list, entity by entity
            const auto emit = [&](uint32_t slot) {
                std::vector<FoliageDrawItem>& draws = entityDraws[slot];
                if (draws.empty()) return;
                const uint32_t firstItem = static_cast<uint32_t>(list.items.size());
                list.items.insert(list.items.end(), draws.begin(), draws.end());
                list.ranges[slot] = {firstItem, static_cast<uint32_t>(draws.size())};
            };
            for (const TreePrototypeState& prototype : region.prototypes) {
                for (uint32_t slot : prototype.slots) emit(slot);
                if (prototype.impostor != NULL_ENTITY) emit(prototype.impostorSlot);
            }

            list.treesVisible += visibleTrees;
            list.meshTrees += meshTrees;
        }
    }
};

// ------------------------------------------------------------------------------- the system

FoliageSystem::FoliageSystem(Scene* scene): SubSystem(scene), impl(std::make_unique<Impl>(this, scene)) {
    declareHookType(scene->getComponentId<TerrainComponent>());
    registerFoliageOps();
}

FoliageSystem::~FoliageSystem() {
    reportMemory(0, 0);
}

// R42: every scene's foliage in one record of each residency. A scene with no Terrain never writes
// to them.
void FoliageSystem::reportMemory(uint64_t cpuBytes, uint64_t gpuBytes) {
    if (cpuBytes == reportedCpuBytes && gpuBytes == reportedGpuBytes) return;
    static memory::TrackedBytes cpuRecord("foliage-cpu", memory::Residency::Cpu);
    static memory::TrackedBytes gpuRecord("foliage-gpu", memory::Residency::Gpu);
    cpuRecord.add(cpuBytes, 0);
    cpuRecord.subtract(reportedCpuBytes, 0);
    gpuRecord.add(gpuBytes, 0);
    gpuRecord.subtract(reportedGpuBytes, 0);
    reportedCpuBytes = cpuBytes;
    reportedGpuBytes = gpuBytes;
}

void FoliageSystem::load() {
    update(0);
}

void FoliageSystem::destroy() {
    for (std::unique_ptr<RegionState>& region : impl->regions) impl->destroyRegion(*region);
    impl->regions.clear();
    impl->dying.clear();
    impl->prototypeEntities.clear();
    drawables = false;
}

void FoliageSystem::draw() {
}

void FoliageSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
    if (componentId != scene->getComponentId<TerrainComponent>()) return;
    for (size_t i = 0; i < impl->regions.size(); i++) {
        if (impl->regions[i]->owner == entity) {
            impl->destroyRegion(*impl->regions[i]);
            impl->regions.erase(impl->regions.begin() + static_cast<std::ptrdiff_t>(i));
            i--;
            residencyRevision++;
        }
    }
    drawables = !impl->regions.empty();
}

void FoliageSystem::update(double dt) {
    auto terrains = scene->getComponentArray<TerrainComponent>();
    if (terrains->size() == 0 && impl->regions.empty()) {
        return;
    }

    bool any = false;
    for (int i = 0; i < terrains->size(); i++) {
        TerrainComponent& terrain = terrains->getComponentFromIndex(i);
        const Entity entity = terrains->getEntity(i);
        RegionState* state = impl->regionOf(entity);

        // R38, and T4 R27: a Terrain World's foliage is its resident tiles' regions, never one of its own
        if ((terrain.foliageLayers.empty() && terrain.treePrototypes.empty() && terrain.treeData.empty()) || terrain.world) {
            // R38: a Terrain with no layer and no prototype has no state at all
            if (state) {
                impl->destroyRegion(*state);
                for (size_t r = 0; r < impl->regions.size(); r++) {
                    if (impl->regions[r].get() == state) {
                        impl->regions.erase(impl->regions.begin() + static_cast<std::ptrdiff_t>(r));
                        break;
                    }
                }
                residencyRevision++;
            }
            terrain.needUpdateFoliage = false;
            continue;
        }

        any = true;
        if (!state) {
            auto created = std::make_unique<RegionState>();
            created->owner = entity;
            created->region = std::make_shared<TerrainFoliageRegion>(scene, entity);
            state = created.get();
            impl->regions.push_back(std::move(created));
            Impl* owner = impl.get();
            state->events = TerrainEditEvents::subscribe(scene, [owner](Entity from, MapId map, TexelRect rect) { owner->onEdit(from, map, rect); });
        }

        Transform* transform = scene->findComponent<Transform>(entity);
        if (transform) {
            impl->updateRegion(*state, terrain, *transform, static_cast<float>(dt));
        }
    }
    drawables = any;
    if (onTreeHit.empty() == contactsConnected) updateContactSubscription();

    // A prototype whose mesh loads after its cells were listed (its shaders still building) draws
    // nothing until the views look again; a still camera's signature would not make them (R9)
    uint64_t loaded = FNV1A64_SEED;
    const auto mixLoaded = [&](Entity entity) {
        const MeshComponent* mesh = entity != NULL_ENTITY ? scene->findComponent<MeshComponent>(entity) : nullptr;
        const uint8_t state = mesh && mesh->loaded ? 1 : 0;
        loaded = fnv1a64(&state, sizeof(state), loaded);
    };
    for (const std::unique_ptr<RegionState>& region : impl->regions) {
        for (const DetailLayerState& layer : region->layers) mixLoaded(layer.entity);
        for (const TreePrototypeState& prototype : region->prototypes) {
            for (Entity lod : prototype.lods) mixLoaded(lod);
            mixLoaded(prototype.impostor);
        }
    }
    if (loaded != loadedSignature) {
        loadedSignature = loaded;
        residencyRevision++;
    }

    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
    for (const std::unique_ptr<RegionState>& region : impl->regions) {
        cpuBytes += region->stats.cpuBytes;
        gpuBytes += region->stats.gpuBytes;
    }
    reportMemory(cpuBytes, gpuBytes);

    // F11: a removed region's cells go a few per frame
    if (!impl->dying.empty()) {
        RegionState& region = *impl->dying.back();
        int released = 0;
        for (auto it = region.cells.begin(); it != region.cells.end() && released < 16; released++) {
            impl->releaseCell(it->second);
            it = region.cells.erase(it);
        }
        if (region.cells.empty()) impl->dying.pop_back();
    }
}

bool FoliageSystem::setPreviewEntity(Entity entity) {
    if (impl->previewEntity == entity) return false;
    if (RegionState* before = impl->regionOf(impl->previewEntity)) before->refill = true;
    impl->previewEntity = entity;
    if (RegionState* after = impl->regionOf(entity)) after->refill = true;
    residencyRevision++;
    return true;
}

bool FoliageSystem::hasPendingUpdates() const {
    for (const std::unique_ptr<RegionState>& region : impl->regions) {
        if (!region->active) continue;
        if (region->stats.pendingCells > 0 || region->storeLoading) return true;
        for (const DetailLayerState& layer : region->layers) {
            if (layer.entity == NULL_ENTITY) continue;
            if (layer.needScan || !layer.pending.empty() || layer.waitingForMap) return true;
            if (!layer.meshReady && !layer.loadFailed) return true;
        }
    }
    return false;
}

std::vector<Entity> FoliageSystem::getEntities(Entity terrainEntity) const {
    std::vector<Entity> entities;
    for (const std::unique_ptr<RegionState>& region : impl->regions) {
        if (region->owner != terrainEntity) continue;
        for (const DetailLayerState& layer : region->layers) {
            if (layer.entity != NULL_ENTITY) entities.push_back(layer.entity);
        }
        for (const TreePrototypeState& prototype : region->prototypes) {
            for (Entity entity : prototype.lods) {
                if (entity != NULL_ENTITY) entities.push_back(entity);
            }
            if (prototype.impostor != NULL_ENTITY) entities.push_back(prototype.impostor);
        }
    }
    return entities;
}

Entity FoliageSystem::getOwner(Entity foliageEntity) const {
    if (foliageEntity == NULL_ENTITY) return NULL_ENTITY;
    const auto it = impl->prototypeEntities.find(foliageEntity);
    return it != impl->prototypeEntities.end() ? it->second.region->owner : NULL_ENTITY;
}

void FoliageSystem::reloadMesh(const std::string& filename) {
    const std::string key = MeshSystem::getModelFilenameKey(filename);
    for (std::unique_ptr<RegionState>& region : impl->regions) {
        for (DetailLayerState& layer : region->layers) {
            if (!layer.loadedPath.empty() && MeshSystem::getModelFilenameKey(layer.loadedPath) == key) {
                // A path that no longer matches the layer makes the next update load it again
                layer.loadedPath.clear();
                layer.meshReady = false;
                layer.loadFailed = false;
            }
        }
        for (TreePrototypeState& prototype : region->prototypes) {
            for (size_t l = 0; l < prototype.paths.size(); l++) {
                if (MeshSystem::getModelFilenameKey(prototype.paths[l]) == key) {
                    prototype.ready[l] = 0;
                    prototype.failed[l] = 0;
                }
            }
        }
    }
}

uint64_t FoliageSystem::getInstanceCount(Entity terrainEntity, int layer) const {
    uint64_t total = 0;
    for (const std::unique_ptr<RegionState>& region : impl->regions) {
        if (region->owner != terrainEntity) continue;
        if (layer >= 0 && layer < static_cast<int>(region->layers.size())) total += region->layers[layer].resident;
    }
    return total;
}

void FoliageSystem::addRegion(Entity owner, std::shared_ptr<FoliageRegion> region) {
    auto created = std::make_unique<RegionState>();
    created->owner = owner;
    created->region = std::move(region);
    created->store = created->region->trees();
    impl->regions.push_back(std::move(created));
    residencyRevision++;
}

void FoliageSystem::removeRegion(Entity owner, uint32_t regionKey) {
    for (size_t i = 0; i < impl->regions.size(); i++) {
        RegionState& region = *impl->regions[i];
        if (region.owner == owner && region.region->regionKey() == regionKey) {
            for (DetailLayerState& layer : region.layers) impl->destroyLayer(layer);
            region.layers.clear();
            for (TreePrototypeState& prototype : region.prototypes) impl->destroyPrototype(prototype);
            region.prototypes.clear();
            impl->dying.push_back(std::move(impl->regions[i]));
            impl->regions.erase(impl->regions.begin() + static_cast<std::ptrdiff_t>(i));
            residencyRevision++;
            return;
        }
    }
}

// ---- trees

std::shared_ptr<TreeStore> FoliageSystem::getTreeStore(Entity terrainEntity) {
    RegionState* state = impl->regionOf(terrainEntity);
    return state ? state->store : nullptr;
}

std::shared_ptr<const TreeStore> FoliageSystem::getTreeStore(Entity terrainEntity) const {
    RegionState* state = impl->regionOf(terrainEntity);
    return state ? state->store : nullptr;
}

std::shared_ptr<TreeStore> FoliageSystem::getOrCreateTreeStore(Entity terrainEntity) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return nullptr;
    if (state->storeRefused && !state->storeOverwriteAllowed) return nullptr;
    if (!state->store) {
        state->store = std::make_shared<TreeStore>();
        state->indexing = state->store;
        state->cellsReady = true;
    }
    impl->prepareWritableStore(*state);
    return state->store;
}

bool FoliageSystem::isTreeStoreRefused(Entity terrainEntity, std::string* error) const {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state || !state->storeRefused) return false;
    if (error) *error = state->storeError;
    return !state->storeOverwriteAllowed;
}

void FoliageSystem::allowOverwriteRefusedStore(Entity terrainEntity) {
    if (RegionState* state = impl->regionOf(terrainEntity)) state->storeOverwriteAllowed = true;
}

bool FoliageSystem::isTreeStoreLoading(Entity terrainEntity) const {
    RegionState* state = impl->regionOf(terrainEntity);
    return state && (state->storeLoading || (state->store && !state->cellsReady));
}

bool FoliageSystem::isTreeStoreDirty(Entity terrainEntity) const {
    RegionState* state = impl->regionOf(terrainEntity);
    return state && state->storeDirty;
}

void FoliageSystem::clearTreeStoreDirty(Entity terrainEntity) {
    if (RegionState* state = impl->regionOf(terrainEntity)) state->storeDirty = false;
}

void FoliageSystem::treeFileWritten(Entity terrainEntity, const std::string& path) {
    if (RegionState* state = impl->regionOf(terrainEntity)) {
        state->storePath = path;
        state->storeDirty = false;
        state->storeRefused = false;
    }
}

void FoliageSystem::shareTreeStore(Entity terrainEntity, std::shared_ptr<TreeStore> store) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) {
        auto created = std::make_unique<RegionState>();
        created->owner = terrainEntity;
        created->region = std::make_shared<TerrainFoliageRegion>(scene, terrainEntity);
        state = created.get();
        impl->regions.push_back(std::move(created));
        Impl* owner = impl.get();
        state->events = TerrainEditEvents::subscribe(scene, [owner](Entity from, MapId map, TexelRect rect) { owner->onEdit(from, map, rect); });
    }
    state->store = std::move(store);
    state->storeShared = true;
    state->storePath = scene->getComponent<TerrainComponent>(terrainEntity).treeData;
    state->cellsReady = false;
    state->indexing.reset();
}

void FoliageSystem::treesChanged(Entity terrainEntity, const TreeDelta& delta) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return;
    state->storeDirty = true;
    if (!state->cellsReady) return;
    for (const TreeRecord& record : delta.removed) impl->markTreeCellDirtyForRecord(*state, record, false, true);
    for (const TreeRecord& record : delta.added) impl->markTreeCellDirtyForRecord(*state, record, true, false);
    residencyRevision++;
}

void FoliageSystem::treeStoreReplaced(Entity terrainEntity, std::shared_ptr<TreeStore> store) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return;
    if (store) {
        state->store = std::move(store);
        state->storeShared = false;
    }
    state->storeDirty = true;
    state->cellsReady = false;
    state->indexing.reset();
    state->indexTask = jobs::Task<std::shared_ptr<TreeCellMap>>();
}

// ---- runtime overlay

bool FoliageSystem::getTreePose(Entity terrainEntity, TreeId id, FoliageInstanceInfo& out) const {
    RegionState* state = impl->regionOf(terrainEntity, treeIdRegion(id));
    if (!state) return false;
    const uint32_t recordId = treeIdRecord(id);
    if (state->runtime.removed.count(recordId)) return false;
    const TreeRecord* record = impl->findRecord(*state, recordId);
    if (!record) return false;
    const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
    FoliageInstance instance;
    float normal[3] = {0.0f, 1.0f, 0.0f};
    const bool leans = (record->flags & TREE_ALIGN_TO_NORMAL) != 0;
    if (leans) {
        const SurfaceSample sample = state->region->surface().sample(foliageTreeLocalX(*record, terrain.terrainSize), foliageTreeLocalZ(*record, terrain.terrainSize));
        normal[0] = sample.normal.x;
        normal[1] = sample.normal.y;
        normal[2] = sample.normal.z;
    }
    foliageInstanceFromTree(*record, terrain.terrainSize, terrain.maxHeight, 0, leans ? normal : nullptr, instance);

    const Transform& transform = scene->getComponent<Transform>(terrainEntity);
    out.id = id;
    out.tree = true;
    out.prototype = Impl::prototypeIndexOf(terrain, record->prototypeId);
    out.position = transform.modelMatrix * Vector3(instance.position[0], instance.position[1], instance.position[2]);
    out.rotation = transform.worldRotation * Quaternion(instance.rotation[3] / 32767.0f, instance.rotation[0] / 32767.0f, instance.rotation[1] / 32767.0f, instance.rotation[2] / 32767.0f);
    out.scale = instance.scale;
    return true;
}

// R30: in the same frame: its cell's array, buffer and body
void FoliageSystem::rebuildCellNow(Entity terrainEntity, const TreeRecord& record) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return;
    const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
    const float size = terrain.terrainSize;
    const int x = static_cast<int>(std::floor(foliageTreeLocalX(record, size) / FOLIAGE_TREE_CELL_SIZE));
    const int z = static_cast<int>(std::floor(foliageTreeLocalZ(record, size) / FOLIAGE_TREE_CELL_SIZE));
    const auto it = state->cells.find(foliageTreeCellKey(x, z));
    if (it != state->cells.end() && it->second.resident) {
        impl->buildCell(*state, it->second, terrain);
    }
    if (it != state->cells.end() && it->second.resident && state->hadBodies) {
        impl->rebuildBody(*state, it->second, terrain);
    }
}

std::optional<FoliageInstanceInfo> FoliageSystem::removeTreeInstance(Entity terrainEntity, TreeId id, bool updateNavigation) {
    FoliageInstanceInfo pose;
    if (!getTreePose(terrainEntity, id, pose)) return std::nullopt;
    RegionState* state = impl->regionOf(terrainEntity, treeIdRegion(id));
    const uint32_t recordId = treeIdRecord(id);
    state->runtime.removed.insert(recordId);
    if (const TreeRecord* record = impl->findRecord(*state, recordId)) {
        impl->markTreeCellDirtyForRecord(*state, *record, false, false);
        rebuildCellNow(terrainEntity, *record);
    }
    residencyRevision++;
    onTreeRemoved(terrainEntity, id, pose);
    if (state->removedHandler) state->removedHandler(id, pose);
    if (updateNavigation) {
        foliagephysics::updateNavigationAround(*scene, pose.position, 4.0f * std::max(pose.scale, 1.0f));
    }
    return pose;
}

bool FoliageSystem::restoreTreeInstance(Entity terrainEntity, TreeId id) {
    RegionState* state = impl->regionOf(terrainEntity, treeIdRegion(id));
    if (!state) return false;
    const uint32_t recordId = treeIdRecord(id);
    if (state->runtime.removed.erase(recordId) == 0) return false;
    if (const TreeRecord* record = impl->findRecord(*state, recordId)) {
        impl->markTreeCellDirtyForRecord(*state, *record, false, false);
        rebuildCellNow(terrainEntity, *record);
    }
    residencyRevision++;
    return true;
}

TreeId FoliageSystem::addTreeInstance(Entity terrainEntity, int prototype, const Vector3& position, float yawDegrees, float scale) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return 0;
    const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
    if (prototype < 0 || prototype >= static_cast<int>(terrain.treePrototypes.size())) return 0;
    const Transform& transform = scene->getComponent<Transform>(terrainEntity);
    const Matrix4 inverse = transform.modelMatrix.inverse();
    const Vector3 local = inverse.isValid() ? inverse * position : position;

    const float size = terrain.terrainSize;
    const SurfaceSample sample = state->region->surface().sample(local.x, local.z);
    TreeRecord record{};
    record.id = state->runtime.nextAdded++;
    foliageSetTreeSurface(record, size, terrain.maxHeight, local.x, local.z, sample.height);
    record.yOffset = local.y > sample.height ? local.y - sample.height : 0.0f;
    record.scale = scale;
    float rotation[4];
    const float up[3] = {0.0f, 1.0f, 0.0f};
    foliageOrientation(yawDegrees / 360.0f, 0.0f, up, rotation);
    foliageQuantizeRotation(rotation, record.rotation);
    record.prototypeId = static_cast<uint16_t>(terrain.treePrototypes[prototype].id);
    record.heightRatio = 128;
    record.flags = TREE_SNAP_TO_TERRAIN;
    std::memset(record.color, 255, sizeof(record.color));
    state->runtime.added.push_back(record);

    impl->markTreeCellDirtyForRecord(*state, record, true, false);
    state->residentDirty = true;
    if (!state->store && !state->storeLoading) state->cellsReady = true;
    residencyRevision++;
    return makeTreeId(state->region->regionKey(), record.id);
}

void FoliageSystem::clearDetailsInRadius(Entity terrainEntity, const Vector3& position, float radius, int layer) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return;
    const Transform& transform = scene->getComponent<Transform>(terrainEntity);
    const Matrix4 inverse = transform.modelMatrix.inverse();
    const Vector3 local = inverse.isValid() ? inverse * position : position;

    if (static_cast<int>(state->runtime.discs.size()) >= MAX_RUNTIME_DISCS) {
        // F10's limit: the oldest makes way, and what it cleared comes back when its cell is placed again
        state->runtime.discs.erase(state->runtime.discs.begin());
    }
    state->runtime.discs.push_back(TreeRuntimeState::Disc{local.x, local.z, radius, layer});

    for (size_t l = 0; l < state->layers.size(); l++) {
        if (layer >= 0 && layer != static_cast<int>(l)) continue;
        DetailLayerState& layerState = state->layers[l];
        for (DetailSlot& slot : layerState.ring) {
            if (!slot.instances || slot.instances->empty()) continue;
            const float originX = static_cast<float>(slot.x) * FOLIAGE_DETAIL_CELL_SIZE;
            const float originZ = static_cast<float>(slot.z) * FOLIAGE_DETAIL_CELL_SIZE;
            const float nearestX = std::clamp(local.x, originX, originX + FOLIAGE_DETAIL_CELL_SIZE);
            const float nearestZ = std::clamp(local.z, originZ, originZ + FOLIAGE_DETAIL_CELL_SIZE);
            const float dx = local.x - nearestX;
            const float dz = local.z - nearestZ;
            if (dx * dx + dz * dz > radius * radius) continue;

            auto kept = std::make_shared<std::vector<FoliageInstance>>();
            kept->reserve(slot.instances->size());
            for (const FoliageInstance& instance : *slot.instances) {
                const float ix = instance.position[0] - local.x;
                const float iz = instance.position[2] - local.z;
                if (ix * ix + iz * iz > radius * radius) kept->push_back(instance);
            }
            layerState.resident -= slot.instances->size() - kept->size();
            slot.instances = std::move(kept);
            layerState.boundsDirty = true;
            residencyRevision++;
        }
    }
}

void FoliageSystem::restoreDetails(Entity terrainEntity) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state || state->runtime.discs.empty()) return;
    state->runtime.discs.clear();
    for (DetailLayerState& layer : state->layers) {
        for (DetailSlot& slot : layer.ring) {
            impl->cancelSlot(slot);
            slot.assigned = false;
        }
        layer.pending.clear();
        layer.needScan = true;
    }
}

// R31's string: readable, and as compact as a text a game keeps with its saves
std::string FoliageSystem::getRuntimeState(Entity terrainEntity) const {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return "JEFS1;;;";
    std::ostringstream out;
    out << "JEFS1;";
    std::vector<uint32_t> removed(state->runtime.removed.begin(), state->runtime.removed.end());
    std::sort(removed.begin(), removed.end());
    for (size_t i = 0; i < removed.size(); i++) out << (i ? "," : "") << removed[i];
    out << ";";
    const float size = state->terrainSize;
    for (size_t i = 0; i < state->runtime.added.size(); i++) {
        const TreeRecord& record = state->runtime.added[i];
        out << (i ? "," : "") << record.id << ":" << record.prototypeId << ":" << foliageTreeLocalX(record, size) << ":" << foliageTreeLocalZ(record, size)
            << ":" << record.scale << ":" << record.rotation[1] << ":" << record.rotation[3] << ":" << record.yOffset;
    }
    out << ";";
    for (size_t i = 0; i < state->runtime.discs.size(); i++) {
        const TreeRuntimeState::Disc& disc = state->runtime.discs[i];
        out << (i ? "," : "") << disc.x << ":" << disc.z << ":" << disc.radius << ":" << disc.layer;
    }
    return out.str();
}

bool FoliageSystem::setRuntimeState(Entity terrainEntity, const std::string& text) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return false;

    const auto split = [](const std::string& value, char separator) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            const size_t next = value.find(separator, start);
            if (next == std::string::npos) {
                parts.push_back(value.substr(start));
                return parts;
            }
            parts.push_back(value.substr(start, next - start));
            start = next + 1;
        }
    };

    const std::vector<std::string> sections = split(text, ';');
    if (sections.size() != 4 || sections[0] != "JEFS1") return false;

    TreeRuntimeState next;
    try {
        if (!sections[1].empty()) {
            for (const std::string& id : split(sections[1], ',')) next.removed.insert(static_cast<uint32_t>(std::stoul(id)));
        }
        if (!sections[2].empty()) {
            for (const std::string& item : split(sections[2], ',')) {
                const std::vector<std::string> fields = split(item, ':');
                if (fields.size() != 8) return false;
                TreeRecord record{};
                record.id = static_cast<uint32_t>(std::stoul(fields[0]));
                if (record.id < TREE_RUNTIME_ID_BASE) return false;
                record.prototypeId = static_cast<uint16_t>(std::stoul(fields[1]));
                const float x = std::stof(fields[2]);
                const float z = std::stof(fields[3]);
                const SurfaceSample sample = state->region->surface().sample(x, z);
                foliageSetTreeSurface(record, state->terrainSize, state->terrainMaxHeight, x, z, sample.height);
                record.scale = std::stof(fields[4]);
                record.rotation[1] = static_cast<int16_t>(std::stoi(fields[5]));
                record.rotation[3] = static_cast<int16_t>(std::stoi(fields[6]));
                record.yOffset = std::stof(fields[7]);
                record.heightRatio = 128;
                record.flags = TREE_SNAP_TO_TERRAIN;
                std::memset(record.color, 255, sizeof(record.color));
                next.added.push_back(record);
                next.nextAdded = std::max(next.nextAdded, record.id + 1);
            }
        }
        if (!sections[3].empty()) {
            for (const std::string& item : split(sections[3], ',')) {
                const std::vector<std::string> fields = split(item, ':');
                if (fields.size() != 4) return false;
                next.discs.push_back(TreeRuntimeState::Disc{std::stof(fields[0]), std::stof(fields[1]), std::stof(fields[2]), std::stoi(fields[3])});
            }
        }
    } catch (const std::exception&) {
        return false;
    }

    clearRuntimeState(terrainEntity);
    state->runtime = std::move(next);
    for (const TreeRecord& record : state->runtime.added) impl->markTreeCellDirtyForRecord(*state, record, true, false);
    for (uint32_t id : state->runtime.removed) {
        if (const TreeRecord* record = impl->findRecord(*state, id)) {
            impl->markTreeCellDirtyForRecord(*state, *record, false, false);
            rebuildCellNow(terrainEntity, *record);
        }
    }
    if (!state->runtime.discs.empty()) {
        for (DetailLayerState& layer : state->layers) {
            for (DetailSlot& slot : layer.ring) {
                impl->cancelSlot(slot);
                slot.assigned = false;
            }
            layer.pending.clear();
            layer.needScan = true;
        }
    }
    state->residentDirty = true;
    residencyRevision++;
    return true;
}

void FoliageSystem::clearRuntimeState(Entity terrainEntity) {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return;
    for (uint32_t id : state->runtime.removed) {
        if (const TreeRecord* record = impl->findRecord(*state, id)) impl->markTreeCellDirtyForRecord(*state, *record, false, false);
    }
    for (const TreeRecord& record : state->runtime.added) impl->markTreeCellDirtyForRecord(*state, record, false, true);
    const bool hadDiscs = !state->runtime.discs.empty();
    state->runtime = TreeRuntimeState();
    if (hadDiscs) {
        for (DetailLayerState& layer : state->layers) {
            for (DetailSlot& slot : layer.ring) {
                impl->cancelSlot(slot);
                slot.assigned = false;
            }
            layer.pending.clear();
            layer.needScan = true;
        }
    }
    residencyRevision++;
}

// ---- queries

std::vector<FoliageInstanceInfo> FoliageSystem::getInstancesInRadius(Entity terrainEntity, const Vector3& position, float radius, int kind, size_t maxResults) const {
    std::vector<FoliageInstanceInfo> found;
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return found;
    const Transform& transform = scene->getComponent<Transform>(terrainEntity);
    const Matrix4 inverse = transform.modelMatrix.inverse();
    if (!inverse.isValid()) return found;
    const Vector3 local = inverse * position;
    const float radiusSquared = radius * radius;

    struct Near { float distance; FoliageInstanceInfo info; };
    std::vector<Near> near;

    const auto makeInfo = [&](const FoliageInstance& instance, FoliageInstanceInfo& info) {
        info.position = transform.modelMatrix * Vector3(instance.position[0], instance.position[1], instance.position[2]);
        info.rotation = transform.worldRotation * Quaternion(instance.rotation[3] / 32767.0f, instance.rotation[0] / 32767.0f, instance.rotation[1] / 32767.0f, instance.rotation[2] / 32767.0f);
        info.scale = instance.scale;
    };

    if (kind == 0 || kind == 2) {
        for (int64_t key : state->residentOrder) {
            const auto it = state->cells.find(key);
            if (it == state->cells.end()) continue;
            const TreeCell& cell = it->second;
            const float closestX = std::clamp(local.x, cell.boxMin[0], cell.boxMax[0]) - local.x;
            const float closestZ = std::clamp(local.z, cell.boxMin[2], cell.boxMax[2]) - local.z;
            if (closestX * closestX + closestZ * closestZ > radiusSquared) continue;
            for (const TreeProtoRange& range : cell.ranges) {
                for (uint32_t i = range.first; i < range.first + range.count; i++) {
                    const FoliageInstance& instance = cell.instances[i];
                    const float dx = instance.position[0] - local.x;
                    const float dz = instance.position[2] - local.z;
                    const float squared = dx * dx + dz * dz;
                    if (squared > radiusSquared) continue;
                    Near entry;
                    entry.distance = squared;
                    entry.info.tree = true;
                    entry.info.id = makeTreeId(state->region->regionKey(), cell.slotIds[i]);
                    entry.info.prototype = range.prototype;
                    makeInfo(instance, entry.info);
                    near.push_back(std::move(entry));
                }
            }
        }
    }

    if (kind == 1 || kind == 2) {
        for (size_t l = 0; l < state->layers.size(); l++) {
            for (const DetailSlot& slot : state->layers[l].ring) {
                if (!slot.instances || slot.instances->empty()) continue;
                const float originX = static_cast<float>(slot.x) * FOLIAGE_DETAIL_CELL_SIZE;
                const float originZ = static_cast<float>(slot.z) * FOLIAGE_DETAIL_CELL_SIZE;
                const float cx = std::clamp(local.x, originX, originX + FOLIAGE_DETAIL_CELL_SIZE) - local.x;
                const float cz = std::clamp(local.z, originZ, originZ + FOLIAGE_DETAIL_CELL_SIZE) - local.z;
                if (cx * cx + cz * cz > radiusSquared) continue;
                for (const FoliageInstance& instance : *slot.instances) {
                    const float dx = instance.position[0] - local.x;
                    const float dz = instance.position[2] - local.z;
                    const float squared = dx * dx + dz * dz;
                    if (squared > radiusSquared) continue;
                    Near entry;
                    entry.distance = squared;
                    entry.info.tree = false;
                    entry.info.id = 0;
                    entry.info.layer = static_cast<int>(l);
                    makeInfo(instance, entry.info);
                    near.push_back(std::move(entry));
                }
            }
        }
    }

    std::stable_sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
    if (near.size() > maxResults) near.resize(maxResults);
    found.reserve(near.size());
    for (Near& entry : near) found.push_back(std::move(entry.info));
    return found;
}

namespace {

    // The instance's matrix in terrain-local space: what foliage_instance.glsl builds
    Matrix4 foliageInstanceMatrix(const FoliageInstance& instance) {
        const float x = instance.rotation[0] / 32767.0f;
        const float y = instance.rotation[1] / 32767.0f;
        const float z = instance.rotation[2] / 32767.0f;
        const float w = instance.rotation[3] / 32767.0f;
        const Quaternion rotation(w, x, y, z);
        const float height = instance.scale * static_cast<float>(instance.heightRatio) / 128.0f;
        return Matrix4::translateMatrix(Vector3(instance.position[0], instance.position[1], instance.position[2])) *
               rotation.getRotationMatrix() *
               Matrix4::scaleMatrix(Vector3(instance.scale, height, instance.scale));
    }

    bool foliageRayBox(const Vector3& origin, const Vector3& direction, const float boxMin[3], const float boxMax[3], float maxT) {
        float tMin = 0.0f, tMax = maxT;
        const float o[3] = {origin.x, origin.y, origin.z};
        const float d[3] = {direction.x, direction.y, direction.z};
        for (int a = 0; a < 3; a++) {
            if (std::abs(d[a]) < 1e-8f) {
                if (o[a] < boxMin[a] || o[a] > boxMax[a]) return false;
                continue;
            }
            float t0 = (boxMin[a] - o[a]) / d[a];
            float t1 = (boxMax[a] - o[a]) / d[a];
            if (t0 > t1) std::swap(t0, t1);
            tMin = std::max(tMin, t0);
            tMax = std::min(tMax, t1);
            if (tMin > tMax) return false;
        }
        return true;
    }

}

void FoliageSystem::forEachTreeAlongRay(const Ray& ray, float maxDistance, const std::function<bool(Entity, TreeId, int, const Matrix4&, float)>& visit) const {
    struct Candidate { float distance; Entity terrain; TreeId id; int prototype; Matrix4 world; };
    std::vector<Candidate> candidates;

    for (const std::unique_ptr<RegionState>& regionPtr : impl->regions) {
        RegionState& region = *regionPtr;
        if (!region.active || region.residentOrder.empty()) continue;
        const Vector3 origin = region.inverseModel * ray.getOrigin();
        const Vector3 through = region.inverseModel * (ray.getOrigin() + ray.getDirection().normalized());
        Vector3 direction = through - origin;
        const float scale = direction.length();
        if (!(scale > 0.0f)) continue;
        direction = direction * (1.0f / scale);
        const float localMax = maxDistance * scale;

        for (int64_t key : region.residentOrder) {
            const auto it = region.cells.find(key);
            if (it == region.cells.end()) continue;
            const TreeCell& cell = it->second;
            if (cell.instances.empty()) continue;
            float reach = 0.0f;
            for (const TreeProtoRange& range : cell.ranges) {
                if (range.prototype < static_cast<int>(region.prototypes.size())) {
                    const TreePrototypeState& prototype = region.prototypes[range.prototype];
                    reach = std::max(reach, (prototype.radius + prototype.center.length()) * range.maxScale);
                }
            }
            const float boxMin[3] = {cell.boxMin[0] - reach, cell.boxMin[1] - reach, cell.boxMin[2] - reach};
            const float boxMax[3] = {cell.boxMax[0] + reach, cell.boxMax[1] + reach, cell.boxMax[2] + reach};
            if (!foliageRayBox(origin, direction, boxMin, boxMax, localMax)) continue;

            for (const TreeProtoRange& range : cell.ranges) {
                if (range.prototype >= static_cast<int>(region.prototypes.size())) continue;
                const TreePrototypeState& prototype = region.prototypes[range.prototype];
                for (uint32_t i = range.first; i < range.first + range.count; i++) {
                    const FoliageInstance& instance = cell.instances[i];
                    const float tall = instance.scale * std::max(1.0f, static_cast<float>(instance.heightRatio) / 128.0f);
                    const Matrix4 local = foliageInstanceMatrix(instance);
                    const Vector3 center = local * prototype.center;
                    const float radius = prototype.radius * tall;
                    const Vector3 toCenter = center - origin;
                    const float along = toCenter.dotProduct(direction);
                    if (along < -radius || along - radius > localMax) continue;
                    const float squared = toCenter.squaredLength() - along * along;
                    if (squared > radius * radius) continue;
                    candidates.push_back(Candidate{std::max(along - radius, 0.0f) / scale, region.owner, makeTreeId(region.region->regionKey(), cell.slotIds[i]), range.prototype, region.model * local});
                }
            }
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    for (const Candidate& candidate : candidates) {
        if (!visit(candidate.terrain, candidate.id, candidate.prototype, candidate.world, candidate.distance)) return;
    }
}

void FoliageSystem::forEachDetailAlongRay(const Ray& ray, float maxDistance, const std::function<bool(Entity, int, const Matrix4&, float)>& visit) const {
    struct Candidate { float distance; Entity terrain; int layer; Matrix4 world; };
    std::vector<Candidate> candidates;
    for (const std::unique_ptr<RegionState>& regionPtr : impl->regions) {
        RegionState& region = *regionPtr;
        if (!region.active) continue;
        const Vector3 origin = region.inverseModel * ray.getOrigin();
        const Vector3 through = region.inverseModel * (ray.getOrigin() + ray.getDirection().normalized());
        Vector3 direction = through - origin;
        const float scale = direction.length();
        if (!(scale > 0.0f)) continue;
        direction = direction * (1.0f / scale);
        const float localMax = maxDistance * scale;
        for (size_t l = 0; l < region.layers.size(); l++) {
            const DetailLayerState& layer = region.layers[l];
            const float reach = (layer.radius + layer.center.length()) * std::max(layer.params.maxScale, 0.001f) * std::max(1.0f, layer.params.maxHeightRatio);
            for (const DetailSlot& slot : layer.ring) {
                if (!slot.instances || slot.instances->empty()) continue;
                const float boxMin[3] = {static_cast<float>(slot.x) * FOLIAGE_DETAIL_CELL_SIZE - reach, slot.minY - reach, static_cast<float>(slot.z) * FOLIAGE_DETAIL_CELL_SIZE - reach};
                const float boxMax[3] = {boxMin[0] + FOLIAGE_DETAIL_CELL_SIZE + 2.0f * reach, slot.maxY + reach, boxMin[2] + FOLIAGE_DETAIL_CELL_SIZE + 2.0f * reach};
                if (!foliageRayBox(origin, direction, boxMin, boxMax, localMax)) continue;
                for (const FoliageInstance& instance : *slot.instances) {
                    const float tall = instance.scale * std::max(1.0f, static_cast<float>(instance.heightRatio) / 128.0f);
                    const Matrix4 local = foliageInstanceMatrix(instance);
                    const Vector3 center = local * layer.center;
                    const float radius = layer.radius * tall;
                    const Vector3 toCenter = center - origin;
                    const float along = toCenter.dotProduct(direction);
                    if (along < -radius || along - radius > localMax) continue;
                    if (toCenter.squaredLength() - along * along > radius * radius) continue;
                    candidates.push_back(Candidate{std::max(along - radius, 0.0f) / scale, region.owner, static_cast<int>(l), region.model * local});
                }
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    for (const Candidate& candidate : candidates) {
        if (!visit(candidate.terrain, candidate.layer, candidate.world, candidate.distance)) return;
    }
}

bool FoliageSystem::getPrototypeBounds(Entity terrainEntity, int prototype, Vector3& center, float& radius) const {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state || prototype < 0 || prototype >= static_cast<int>(state->prototypes.size())) return false;
    const TreePrototypeState& entry = state->prototypes[prototype];
    if (entry.ready.empty() || !entry.ready[0]) return false;
    center = entry.center;
    radius = entry.radius;
    return true;
}

Entity FoliageSystem::getPrototypeEntity(Entity terrainEntity, int prototype, int lod) const {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state || prototype < 0 || prototype >= static_cast<int>(state->prototypes.size())) return NULL_ENTITY;
    const TreePrototypeState& entry = state->prototypes[prototype];
    return lod >= 0 && lod < static_cast<int>(entry.lods.size()) ? entry.lods[lod] : NULL_ENTITY;
}

Entity FoliageSystem::getDetailEntity(Entity terrainEntity, int layer) const {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state || layer < 0 || layer >= static_cast<int>(state->layers.size())) return NULL_ENTITY;
    return state->layers[layer].entity;
}

void FoliageSystem::forEachNavObstacle(Entity terrainEntity, const std::function<void(const Vector3&, float, float)>& visit) const {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return;
    const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(terrainEntity);
    const Transform& transform = scene->getComponent<Transform>(terrainEntity);
    const float size = terrain.terrainSize;
    const float worldScale = std::max(std::abs(transform.worldScale.x), std::abs(transform.worldScale.z));

    const auto emit = [&](const TreeRecord& record) {
        if ((record.flags & TREE_HIDDEN_BY_HOLE) || state->runtime.removed.count(record.id)) return;
        const int prototype = Impl::prototypeIndexOf(terrain, record.prototypeId);
        if (prototype < 0 || !terrain.treePrototypes[prototype].navObstacle) return;
        const TerrainTreePrototype& authored = terrain.treePrototypes[prototype];
        const Vector3 base = transform.modelMatrix * Vector3(foliageTreeLocalX(record, size), record.height * terrain.maxHeight + record.yOffset, foliageTreeLocalZ(record, size));
        const float heightRatio = static_cast<float>(record.heightRatio) / 128.0f;
        visit(base, authored.collisionRadius * record.scale * worldScale, authored.collisionHeight * record.scale * heightRatio * std::abs(transform.worldScale.y));
    };
    if (state->store) {
        for (const TreeRecord& record : state->store->records()) emit(record);
    }
    for (const TreeRecord& record : state->runtime.added) emit(record);
}

void FoliageSystem::forEachResidentCell(Entity terrainEntity, const std::function<void(int cellX, int cellZ, std::span<const FoliageInstance> instances, std::span<const uint32_t> ids)>& visit) const {
    RegionState* state = impl->regionOf(terrainEntity);
    if (!state) return;
    for (int64_t key : state->residentOrder) {
        const auto it = state->cells.find(key);
        if (it == state->cells.end()) continue;
        visit(it->second.x, it->second.z, it->second.instances, it->second.slotIds);
    }
}

const FoliageStats& FoliageSystem::getStats(Entity terrainEntity) const {
    if (RegionState* state = impl->regionOf(terrainEntity)) return state->stats;
    static const FoliageStats empty;
    return empty;
}

uint64_t FoliageSystem::getPlacementHash(Entity terrainEntity) const {
    RegionState* state = impl->regionOf(terrainEntity);
    uint64_t hash = foliageHash({});
    if (!state) return hash;
    for (const DetailLayerState& layer : state->layers) {
        std::vector<const DetailSlot*> slots;
        slots.reserve(layer.ring.size());
        for (const DetailSlot& slot : layer.ring) {
            if (slot.instances) slots.push_back(&slot);
        }
        std::sort(slots.begin(), slots.end(), [](const DetailSlot* a, const DetailSlot* b) {
            return a->z != b->z ? a->z < b->z : a->x < b->x;
        });
        for (const DetailSlot* slot : slots) hash = foliageHash(*slot->instances, hash);
    }
    return hash;
}

void FoliageSystem::waitForJobs() {
    for (std::unique_ptr<RegionState>& region : impl->regions) {
        for (DetailLayerState& layer : region->layers) {
            for (int index : layer.pending) {
                DetailSlot& slot = layer.ring[index];
                if (slot.job) slot.task.wait();
            }
        }
        if (region->storeLoading) region->storeTask.wait();
        if (region->indexTask.isValid()) region->indexTask.wait();
    }
}

Vector3 FoliageSystem::getWindVector(const TerrainComponent& terrain, double timeSeconds) const {
    Vector3 direction = terrain.windDirection;
    float strength = terrain.windBending;

    // R20: the first enabled Infinite Directional force field, as Unity's Wind Zone
    auto fields = scene->getComponentArray<ForceFieldComponent>();
    for (int i = 0; i < fields->size(); i++) {
        const ForceFieldComponent& field = fields->getComponentFromIndex(i);
        const Entity entity = fields->getEntity(i);
        if (!field.enabled || field.shape != ForceFieldShapeType::Infinite || field.kind != ForceFieldKind::Directional) continue;
        if (!scene->isEntityActiveInHierarchy(entity)) continue;
        const Transform* transform = scene->findComponent<Transform>(entity);
        direction = transform ? transform->worldRotation * field.direction : field.direction;
        float pulse = 1.0f;
        if (field.pulseFrequency > 0.0f) {
            // a pulse in cycles, wrapped before its sine
            const double cycle = timeSeconds * field.pulseFrequency;
            pulse = 0.5f + 0.5f * detSin(static_cast<float>(cycle - std::floor(cycle)) * DET_TWO_PI);
        }
        strength *= field.strength * pulse;
        break;
    }

    const float length = direction.length();
    if (length > 0.0f) direction = direction * (1.0f / length);
    return direction * strength;
}

bool FoliageSystem::getPrototypeDraw(Entity prototypeEntity, PrototypeDraw& out) const {
    const auto it = impl->prototypeEntities.find(prototypeEntity);
    if (it == impl->prototypeEntities.end()) return false;
    const PrototypeRef& ref = it->second;
    const RegionState& region = *ref.region;
    const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(region.owner);
    if (!terrain) return false;

    out.terrain = region.owner;
    out.wind = region.wind;
    out.prototype = ref.prototype;
    out.layer = ref.layer;
    out.impostor = ref.impostor;

    // the mesh's own height, whichever it is: a plant bends from its base to its top
    const Vector3 center = ref.layer >= 0 ? region.layers[ref.layer].center : (ref.prototype >= 0 && ref.prototype < static_cast<int>(region.prototypes.size()) ? region.prototypes[ref.prototype].center : Vector3());
    const float radius = ref.layer >= 0 ? region.layers[ref.layer].radius : (ref.prototype >= 0 && ref.prototype < static_cast<int>(region.prototypes.size()) ? region.prototypes[ref.prototype].radius : 1.0f);
    out.heightStart = std::max(center.y - radius, 0.0f);
    out.heightRange = std::max(radius * 2.0f, 1e-4f);

    if (ref.layer >= 0) return true;

    if (ref.prototype < 0 || ref.prototype >= static_cast<int>(terrain->treePrototypes.size())) return false;
    const TerrainTreePrototype& authored = terrain->treePrototypes[ref.prototype];
    const TreePrototypeState& prototype = region.prototypes[ref.prototype];
    const FoliageTierLimits limits = foliageTierLimits(foliageTierOf(Engine::getFoliageQuality()));
    const bool uncapped = Engine::isFoliageQualityUncapped();
    const float fade = terrain->fadeLength * (uncapped ? 1.0f : limits.fadeLength);
    const float billboard = Impl::scaledDistance(terrain->billboardStart, false);
    const float treeDistance = Impl::treeDistanceOf(*terrain);

    out.colorMask = authored.windMask == TerrainTreeWindMask::VertexColor;
    if (ref.impostor) {
        // the complement of the mesh's fade, and out over the fade before Tree Distance
        out.inStart = billboard - fade;
        out.inEnd = billboard;
        out.outStart = treeDistance - fade;
        out.outEnd = treeDistance;
        out.impostorCenter = authored.impostorBoundsCenter;
        out.impostorSize = authored.impostorSize;
        out.impostorViews = static_cast<float>(authored.impostorViews);
        out.impostorBlend = uncapped || limits.impostorBlend;
    } else if (prototype.impostorReady) {
        out.outStart = billboard - fade;
        out.outEnd = billboard;
    } else {
        out.outStart = treeDistance - fade;
        out.outEnd = treeDistance;
    }
    return true;
}

void FoliageSystem::buildView(FoliageViewList& list, const FoliageViewRequest& request) {
    // F4: what the list depends on. A still camera re-culls nothing and uploads nothing (R9).
    uint64_t signature = FNV1A64_SEED;
    const auto mix = [&signature](const void* data, size_t size) { signature = fnv1a64(data, size, signature); };
    for (int p = 0; p < 6; p++) {
        const float plane[4] = {request.planes[p].normal.x, request.planes[p].normal.y, request.planes[p].normal.z, request.planes[p].d};
        mix(plane, sizeof(plane));
    }
    const float values[11] = {request.farClip, request.eye.x, request.eye.y, request.eye.z, request.lodOrigin.x, request.lodOrigin.y, request.lodOrigin.z,
                              request.lodThreshold, request.lodPerUnit, request.lodConstant, request.lodEnabled ? 1.0f : 0.0f};
    mix(values, sizeof(values));
    const uint32_t revision = residencyRevision;
    mix(&revision, sizeof(revision));
    const FoliageQuality quality = Engine::getFoliageQuality();
    mix(&quality, sizeof(quality));
    const float distanceScale = Engine::getFoliageDistanceScale();
    mix(&distanceScale, sizeof(distanceScale));
    for (const std::unique_ptr<RegionState>& region : impl->regions) {
        const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(region->owner);
        if (!terrain) continue;
        const float settings[5] = {terrain->detailDensity, terrain->treeDistance, terrain->billboardStart, terrain->fadeLength, static_cast<float>(terrain->maxMeshTrees)};
        mix(settings, sizeof(settings));
        for (const TerrainFoliageLayer& layer : terrain->foliageLayers) {
            const float layerValues[4] = {layer.densityScale, layer.densityFalloffStart, layer.farDensity, layer.minDrawDistance};
            mix(layerValues, sizeof(layerValues));
        }
    }
    if (list.built && list.signature == signature) {
        return;
    }
    list.signature = signature;
    list.built = true;

    list.stream.clear();
    list.expanded.clear();
    list.items.clear();
    list.ranges.assign(nextSlot, std::pair<uint32_t, uint32_t>(0u, 0u));
    list.detailVisible = 0;
    list.treesVisible = 0;
    list.meshTrees = 0;
    list.draws = 0;
    list.capped = false;

    const FoliageTierLimits limits = foliageTierLimits(foliageTierOf(quality));
    const bool uncapped = Engine::isFoliageQualityUncapped();

    impl->buildDetailDraws(list, request, limits, uncapped);
    impl->buildTreeDraws(list, request, limits, uncapped);

    list.needUpload = !list.stream.empty() || !list.expanded.empty();

    if (request.main) {
        for (std::unique_ptr<RegionState>& region : impl->regions) {
            region->stats.detailVisible = list.detailVisible;
            region->stats.treesVisible = list.treesVisible;
            region->stats.meshTrees = list.meshTrees;
            region->stats.draws = list.draws;
            region->stats.budgetExceeded = list.capped;
            region->stats.streamBytes = list.stream.size() * sizeof(FoliageInstance) + list.expanded.size() * sizeof(InstanceRenderData);
        }
    }
}

bool FoliageSystem::getTreeFromBody(Entity bodyEntity, size_t colliderIndex, Entity& terrainEntity, TreeId& id) const {
    const auto it = impl->bodyCells.find(bodyEntity);
    if (it == impl->bodyCells.end()) return false;
    const std::vector<uint32_t>& ids = *it->second.second;
    if (colliderIndex >= ids.size()) return false;
    terrainEntity = it->second.first;
    id = makeTreeId(0, ids[colliderIndex]);
    return true;
}

bool FoliageSystem::isTreeBody(Entity bodyEntity) const {
    return impl->bodyCells.find(bodyEntity) != impl->bodyCells.end();
}

void FoliageSystem::setTreeHitHandler(Entity terrainEntity, std::function<void(const TreeHitEvent&)> handler) {
    if (RegionState* state = impl->regionOf(terrainEntity)) {
        state->hitHandler = std::move(handler);
        updateContactSubscription();
    }
}

void FoliageSystem::setTreeRemovedHandler(Entity terrainEntity, std::function<void(TreeId, const FoliageInstanceInfo&)> handler) {
    if (RegionState* state = impl->regionOf(terrainEntity)) state->removedHandler = std::move(handler);
}

// R33: with no subscriber it adds no work
void FoliageSystem::updateContactSubscription() {
    bool wanted = !onTreeHit.empty();
    for (const std::unique_ptr<RegionState>& region : impl->regions) wanted = wanted || static_cast<bool>(region->hitHandler);
    if (!wanted) {
        impl->contactSubscription = Subscription();
        contactsConnected = false;
        return;
    }
    if (contactsConnected) return;
    contactsConnected = true;
    FoliageSystem* self = this;
    impl->contactSubscription = foliagephysics::subscribeContacts(*scene, [self](Entity treeBody, size_t collider, Entity other, const Vector3& point, const Vector3& normal, float speed) {
        Entity terrainEntity = NULL_ENTITY;
        TreeId id = 0;
        if (!self->getTreeFromBody(treeBody, collider, terrainEntity, id)) return;
        FoliageInstanceInfo pose;
        TreeHitEvent event;
        event.id = id;
        event.prototype = self->getTreePose(terrainEntity, id, pose) ? pose.prototype : -1;
        event.point = point;
        event.normal = normal;
        event.body = other;
        event.speed = speed;
        self->onTreeHit(terrainEntity, event);
        if (RegionState* state = self->impl->regionOf(terrainEntity)) {
            if (state->hitHandler) state->hitHandler(event);
        }
    });
}

bool FoliageSystem::isWindBlowing(Entity terrainEntity) const {
    const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(terrainEntity);
    return terrain && terrain->windBending > 0.0f && terrain->windSpeed > 0.0f;
}

void FoliageSystem::setLastUploadBytes(uint64_t bytes) {
    for (std::unique_ptr<RegionState>& region : impl->regions) region->stats.uploadBytes = bytes;
}

void FoliageSystem::uploadTreeBuffers() {
    int budget = foliageBufferCap();
    for (std::unique_ptr<RegionState>& regionPtr : impl->regions) {
        RegionState& region = *regionPtr;
        uint64_t bufferBytes = 0;
        for (int64_t key : region.residentOrder) {
            const auto it = region.cells.find(key);
            if (it == region.cells.end()) continue;
            TreeCell& cell = it->second;
            if (cell.gpu) bufferBytes += cell.gpuCapacity;
            if (!cell.wantsGpu || (!cell.gpuDirty && cell.gpu) || cell.instances.empty()) continue;
            if (budget <= 0) {
                region.stats.pendingCells++;
                continue;
            }
            budget--;
            const size_t bytes = cell.instances.size() * sizeof(FoliageInstance);
            if (!cell.gpu) cell.gpu = std::make_shared<ExternalBuffer>();
            BufferRender* render = cell.gpu->getRender();
            if (render->isCreated() && cell.gpuCapacity < bytes) render->destroyBuffer();
            if (!render->isCreated()) {
                cell.gpuCapacity = bytes + bytes / 4;
                render->createBuffer(static_cast<unsigned int>(cell.gpuCapacity), nullptr, BufferType::VERTEX_BUFFER, BufferUsage::DYNAMIC);
            }
            render->updateBuffer(static_cast<unsigned int>(bytes), cell.instances.data());
            cell.gpuDirty = false;
            residencyRevision++;
        }
        region.stats.bufferBytes = bufferBytes;
        region.stats.gpuBytes = bufferBytes + region.stats.streamBytes;
    }
}
