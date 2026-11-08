// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SplineSystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "object/TerrainEdit.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "manager/PrefabManager.h"
#include "object/spline/Spline.h"
#include "profiler/Profile.h"
#include "spline/SplineCenterline.h"
#include "spline/SplineCorner.h"
#include "spline/SplineEndTaper.h"
#include "spline/SplineEvaluator.h"
#include "spline/SplineFenceLayout.h"
#include "spline/SplineFillField.h"
#include "spline/SplineFillMesher.h"
#include "spline/SplinePolygonFill.h"
#include "spline/SplineProfile.h"
#include "spline/SplineStations.h"
#include "spline/SplineStep.h"
#include "spline/SplineStripBuilder.h"
#include "spline/SplineTileLayout.h"
#include "spline/SplineUtility.h"
#include "subsystem/MeshSystem.h"
#if JASPERENGINE_TERRAIN_WORLD
#include "subsystem/TerrainWorldRuntime.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "subsystem/PhysicsSystem.h"
#endif
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Rigidbody2D.h"
#ifndef JASPERENGINE_PHYSICS_3D
#include "subsystem/PhysicsSystem.h"
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

using namespace jasperengine;

namespace{

    // Measured cost of the generators (Release, the benchmark server; docs/perf/splines.md):
    // a strip ring point (one vertex of one station) and a placed piece. The estimate decides
    // inline against INLINE_BUDGET_MS (R30).
    constexpr double SPLINE_NS_PER_RING_POINT = 45.0;
    constexpr double SPLINE_NS_PER_FILL_CELL = 60.0;
    constexpr double SPLINE_NS_PER_PIECE = 700.0;

    double nowMs(){
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // the work the splines did (getWorkMs): SplineSystem::update and the chunk builds and layouts
    // run as jobs; a build inside the update is the update's
    std::atomic<uint64_t> workNanoseconds{0};
    thread_local bool insideUpdate = false;

    struct WorkTimer{
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        bool counts = !insideUpdate;
        ~WorkTimer(){
            if (counts){
                workNanoseconds.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
            }
        }
    };

    // FNV-1a over values: change detection of settings and chunk inputs (not saved anywhere)
    struct Hasher{
        uint64_t value = 1469598103934665603ull;

        void bytes(const void* data, size_t size){
            const unsigned char* p = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; i++){
                value ^= p[i];
                value *= 1099511628211ull;
            }
        }
        void f(float v){ bytes(&v, sizeof(v)); }
        void u(uint64_t v){ bytes(&v, sizeof(v)); }
        void v3(const Vector3& v){ f(v.x); f(v.y); f(v.z); }
        void s(const std::string& text){ u(text.size()); bytes(text.data(), text.size()); }
        template<typename E>
        void e(E v){ u(static_cast<uint64_t>(v)); }
    };

    void hashStation(Hasher& h, const spline::SplineStation& s){
        h.v3(s.position); h.v3(s.right); h.v3(s.up); h.v3(s.forward);
        h.f(s.halfWidthLeft); h.f(s.halfWidthRight); h.f(s.rollRadians); h.f(s.distance);
        h.e(s.join); h.v3(s.mitreRight); h.u(s.mitreSides); h.f(s.groundOffset); h.f(s.topOffset);
        h.f(s.faceTurnLeft); h.f(s.faceTurnRight); h.f(s.topArcAllowance);
    }

    uint64_t hashExtrude(const SplineExtrudeComponent& c){
        Hasher h;
        h.u(c.enabled); h.e(c.shape); h.e(c.widthSource); h.f(c.width);
        h.f(c.edgeDrop); h.f(c.edgeInset); h.f(c.crownRise); h.f(c.shoulderWidth); h.f(c.shoulderDrop);
        h.f(c.thickness); h.f(c.height); h.e(c.grade); h.f(c.stepHeight); h.u(c.tubeSides);
        h.u(c.customProfile.size());
        for (const SplineProfilePoint& p : c.customProfile){
            h.f(p.position.x); h.f(p.position.y); h.u(p.hard); h.u(p.grounded);
        }
        h.u(c.customClosed);
        h.f(c.maxHalfWidth); h.f(c.waterEdgeDrop); h.u(c.seaLevelFloor); h.f(c.seaLevel); h.u(c.flow); h.f(c.flowSpeed);
        h.f(c.tilesPerMetreU); h.f(c.tilesPerMetreV); h.u(c.levelCourseV);
        h.e(c.corners); h.u(c.endCaps); h.f(c.endTaper); h.f(c.lateralOffset); h.f(c.verticalOffset);
        h.e(c.conform); h.f(c.slopeBlend); h.f(c.stationSpacing); h.e(c.collision);
        return h.value;
    }

    void hashPool(Hasher& h, const std::vector<SplinePoolEntry>& pool){
        h.u(pool.size());
        for (const SplinePoolEntry& entry : pool){
            h.s(entry.asset.path);
            h.f(entry.weight);
        }
    }

    uint64_t hashInstantiate(const SplineInstantiateComponent& c){
        Hasher h;
        h.u(c.enabled); h.e(c.mode);
        hashPool(h, c.items); hashPool(h, c.posts); hashPool(h, c.spans); hashPool(h, c.gates);
        hashPool(h, c.crests); hashPool(h, c.caps);
        h.f(c.spacing); h.e(c.fit); h.f(c.postPitch); h.f(c.spanMaxStretch); h.e(c.fenceGrade); h.f(c.crestPitch);
        h.u(c.spanOverrides.size());
        for (const SplineSpanOverride& o : c.spanOverrides){
            h.u(o.knot); h.u(o.span); h.e(o.kind); h.u(o.piece);
        }
        h.f(c.lateralOffset); h.f(c.verticalOffset); h.f(c.spacingJitter); h.f(c.yawJitter); h.f(c.lateralJitter);
        h.f(c.scaleRange.x); h.f(c.scaleRange.y); h.f(c.dropout); h.u(c.seed); h.f(c.endTrim);
        h.e(c.plantMode); h.f(c.maxTilt); h.e(c.conform); h.f(c.slopeBlend); h.f(c.stationSpacing);
        return h.value;
    }

    bool sameMatrix(const Matrix4& a, const Matrix4& b){
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++){
                if (a.get(c, r) != b.get(c, r)) return false;
            }
        }
        return true;
    }

    float largestScale(const Transform& transform){
        const Vector3& s = transform.worldScale;
        const float scale = std::max(std::fabs(s.x), std::max(std::fabs(s.y), std::fabs(s.z)));
        return scale > 0.0f ? scale : 1.0f;
    }

    bool isUniformScale(const Transform& transform){
        const Vector3& s = transform.worldScale;
        const float m = largestScale(transform);
        const float tolerance = m * 1.0e-4f;
        return std::fabs(std::fabs(s.x) - m) <= tolerance && std::fabs(std::fabs(s.y) - m) <= tolerance &&
               std::fabs(std::fabs(s.z) - m) <= tolerance;
    }

    bool is2D(const SplineComponent& spline){
        return spline.spline.upAxis().z != 0.0f;
    }

    spline::SplineProfileShape profileShape(SplineExtrudeShape shape){
        switch (shape){
            case SplineExtrudeShape::Flat: return spline::SplineProfileShape::Flat;
            case SplineExtrudeShape::Bevel: return spline::SplineProfileShape::Bevel;
            case SplineExtrudeShape::Crown: return spline::SplineProfileShape::Crown;
            case SplineExtrudeShape::Rectangle: return spline::SplineProfileShape::Rectangle;
            case SplineExtrudeShape::Tube: return spline::SplineProfileShape::Tube;
            case SplineExtrudeShape::Custom: return spline::SplineProfileShape::Custom;
            default: return spline::SplineProfileShape::Flat;
        }
    }

    spline::SplineProfile buildExtrudeProfile(const SplineExtrudeComponent& extrude){
        if (extrude.shape == SplineExtrudeShape::Custom){
            std::vector<spline::SplineProfilePoint> points;
            points.reserve(std::min<size_t>(extrude.customProfile.size(), spline::SPLINE_PROFILE_MAX_CUSTOM_POINTS));
            for (const SplineProfilePoint& p : extrude.customProfile){
                if (points.size() >= spline::SPLINE_PROFILE_MAX_CUSTOM_POINTS) break;
                spline::SplineProfilePoint point;
                point.position = p.position;
                point.hard = p.hard ? 1 : 0;
                point.grounded = p.grounded ? 1 : 0;
                points.push_back(point);
            }
            return spline::buildCustomProfile(points, extrude.customClosed);
        }
        spline::SplineProfileParams params;
        params.shape = profileShape(extrude.shape);
        // the preset is authored at the nominal width: the stations' half-widths scale it
        params.width = extrude.width;
        params.edgeDrop = extrude.edgeDrop;
        params.edgeInset = extrude.edgeInset;
        params.crownRise = extrude.crownRise;
        params.shoulderWidth = extrude.shoulderWidth;
        params.shoulderDrop = extrude.shoulderDrop;
        params.thickness = extrude.thickness;
        params.height = extrude.height;
        params.tubeSides = extrude.tubeSides;
        return spline::buildProfile(params);
    }

    spline::SplineDrape stationConform(SplineConform conform){
        switch (conform){
            case SplineConform::TerrainHeight: return spline::SplineDrape::Height;
            case SplineConform::TerrainHeightAndSlope: return spline::SplineDrape::HeightAndSlope;
            default: return spline::SplineDrape::None;
        }
    }

    spline::SplineCornerStyle cornerStyle(SplineCornerStyle style){
        switch (style){
            case SplineCornerStyle::Mitre: return spline::SplineCornerStyle::Mitre;
            case SplineCornerStyle::Round: return spline::SplineCornerStyle::Round;
            default: return spline::SplineCornerStyle::None;
        }
    }

    void addWarning(std::vector<SplineWarning>& warnings, SplineWarningKind kind, int knot, std::string message){
        for (const SplineWarning& w : warnings){
            if (w.kind == kind && w.knot == knot) return;
        }
        SplineWarning warning;
        warning.kind = kind;
        warning.knot = knot;
        warning.message = std::move(message);
        warnings.push_back(std::move(warning));
    }

    std::string knotText(uint32_t knot){
        return "knot " + std::to_string(knot);
    }

    // the vertices of one generated chunk, chunk-local (the chunk GameObject stands at origin)
    struct ChunkMesh{
        std::vector<spline::SplineVertex> vertices;
        std::vector<uint32_t> indices;
        AABB bounds;
        Vector3 origin;
    };

    void finishChunkMesh(ChunkMesh& mesh, const AABB& bounds){
        mesh.bounds = bounds;
        mesh.origin = bounds.isNull() ? Vector3(0.0f, 0.0f, 0.0f) : bounds.getCenter();
        for (spline::SplineVertex& v : mesh.vertices){
            v.position -= mesh.origin;
        }
    }

    void writeMesh(MeshComponent& mesh, const ChunkMesh& chunk){
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.numSubmeshes = 1;

        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        mesh.buffer.addAttribute(AttributeType::TANGENT, 4);
        mesh.buffer.addAttribute(AttributeType::COLOR, 4);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* uv = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        Attribute* tangent = mesh.buffer.getAttribute(AttributeType::TANGENT);
        Attribute* color = mesh.buffer.getAttribute(AttributeType::COLOR);
        for (const spline::SplineVertex& v : chunk.vertices){
            mesh.buffer.addVector3(position, v.position);
            mesh.buffer.addVector2(uv, v.uv);
            mesh.buffer.addVector3(normal, v.normal);
            mesh.buffer.addVector4(tangent, v.tangent);
            mesh.buffer.addVector4(color, v.color);
        }

        std::vector<uint16_t> indices(chunk.indices.size());
        for (size_t i = 0; i < chunk.indices.size(); i++){
            indices[i] = static_cast<uint16_t>(chunk.indices[i]);
        }
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), static_cast<unsigned int>(indices.size()),
                               reinterpret_cast<char*>(indices.data()), sizeof(uint16_t));

        AABB local;
        for (const spline::SplineVertex& v : chunk.vertices){
            local.merge(v.position);
        }
        mesh.verticesAABB = local;
        mesh.aabb = local;
        mesh.vertexCount = static_cast<unsigned int>(chunk.vertices.size());
        mesh.needUpdateAABB = true;
        if (mesh.loaded){
            mesh.needReload = true;
        }
    }

    // the owner's Mesh as the chunks' template: material, shadows and lighting
    void copyMeshLook(const MeshComponent& owner, MeshComponent& chunk){
        chunk.submeshes[0].material = owner.submeshes[0].material;
        chunk.submeshes[0].faceCulling = owner.submeshes[0].faceCulling;
        chunk.castShadows = owner.castShadows;
        chunk.receiveShadows = owner.receiveShadows;
        chunk.receiveLights = owner.receiveLights;
        chunk.receiveIBL = owner.receiveIBL;
        chunk.staticShadowCaster = owner.staticShadowCaster;
        chunk.renderInReflectionProbes = owner.renderInReflectionProbes;
        chunk.transparent = owner.transparent;
        chunk.autoTransparency = owner.autoTransparency;
        chunk.cullingMode = owner.cullingMode;
    }

    float easeValue(SplineEasing easing, float t){
        switch (easing){
            case SplineEasing::EaseIn: return t * t;
            case SplineEasing::EaseOut: return 1.0f - (1.0f - t) * (1.0f - t);
            case SplineEasing::EaseInOut: return t * t * (3.0f - 2.0f * t);
            default: return t;
        }
    }

    bool isPrefabPath(const std::string& path){
        static const char extension[] = ".prefab";
        const size_t n = sizeof(extension) - 1;
        return path.size() >= n && path.compare(path.size() - n, n, extension) == 0;
    }

}

// ---- the resolve map's types

struct SplineSystem::ExtrudeChunk{
    Entity entity = NULL_ENTITY;
    uint64_t hash = 0;
    AABB worldBounds;
    uint32_t vertices = 0;
    uint32_t triangles = 0;
};

struct SplineSystem::PoolModel{
    std::string path;
    float weight = 1.0f;
    bool prefab = false;
    // the GameObject the model is loaded into to measure it (hidden; the chunks load their own)
    Entity probe = NULL_ENTITY;
    bool loaded = false;
    bool failed = false;
    AABB bounds;
};

struct SplineSystem::PieceChunk{
    Entity entity = NULL_ENTITY;
    std::string path;
    uint32_t count = 0;
};

// one scheduled rebuild: immutable inputs, the chunks it rebuilds, and their results
struct SplineSystem::BuildJob{
    enum class Kind{ Strip, Polygon, Water, Pieces };
    Kind kind = Kind::Strip;
    uint64_t generation = 0;
    double startMs = 0.0;

    // Strip
    spline::SplineProfile profile;
    std::vector<spline::SplineStation> stations;
    spline::SplineStripParams params;
    std::vector<spline::SplineStripChunkRange> ranges;
    // Water
    std::vector<float> groundHeights;
    spline::SplineGroundGrid ground;
    spline::SplineFillResult field;
    spline::SplineFillMeshParams fillParams;
    std::vector<spline::SplineFillChunkRange> fillRanges;

    // per chunk of the new plan: its input hash, and whether this job builds it
    std::vector<uint64_t> hashes;
    std::vector<uint8_t> rebuild;
    std::vector<ChunkMesh> meshes;
    std::vector<jobs::Task<void>> tasks;

    // Pieces: the layout's inputs (copies, so it may run on a worker), then the poses by path
    SplineInstantiateComponent instantiate;
    bool closedSpline = false;
    std::vector<spline::SplineCenterSample> center;
    std::vector<std::string> paths;
    std::vector<spline::SplinePiece> items, posts, spans, gates, crests, caps;
    std::vector<float> knotStations;
    std::vector<spline::SplineSpanOverride> overrides;
    std::vector<std::string> layoutMessages;
    bool capped = false;
    std::vector<std::pair<std::string, std::vector<spline::SplinePiecePose>>> poses;
    uint32_t instances = 0;

    void layoutPieces();

    std::vector<SplineWarning> warnings;
    SplineBuildStats stats;

    void buildChunk(size_t i){
        JE_PROFILE_SCOPE("SplineSystem::buildChunk");
        const WorkTimer timer;
        ChunkMesh& mesh = meshes[i];
        if (kind == Kind::Strip){
            spline::SplineStripMesh strip;
            spline::buildSplineStripChunk(profile, stations, ranges[i], params, strip);
            mesh.vertices = std::move(strip.vertices);
            mesh.indices = std::move(strip.indices);
            finishChunkMesh(mesh, strip.bounds);
        }else if (kind == Kind::Water){
            spline::SplineFillMesh fill = spline::buildSplineFillMesh(field, ground, stations, fillRanges[i], fillParams);
            mesh.vertices = std::move(fill.vertices);
            mesh.indices = std::move(fill.indices);
            finishChunkMesh(mesh, fill.bounds);
        }
    }

    // The flood over the ground lattice, its warnings and its chunks (each to build, hashed by build)
    void floodFill(const spline::SplineFillParams& fill){
        field = spline::buildSplineFillField(stations, ground, fill);
        const spline::SplineFillDiagnostics& d = field.diagnostics;
        if (d.waterlineRiseMetres > spline::SPLINE_WATERLINE_RISE_TOLERANCE_METRES){
            const int knot = d.waterlineRiseStation != spline::SPLINE_FILL_NO_STATION &&
                             stations[d.waterlineRiseStation].splineT >= 0.0f
                             ? static_cast<int>(stations[d.waterlineRiseStation].splineT) : -1;
            addWarning(warnings, SplineWarningKind::WaterlineRise, knot,
                       "The waterline rises " + std::to_string(d.waterlineRiseMetres) + " units to close the banks.");
        }
        if (d.exceededBudget){
            addWarning(warnings, SplineWarningKind::InstanceCap, -1, "The flooded region is larger than the fill's budget.");
        }
        fillRanges = spline::carveFillChunks(field);
        const size_t count = fillRanges.size();
        hashes.resize(count);
        rebuild.assign(count, 1);
        meshes.resize(count);
        for (size_t i = 0; i < count; i++){
            // the fill's chunks depend on the whole field: a chunk's hash is the field's plus its block
            Hasher h;
            h.u(generation);
            h.u(i);
            hashes[i] = h.value;
        }
        stats.stations = static_cast<uint32_t>(stations.size());
    }

#if JASPERENGINE_TERRAIN_WORLD
    // T4 R26: a Terrain World's ground, the world's own sample lattice over the footprint, read here on
    // the job, which may wait for a tile that is not in memory (R24), then the flood and its chunks
    std::shared_ptr<TerrainWorldHeightReader> worldGround;
    int worldX0 = 0, worldZ0 = 0;
    float worldBaseY = 0.0f;
    spline::SplineFillParams worldFill;

    void buildOnWorld(){
        JE_PROFILE_SCOPE("SplineSystem::waterOnTerrainWorld");
        worldGround->read(worldX0, worldZ0, static_cast<int>(ground.countX), static_cast<int>(ground.countZ), groundHeights);
        for (float& height : groundHeights){
            height += worldBaseY;
        }
        ground.heights = groundHeights;
        floodFill(worldFill);
        for (size_t i = 0; i < fillRanges.size(); i++){
            buildChunk(i);
        }
    }
#endif

    bool ready() const{
        for (const jobs::Task<void>& task : tasks){
            if (!task.isReady()) return false;
        }
        return true;
    }
};

struct SplineSystem::Resolve{
    // the sum of the scene's terrains' surface revisions, at the last look
    uint64_t surfaceRevisions = 0;
    uint64_t splineVersion = 0;
    uint64_t holeStamp = 0;
    uint64_t extrudeHash = 0;
    uint64_t instantiateHash = 0;
    Matrix4 world;
    bool hasWorld = false;
    bool hadExtrude = false;
    bool hadInstantiate = false;

    bool extrudeDirty = false;
    bool instantiateDirty = false;
    double lastChangeMs = 0.0;
    // build ids: a result whose id is not the latest is stale (R30)
    uint64_t extrudeGeneration = 0;
    uint64_t piecesGeneration = 0;

    std::vector<ExtrudeChunk> extrudeChunks;
    std::shared_ptr<BuildJob> extrudeJob;
    std::vector<SplineWarning> extrudeWarnings;
    std::vector<SplineWarning> stationWarnings;
    SplineBuildStats extrudeStats;
    Material material;
    bool materialSynced = false;

    std::vector<PoolModel> pools;
    std::vector<PieceChunk> pieceChunks;
    std::vector<Entity> prefabRoots;
    std::shared_ptr<BuildJob> piecesJob;
    std::vector<SplineWarning> instantiateWarnings;
    SplineBuildStats instantiateStats;
    std::vector<SplinePiecePlacement> placements;

    // 3D mesh colliders live on the chunks; the 2D chain on bodies of its own
    std::vector<Entity> chainBodies;
    SplineCollision collision = SplineCollision::None;

    // the river or lake surface (sampleWaterSurface): world stations of the water
    std::vector<spline::SplineStation> waterStations;
    spline::SplineStationGridXZ waterGrid;
    AABB waterBounds;
    float waterHalfWidth = 0.0f;
    float waterSpeed = 0.0f;
};

// ---- lifetime

SplineSystem::SplineSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<SplineComponent>());
    declareHookType(scene->getComponentId<SplineComponent>());
    declareHookType(scene->getComponentId<SplineExtrudeComponent>());
    declareHookType(scene->getComponentId<SplineInstantiateComponent>());
}

SplineSystem::~SplineSystem() = default;

void SplineSystem::load(){
    update(0);
}

void SplineSystem::draw(){
}

void SplineSystem::destroy(){
    std::vector<Entity> owners;
    owners.reserve(resolves.size());
    for (const auto& item : resolves){
        owners.push_back(item.first);
    }
    for (Entity owner : owners){
        release(owner);
    }
}

void SplineSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (generatedOwners.count(entity)){
        // a chunk destroyed by someone else (the scene clearing): forget it
        generatedOwners.erase(entity);
        return;
    }
    if (componentId == scene->getComponentId<SplineExtrudeComponent>()){
        // the Mesh left behind may load again (RenderSystem skipped it while it was a template)
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            mesh->loadCalled = false;
        }
    }
    auto it = resolves.find(entity);
    if (it == resolves.end()){
        return;
    }
    if (componentId == scene->getComponentId<SplineComponent>()){
        release(entity);
    }else if (componentId == scene->getComponentId<SplineExtrudeComponent>()){
        Resolve& resolve = *it->second;
        resolve.extrudeJob.reset();
        for (ExtrudeChunk& chunk : resolve.extrudeChunks){
            destroyGenerated(chunk.entity);
        }
        resolve.extrudeChunks.clear();
        for (Entity body : resolve.chainBodies){
            destroyGenerated(body);
        }
        resolve.chainBodies.clear();
        resolve.extrudeWarnings.clear();
        resolve.extrudeStats = SplineBuildStats();
        resolve.waterStations.clear();
        resolve.hadExtrude = false;
    }else if (componentId == scene->getComponentId<SplineInstantiateComponent>()){
        Resolve& resolve = *it->second;
        resolve.piecesJob.reset();
        for (PieceChunk& chunk : resolve.pieceChunks){
            destroyGenerated(chunk.entity);
        }
        resolve.pieceChunks.clear();
        for (PoolModel& pool : resolve.pools){
            destroyGenerated(pool.probe);
        }
        resolve.pools.clear();
        for (Entity root : resolve.prefabRoots){
            destroyPrefabPiece(root);
        }
        resolve.prefabRoots.clear();
        resolve.instantiateWarnings.clear();
        resolve.instantiateStats = SplineBuildStats();
        resolve.hadInstantiate = false;
    }
}

SplineSystem::Resolve& SplineSystem::resolveOf(Entity entity){
    std::unique_ptr<Resolve>& resolve = resolves[entity];
    if (!resolve){
        resolve = std::make_unique<Resolve>();
    }
    return *resolve;
}

void SplineSystem::release(Entity entity){
    // ownership leaves the map before the chunks are destroyed (their removal hooks run again)
    auto node = resolves.extract(entity);
    if (node.empty()){
        return;
    }
    releaseResolve(*node.mapped());
}

void SplineSystem::releaseResolve(Resolve& resolve){
    resolve.extrudeJob.reset();
    resolve.piecesJob.reset();
    for (ExtrudeChunk& chunk : resolve.extrudeChunks){
        destroyGenerated(chunk.entity);
    }
    for (PieceChunk& chunk : resolve.pieceChunks){
        destroyGenerated(chunk.entity);
    }
    for (PoolModel& pool : resolve.pools){
        destroyGenerated(pool.probe);
    }
    for (Entity root : resolve.prefabRoots){
        destroyPrefabPiece(root);
    }
    for (Entity body : resolve.chainBodies){
        destroyGenerated(body);
    }
    resolve.extrudeChunks.clear();
    resolve.pieceChunks.clear();
    resolve.pools.clear();
    resolve.prefabRoots.clear();
    resolve.chainBodies.clear();
}

Entity SplineSystem::createChunkEntity(Entity owner){
    const Entity entity = scene->createEntity();
    scene->addComponent<Transform>(entity);
    scene->addComponent<MeshComponent>(entity);
    generatedOwners[entity] = owner;
    return entity;
}

void SplineSystem::destroyGenerated(Entity entity){
    if (entity == NULL_ENTITY){
        return;
    }
    generatedOwners.erase(entity);
    if (scene->isEntityCreated(entity)){
        scene->destroyEntity(entity);
    }
}

void SplineSystem::destroyPrefabPiece(Entity root){
    if (root == NULL_ENTITY){
        return;
    }
    generatedOwners.erase(root);
    if (scene->isEntityCreated(root)){
        // the prefab's children and scripts go with it
        scene->destroyHierarchy(root);
    }
}

// ---- the frame

void SplineSystem::update(double dt){
    auto splines = scene->getComponentArray<SplineComponent>();
    if (splines->size() == 0 && resolves.empty() && scene->getComponentArray<SplineAnimateComponent>()->size() == 0){
        return;
    }
    JE_PROFILE_SCOPE("SplineSystem::update");
    const WorkTimer timer;
    insideUpdate = true;
    clockMs = nowMs();

    // owners whose Spline is gone (or GameObject destroyed) release their chunks
    if (!resolves.empty()){
        std::vector<Entity> gone;
        for (const auto& item : resolves){
            if (!scene->isEntityCreated(item.first) || !scene->findComponent<SplineComponent>(item.first)){
                gone.push_back(item.first);
            }
        }
        for (Entity entity : gone){
            release(entity);
        }
    }

    for (size_t i = 0; i < splines->size(); i++){
        const Entity entity = splines->getEntity(i);
        SplineComponent& spline = splines->getComponentFromIndex(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform){
            continue;
        }
        updateConsumers(entity, spline, *transform);
        // the component array may have moved while chunks were created
        splines = scene->getComponentArray<SplineComponent>();
    }

    updateAnimate(dt);
    insideUpdate = false;
}

double SplineSystem::getWorkMs(){
    return static_cast<double>(workNanoseconds.load(std::memory_order_relaxed)) * 1.0e-6;
}

void SplineSystem::updateConsumers(Entity entity, SplineComponent& spline, Transform& transform){
    SplineExtrudeComponent* extrude = scene->findComponent<SplineExtrudeComponent>(entity);
    SplineInstantiateComponent* instantiate = scene->findComponent<SplineInstantiateComponent>(entity);
    const bool active = spline.enabled && scene->isEntityActiveInHierarchy(entity);
    const bool wantsExtrude = active && extrude && extrude->enabled;
    const bool wantsInstantiate = active && instantiate && instantiate->enabled;

    auto existing = resolves.find(entity);
    if (!wantsExtrude && !wantsInstantiate){
        if (existing != resolves.end()){
            release(entity);
        }
        return;
    }

    Resolve& resolve = resolveOf(entity);

    // what changed since the last build
    bool common = false;
    if (resolve.splineVersion != spline.spline.version()){
        resolve.splineVersion = spline.spline.version();
        common = true;
    }
    if (!resolve.hasWorld || !sameMatrix(resolve.world, transform.modelMatrix)){
        resolve.world = transform.modelMatrix;
        resolve.hasWorld = true;
        common = true;
    }
    // a terrain under a conformed consumer was built again (MeshSystem clears the flag after us)
    // or edited
    bool terrainChanged = false;
    if ((wantsExtrude && (extrude->conform != SplineConform::None || extrude->shape == SplineExtrudeShape::WaterBanks)) ||
        (wantsInstantiate && instantiate->conform != SplineConform::None)){
        auto terrains = scene->getComponentArray<TerrainComponent>();
        uint64_t surfaceRevisions = 0;
        for (size_t t = 0; t < terrains->size(); t++){
            const TerrainComponent& terrain = terrains->getComponentFromIndex(t);
            terrainChanged = terrainChanged || terrain.needUpdateTerrain;
            surfaceRevisions += terrain.surfaceRevision;
        }
        // T1 R23: an edit keeps the node tree, and moves the surface all the same
        terrainChanged = terrainChanged || surfaceRevisions != resolve.surfaceRevisions;
        resolve.surfaceRevisions = surfaceRevisions;
    }

    // T3 R26: a hole cut or filled: a road over it warns, an item over it goes. Only a terrain's holes count
    if (wantsExtrude || wantsInstantiate){
        auto terrains = scene->getComponentArray<TerrainComponent>();
        uint64_t holeStamp = 0;
        for (size_t t = 0; t < terrains->size(); t++){
            holeStamp += terrains->getComponentFromIndex(t).holeMapRevision;
        }
        if (holeStamp != resolve.holeStamp){
            resolve.holeStamp = holeStamp;
            common = true;
        }
    }

    if (wantsExtrude){
        const uint64_t h = hashExtrude(*extrude);
        if (!resolve.hadExtrude || h != resolve.extrudeHash || common ||
            (terrainChanged && (extrude->conform != SplineConform::None || extrude->shape == SplineExtrudeShape::WaterBanks))){
            resolve.extrudeHash = h;
            resolve.extrudeDirty = true;
            resolve.lastChangeMs = clockMs;
        }
        resolve.hadExtrude = true;
    }else if (resolve.hadExtrude){
        onComponentRemoved(entity, scene->getComponentId<SplineExtrudeComponent>());
        resolve.hadExtrude = false;
    }

    if (wantsInstantiate){
        const uint64_t h = hashInstantiate(*instantiate);
        if (!resolve.hadInstantiate || h != resolve.instantiateHash || common ||
            (terrainChanged && instantiate->conform != SplineConform::None)){
            resolve.instantiateHash = h;
            resolve.instantiateDirty = true;
            resolve.lastChangeMs = clockMs;
        }
        resolve.hadInstantiate = true;
    }else if (resolve.hadInstantiate){
        onComponentRemoved(entity, scene->getComponentId<SplineInstantiateComponent>());
        resolve.hadInstantiate = false;
    }

    // finished background work swaps in on the main thread
    if (resolve.extrudeJob && resolve.extrudeJob->ready()){
        finishExtrude(entity, resolve);
    }
    if (resolve.piecesJob && resolve.piecesJob->ready()){
        std::shared_ptr<BuildJob> job = std::move(resolve.piecesJob);
        if (job->generation == resolve.piecesGeneration){
            applyPieces(entity, resolve, *job);
        }
    }

    if (liveBuilds){
        // while the editor drags, a large rebuild waits for a pause in the changes
        const bool debounce = interactiveEdit && clockMs - resolve.lastChangeMs < DRAG_DEBOUNCE_MS;
        if (resolve.extrudeDirty && wantsExtrude){
            const SplineBuildStats& last = resolve.extrudeStats;
            const double estimate = last.stations * std::max<uint32_t>(1u, last.vertices / std::max<uint32_t>(1u, last.stations)) *
                                    SPLINE_NS_PER_RING_POINT * 1.0e-6;
            if (!debounce || estimate < INLINE_BUDGET_MS){
                resolve.extrudeDirty = false;
                scheduleExtrude(entity, resolve, spline, transform, *scene->findComponent<SplineExtrudeComponent>(entity));
            }
        }
        if (resolve.instantiateDirty && wantsInstantiate){
            const double estimate = resolve.instantiateStats.instances * SPLINE_NS_PER_PIECE * 1.0e-6;
            if (!debounce || estimate < INLINE_BUDGET_MS){
                SplineInstantiateComponent& current = *scene->findComponent<SplineInstantiateComponent>(entity);
                if (preparePools(entity, resolve, current)){
                    resolve.instantiateDirty = false;
                    scheduleInstantiate(entity, resolve, spline, transform, current);
                }
            }
        }
    }

    if (wantsExtrude){
        // the Mesh is the chunks' template: one submesh holds their material (the Inspector's)
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity); mesh && mesh->numSubmeshes == 0){
            mesh->numSubmeshes = 1;
        }
        syncChunkMaterials(entity, resolve);
    }
}

// ---- stations

TerrainComponent* SplineSystem::findTerrainUnder(const Vector3& worldPoint, Entity& terrainEntity) const{
    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (size_t i = 0; i < terrains->size(); i++){
        const Entity entity = terrains->getEntity(i);
        TerrainComponent& terrain = terrains->getComponentFromIndex(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !scene->isEntityActiveInHierarchy(entity)){
            continue;
        }
        const Vector3 local = transform->modelMatrix.inverse() * worldPoint;
        const float half = terrain.terrainSize * 0.5f;
        // T4: a World is under the point where one of its tiles is
        const bool under = terrain.world ? querySurface(terrain, local.x, local.z).present
                                         : (local.x >= -half && local.x <= half && local.z >= -half && local.z <= half);
        if (under){
            terrainEntity = entity;
            return &terrain;
        }
    }
    terrainEntity = NULL_ENTITY;
    return nullptr;
}

bool SplineSystem::collectStations(Entity entity, const SplineComponent& spline, const Transform& transform,
                                   spline::SplineStationOptions& options, std::vector<spline::SplineStation>& stations,
                                   std::vector<SplineWarning>& warnings){
    const Matrix4 world = transform.modelMatrix;
    options.localToWorld = &world;
    if (!isUniformScale(transform)){
        addWarning(warnings, SplineWarningKind::NonUniformScale, -1,
                   "The GameObject's scale is not uniform: the radius uses its largest axis.");
    }

    // terrain probe: the named terrain, else the first one under each point
    const Entity named = spline.terrain.terrain;
    // a probe outlives this call in what it was handed to, so the count is shared
    auto stationsOverHole = std::make_shared<int>(0);
    if (options.conform != spline::SplineDrape::None){
        Scene* s = scene;
        const SplineSystem* self = this;
        options.probe = [s, self, named, stationsOverHole](const Vector3& point) -> spline::SplineSurfaceSample{
            spline::SplineSurfaceSample sample;
            Entity terrainEntity = named;
            TerrainComponent* terrain = nullptr;
            if (named != NULL_ENTITY){
                terrain = s->findComponent<TerrainComponent>(named);
            }else{
                terrain = self->findTerrainUnder(point, terrainEntity);
            }
            const Transform* terrainTransform = terrain ? s->findComponent<Transform>(terrainEntity) : nullptr;
            if (!terrain || !terrainTransform){
                return sample;
            }
            const Vector3 local = terrainTransform->modelMatrix.inverse() * point;
            const float half = terrain->terrainSize * 0.5f;
            // T4 R26: a World's ground is its tiles' (a job waits for a cold one), absent off them
            if (!terrain->world && (local.x < -half || local.x > half || local.z < -half || local.z > half || !terrain->heightMap.hasData())){
                return sample;
            }
            // T3 R26: the height map stays defined under a hole, so a road bridges it
            const terrain::SurfaceSample ground = querySurface(*terrain, local.x, local.z);
            if (terrain->world && !ground.present){
                return sample;
            }
            if (ground.hole){
                (*stationsOverHole)++;
            }
            const Vector3 surface = terrainTransform->modelMatrix * Vector3(local.x, ground.height, local.z);
            const Vector3 origin = terrainTransform->modelMatrix * Vector3(0.0f, 0.0f, 0.0f);
            sample.hit = true;
            sample.height = surface.y;
            sample.normal = (terrainTransform->modelMatrix * ground.normal - origin).normalized();
            return sample;
        };
    }

    const spline::SplineStationReport report = spline::buildStations(spline.spline, options, stations);
    options.localToWorld = nullptr;
    if (options.conform != spline::SplineDrape::None){
        if (report.allMissed){
            addWarning(warnings, SplineWarningKind::NoTerrain, -1, "No terrain under the spline: it is not conformed.");
        }else if (report.probeMisses > 0){
            addWarning(warnings, SplineWarningKind::ConformMiss, -1,
                       std::to_string(report.probeMisses) + " stations are off the terrain: they hold the nearest height.");
        }
        if (*stationsOverHole > 0){
            addWarning(warnings, SplineWarningKind::OverHole, -1,
                       std::to_string(*stationsOverHole) + " stations are over a terrain hole: they hold the height map's height, so the spline bridges it.");
        }
    }
    (void)entity;
    return stations.size() >= 2;
}

bool SplineSystem::getWorldStations(Entity owner, float spacing, std::vector<spline::SplineStation>& stations, bool conform){
    stations.clear();
    const SplineComponent* spline = scene->findComponent<SplineComponent>(owner);
    const Transform* transform = scene->findComponent<Transform>(owner);
    if (!spline || !transform){
        return false;
    }
    const SplineExtrudeComponent* extrude = scene->findComponent<SplineExtrudeComponent>(owner);
    spline::SplineStationOptions options;
    options.stationSpacing = spacing > 0.0f ? spacing : (extrude ? std::max(extrude->stationSpacing, 0.05f) : 1.0f);
    if (conform && extrude){
        options.conform = stationConform(extrude->conform);
        options.slopeBlend = extrude->slopeBlend;
    }
    if (extrude && extrude->widthSource == SplineWidthSource::Constant){
        options.constantWidth = true;
        options.halfWidth = extrude->width * 0.5f;
    }
    std::vector<SplineWarning> warnings;
    return collectStations(owner, *spline, *transform, options, stations, warnings);
}

// ---- Spline Extrude

void SplineSystem::scheduleExtrude(Entity entity, Resolve& resolve, const SplineComponent& spline, const Transform& transform,
                                   const SplineExtrudeComponent& extrude){
    std::shared_ptr<BuildJob> job = std::make_shared<BuildJob>();
    job->generation = ++resolve.extrudeGeneration;
    job->startMs = nowMs();
    std::vector<SplineWarning>& warnings = job->warnings;
    const bool flat2D = is2D(spline);
    const float scale = largestScale(transform);

    if (extrude.shape == SplineExtrudeShape::Fill){
        job->kind = BuildJob::Kind::Polygon;
        job->hashes.assign(1, 0);
        job->rebuild.assign(1, 1);
        job->meshes.resize(1);
        if (!spline.spline.effectivelyClosed()){
            addWarning(warnings, SplineWarningKind::Triangulation, -1, "Fill needs a closed spline of three knots or more.");
        }else{
            std::vector<Vector3> outline;
            spline::sampleFillOutline(spline.spline, std::max(extrude.stationSpacing, 0.05f) / scale, outline);
            const Matrix4& world = transform.modelMatrix;
            for (Vector3& p : outline){
                p = world * p;
            }
            spline::SplineFillOptions options;
            options.plane = flat2D ? spline::SplineFillPlane::XY : spline::SplineFillPlane::XZ;
            if (flat2D){
                options.height = (world * Vector3(0.0f, 0.0f, 0.0f)).z + extrude.verticalOffset;
            }else{
                const Vector3 mean = world * Vector3(0.0f, spline::meanKnotHeight(spline.spline), 0.0f);
                options.height = mean.y + extrude.verticalOffset;
            }
            options.tilingU = extrude.tilesPerMetreU;
            options.tilingV = extrude.tilesPerMetreV;
            ChunkMesh& mesh = job->meshes[0];
            spline::SplineFillIssue issue;
            if (spline::buildPolygonFill(outline, options, mesh.vertices, mesh.indices, &issue)){
                AABB bounds;
                for (const spline::SplineVertex& v : mesh.vertices){
                    bounds.merge(v.position);
                }
                finishChunkMesh(mesh, bounds);
            }else{
                addWarning(warnings, SplineWarningKind::Triangulation, -1,
                           std::string("Fill refused: ") + spline::fillErrorReason(issue.error));
            }
        }
        job->stats.stations = 0;
        resolve.extrudeJob = job;
        finishExtrude(entity, resolve);
        return;
    }

    spline::SplineStationOptions options;
    options.stationSpacing = std::max(extrude.stationSpacing, 0.05f) / scale;
    options.conform = flat2D ? spline::SplineDrape::None : stationConform(extrude.conform);
    options.slopeBlend = extrude.slopeBlend;
    if (extrude.widthSource == SplineWidthSource::Constant || extrude.shape == SplineExtrudeShape::WaterBanks){
        options.constantWidth = extrude.widthSource == SplineWidthSource::Constant;
        options.halfWidth = extrude.width * 0.5f / scale;
    }
    std::vector<spline::SplineStation>& stations = job->stations;
    if (!collectStations(entity, spline, transform, options, stations, warnings)){
        job->hashes.clear();
        resolve.extrudeJob = job;
        finishExtrude(entity, resolve);
        return;
    }
    const Vector3 up = flat2D ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(0.0f, 1.0f, 0.0f);
    const bool closed = spline.spline.effectivelyClosed();

    if (extrude.shape == SplineExtrudeShape::WaterBanks){
        job->kind = BuildJob::Kind::Water;
        Entity terrainEntity = spline.terrain.terrain;
        TerrainComponent* terrain = terrainEntity != NULL_ENTITY ? scene->findComponent<TerrainComponent>(terrainEntity)
                                                                 : findTerrainUnder(stations.front().position, terrainEntity);
        const Transform* terrainTransform = terrain ? scene->findComponent<Transform>(terrainEntity) : nullptr;
#if JASPERENGINE_TERRAIN_WORLD
        TerrainWorldRuntime* world = terrain && terrain->world && terrain->world->started() ? terrain->world : nullptr;
#endif
        bool ground = terrain && terrain->heightMap.hasData();
#if JASPERENGINE_TERRAIN_WORLD
        ground = ground || world != nullptr;
#endif
        if (!terrain || !terrainTransform || !ground || flat2D){
            addWarning(warnings, SplineWarningKind::NoTerrain, -1, "WaterBanks needs a terrain under the spline.");
            job->hashes.clear();
            resolve.extrudeJob = job;
            finishExtrude(entity, resolve);
            return;
        }
        for (spline::SplineStation& s : stations){
            s.position.y += extrude.verticalOffset;
        }
        spline::SplineFillParams fill;
        fill.maxHalfWidth = extrude.maxHalfWidth;
        fill.edgeDrop = extrude.waterEdgeDrop;
        fill.seaLevelFloor = extrude.seaLevelFloor ? extrude.seaLevel : spline::SPLINE_NO_SEA_LEVEL_FLOOR;
        job->fillParams.edgeDrop = extrude.waterEdgeDrop;
        job->fillParams.flowSpeedScale = extrude.flowSpeed;
        resolve.waterStations = stations;
        resolve.waterHalfWidth = extrude.maxHalfWidth;
        resolve.waterSpeed = extrude.flow ? extrude.flowSpeed : 0.0f;
#if JASPERENGINE_TERRAIN_WORLD
        if (world){
            // the footprint's block of the world's samples (sample s at s * spacing, the World only
            // translates, R4), within its tiles: an absent tile there reads the base height
            const jasperengine::terrain::WorldLattice& lattice = world->lattice();
            const float spacing = lattice.spacing;
            const Vector3 at = terrainTransform->worldPosition;
            float minX = std::numeric_limits<float>::max(), minZ = minX;
            float maxX = std::numeric_limits<float>::lowest(), maxZ = maxX;
            for (const spline::SplineStation& s : stations){
                minX = std::min(minX, s.position.x - at.x); maxX = std::max(maxX, s.position.x - at.x);
                minZ = std::min(minZ, s.position.z - at.z); maxZ = std::max(maxZ, s.position.z - at.z);
            }
            jasperengine::terrain::TileCoord low, high;
            world->occupancy().bounds(low, high);
            const int n = lattice.tileQuads;
            const float margin = extrude.maxHalfWidth + 2.0f * spacing;
            const int x0 = std::clamp(static_cast<int>(std::floor((minX - margin) / spacing)), low.x * n, (high.x + 1) * n);
            const int x1 = std::clamp(static_cast<int>(std::ceil((maxX + margin) / spacing)), low.x * n, (high.x + 1) * n);
            const int z0 = std::clamp(static_cast<int>(std::floor((minZ - margin) / spacing)), low.z * n, (high.z + 1) * n);
            const int z1 = std::clamp(static_cast<int>(std::ceil((maxZ + margin) / spacing)), low.z * n, (high.z + 1) * n);
            job->worldGround = world->heightReader(jasperengine::terrain::TileCoord(jasperengine::terrain::floorDiv(x0, n) - 1, jasperengine::terrain::floorDiv(z0, n) - 1),
                                                   jasperengine::terrain::TileCoord(jasperengine::terrain::floorDiv(x1, n), jasperengine::terrain::floorDiv(z1, n)));
            job->worldX0 = x0;
            job->worldZ0 = z0;
            job->worldBaseY = at.y;
            job->worldFill = fill;
            job->ground.originX = at.x + static_cast<float>(x0) * spacing;
            job->ground.originZ = at.z + static_cast<float>(z0) * spacing;
            job->ground.spacingX = spacing;
            job->ground.spacingZ = spacing;
            job->ground.countX = static_cast<uint32_t>(x1 - x0 + 1);
            job->ground.countZ = static_cast<uint32_t>(z1 - z0 + 1);
            if (!jobs::hasSystem()){
                job->buildOnWorld();
                resolve.extrudeJob = job;
                finishExtrude(entity, resolve);
            }else{
                BuildJob* raw = job.get();
                job->tasks.push_back(jobs::submit([raw](){ raw->buildOnWorld(); },
                                                  {jobs::Lane::Background, jobs::GateClass::Default, "spline-water-world"}));
                resolve.extrudeJob = job;
            }
            return;
        }
#endif
        // the ground lattice is the heightmap's texel centres (S18 D9; the terrain samples its
        // map at (i + 0.5) / size, MeshSystem::terrainTexelCoord), the footprint's block of them
        TextureData& data = terrain->heightMap.getData();
        const int texels = std::max(2, data.getWidth());
        const float spacing = terrain->terrainSize / static_cast<float>(texels);
        const Matrix4 toLocal = terrainTransform->modelMatrix.inverse();
        float minX = std::numeric_limits<float>::max(), minZ = minX;
        float maxX = std::numeric_limits<float>::lowest(), maxZ = maxX;
        for (const spline::SplineStation& s : stations){
            const Vector3 local = toLocal * s.position;
            minX = std::min(minX, local.x); maxX = std::max(maxX, local.x);
            minZ = std::min(minZ, local.z); maxZ = std::max(maxZ, local.z);
        }
        const float margin = extrude.maxHalfWidth + 2.0f * spacing;
        const float half = terrain->terrainSize * 0.5f;
        const int x0 = std::clamp(static_cast<int>(std::floor((minX - margin + half) / spacing - 0.5f)), 0, texels - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil((maxX + margin + half) / spacing - 0.5f)), 0, texels - 1);
        const int z0 = std::clamp(static_cast<int>(std::floor((minZ - margin + half) / spacing - 0.5f)), 0, texels - 1);
        const int z1 = std::clamp(static_cast<int>(std::ceil((maxZ + margin + half) / spacing - 0.5f)), 0, texels - 1);
        const uint32_t countX = static_cast<uint32_t>(x1 - x0 + 1);
        const uint32_t countZ = static_cast<uint32_t>(z1 - z0 + 1);
        // the lattice in world space: the terrain's transform (translation and uniform scale)
        const Vector3 worldOrigin = terrainTransform->modelMatrix * Vector3((x0 + 0.5f) * spacing - half, 0.0f, (z0 + 0.5f) * spacing - half);
        const float worldSpacing = spacing * largestScale(*terrainTransform);
        job->groundHeights.resize(static_cast<size_t>(countX) * countZ);
        for (uint32_t cz = 0; cz < countZ; cz++){
            for (uint32_t cx = 0; cx < countX; cx++){
                float height = 0.0f;
                Vector3 normal;
                const float lx = (static_cast<float>(x0 + static_cast<int>(cx)) + 0.5f) * spacing - half;
                const float lz = (static_cast<float>(z0 + static_cast<int>(cz)) + 0.5f) * spacing - half;
                MeshSystem::sampleTerrainSurface(*terrain, lx, lz, height, normal);
                job->groundHeights[cz * countX + cx] = (terrainTransform->modelMatrix * Vector3(lx, height, lz)).y;
            }
        }
        job->ground.originX = worldOrigin.x;
        job->ground.originZ = worldOrigin.z;
        job->ground.spacingX = worldSpacing;
        job->ground.spacingZ = worldSpacing;
        job->ground.countX = countX;
        job->ground.countZ = countZ;
        job->ground.heights = job->groundHeights;
        job->floodFill(fill);
        const size_t count = job->fillRanges.size();
        double cells = 0.0;
        for (const spline::SplineFillChunkRange& r : job->fillRanges){
            cells += static_cast<double>(r.maxCornerX - r.minCornerX) * (r.maxCornerZ - r.minCornerZ);
        }
        const double estimateMs = cells * SPLINE_NS_PER_FILL_CELL * 1.0e-6;
        if (estimateMs < INLINE_BUDGET_MS || !jobs::hasSystem()){
            for (size_t i = 0; i < count; i++){
                job->buildChunk(i);
            }
            resolve.extrudeJob = job;
            finishExtrude(entity, resolve);
        }else{
            for (size_t i = 0; i < count; i++){
                BuildJob* raw = job.get();
                job->tasks.push_back(jobs::submit([raw, i](){ raw->buildChunk(i); },
                                                  {jobs::Lane::Background, jobs::GateClass::Default, "spline-water-chunk"}));
            }
            resolve.extrudeJob = job;
        }
        return;
    }

    // the swept strip: corners, steps, taper, then chunks
    job->kind = BuildJob::Kind::Strip;
    job->profile = buildExtrudeProfile(extrude);
    if (!job->profile.isValid()){
        job->hashes.clear();
        resolve.extrudeJob = job;
        finishExtrude(entity, resolve);
        return;
    }
    if (closed){
        spline::closeStationLoop(stations, up);
    }
    std::vector<spline::SplineCornerSite> sites;
    std::vector<float> pointDistances;
    for (uint32_t s = 0; s < stations.size(); s++){
        const float t = stations[s].splineT;
        if (t >= 0.0f && t == std::floor(t)){
            const uint32_t knot = static_cast<uint32_t>(t);
            const bool end = !closed && (s == 0 || s + 1 == stations.size());
            const bool seamRepeat = closed && s + 1 == stations.size();
            if (!end && !seamRepeat){
                sites.push_back({s, knot % static_cast<uint32_t>(std::max<size_t>(1, spline.spline.knotCount()))});
                if (s != 0){
                    pointDistances.push_back(stations[s].distance);
                }
            }
        }
    }
    std::vector<spline::SplineCornerIssue> issues;
    if (extrude.corners != SplineCornerStyle::None && spline.spline.type() == spline::SplineType::Linear){
        // corners are authored turns: a curve type's knots are smooth
        issues = spline::applyCorners(stations, sites, cornerStyle(extrude.corners), up, closed);
    }
    {
        std::vector<uint8_t> skip(stations.size(), 0);
        for (uint32_t s = 0; s < stations.size(); s++){
            if (stations[s].join != spline::SplineStationJoin::Advance){
                skip[s] = 1;
                if (s > 0) skip[s - 1] = 1;
                if (s + 1 < stations.size()) skip[s + 1] = 1;
            }
        }
        const std::vector<uint32_t> folds = spline::findInnerFolds(stations, skip, up);
        const std::vector<spline::SplineCornerIssue> foldIssues =
            spline::innerFoldIssues(stations, folds, static_cast<uint32_t>(spline.spline.knotCount()), closed);
        issues.insert(issues.end(), foldIssues.begin(), foldIssues.end());
    }
    for (const spline::SplineCornerIssue& issue : issues){
        switch (issue.kind){
            case spline::SplineCornerIssueKind::PastMitreLimit:
                addWarning(warnings, SplineWarningKind::MitreLimit, static_cast<int>(issue.pointIndex),
                           knotText(issue.pointIndex) + " turns " + std::to_string(static_cast<int>(issue.turnDegrees)) +
                           " degrees, past the mitre limit (120): built Round.");
                break;
            case spline::SplineCornerIssueKind::CornersOverlap:
                addWarning(warnings, SplineWarningKind::CornersOverlap, static_cast<int>(issue.pointIndex),
                           "The corners at " + knotText(issue.pointIndex) + " and " + knotText(issue.otherPointIndex) + " overlap.");
                break;
            case spline::SplineCornerIssueKind::LegShorterThanReach:
                addWarning(warnings, SplineWarningKind::CornersOverlap, static_cast<int>(issue.pointIndex),
                           "The corner at " + knotText(issue.pointIndex) + " reaches past the end of the spline.");
                break;
            case spline::SplineCornerIssueKind::InnerFold:
                addWarning(warnings, SplineWarningKind::InnerFold, static_cast<int>(issue.pointIndex),
                           "Between " + knotText(issue.pointIndex) + " and " + knotText(issue.otherPointIndex) +
                           " the curve is tighter than the width: the inside folds.");
                break;
        }
    }
    spline::accumulateFaceTurn(stations, up);
    if (extrude.shape == SplineExtrudeShape::Rectangle && extrude.grade == SplineGrade::Stepped){
        spline::stepTopsAtPoints(stations, pointDistances, up, closed, std::max(0.0f, extrude.stepHeight));
    }
    if (extrude.endTaper > 0.0f){
        spline::SplineEndTaperParams taper;
        taper.taperMetres = extrude.endTaper;
        taper.closedLoop = closed;
        spline::applyEndTaper(stations, taper);
    }

    spline::SplineStripParams& params = job->params;
    params.widthScale = extrude.shape == SplineExtrudeShape::Tube ? spline::SplineProfileScale::Uniform
                      : extrude.shape == SplineExtrudeShape::Rectangle ? spline::SplineProfileScale::None
                      : spline::SplineProfileScale::LateralOnly;
    params.tilesPerMetreU = extrude.tilesPerMetreU;
    params.tilesPerMetreV = extrude.tilesPerMetreV;
    params.uOriginMetres = 0.0f;
    if (closed){
        const spline::SplineStation& last = stations.back();
        params.loopLengthMetres = last.distance;
        params.loopFaceTurnLeft = last.faceTurnLeft;
        params.loopFaceTurnRight = last.faceTurnRight;
        params.loopTopArcAllowance = last.topArcAllowance;
    }
    params.generateCaps = extrude.endCaps && !closed;
    if (extrude.levelCourseV){
        params.heightVDatumMetres = 0.0f;
    }
    params.lateralOffset = extrude.lateralOffset;
    params.verticalOffset = extrude.verticalOffset;
    params.writeFlow = extrude.flow;
    params.flowSpeedScale = extrude.flowSpeed;

    job->ranges = spline::planSplineStripChunks(job->profile, stations, params, CHUNK_LENGTH, CHUNK_MAX_VERTICES);
    const size_t count = job->ranges.size();
    job->hashes.resize(count);
    job->rebuild.assign(count, 1);
    job->meshes.resize(count);

    Hasher common;
    common.u(resolve.extrudeHash);
    common.u(flat2D);
    // the run's U origin is its first station's distance; a closed loop's seam and U density
    // also depend on its whole length (snapped to whole tiles), an open run's chunks do not
    common.f(stations.front().distance);
    common.u(params.snapUForClosedLoop); common.u(closed);
    if (params.snapUForClosedLoop || closed){
        common.f(stations.back().distance);
        common.f(params.loopLengthMetres); common.f(params.loopFaceTurnLeft); common.f(params.loopFaceTurnRight);
        common.f(params.loopTopArcAllowance);
    }
    for (const spline::SplineProfilePoint& p : job->profile.points){
        common.f(p.position.x); common.f(p.position.y); common.u(p.hard); common.u(p.grounded);
    }
    double ringPoints = 0.0;
    for (size_t i = 0; i < count; i++){
        const spline::SplineStripChunkRange& r = job->ranges[i];
        Hasher h = common;
        h.u(r.firstStation); h.u(r.endStation); h.u(r.opensRun); h.u(r.closesRun);
        for (uint32_t s = r.firstStation; s < r.endStation && s < stations.size(); s++){
            hashStation(h, stations[s]);
        }
        job->hashes[i] = h.value;
        bool kept = false;
        for (const ExtrudeChunk& chunk : resolve.extrudeChunks){
            if (chunk.hash == h.value && chunk.entity != NULL_ENTITY){
                kept = true;
                break;
            }
        }
        if (kept){
            job->rebuild[i] = 0;
        }else{
            ringPoints += static_cast<double>(r.endStation - r.firstStation) * job->profile.points.size();
        }
    }
    job->stats.stations = static_cast<uint32_t>(stations.size());

    // the water surface of a Flat channel with flow
    if (extrude.shape == SplineExtrudeShape::Flat && extrude.flow && !flat2D){
        resolve.waterStations = stations;
        resolve.waterHalfWidth = 0.0f;
        resolve.waterSpeed = extrude.flowSpeed;
    }else{
        resolve.waterStations.clear();
    }

    const double estimateMs = ringPoints * SPLINE_NS_PER_RING_POINT * 1.0e-6;
    if (estimateMs < INLINE_BUDGET_MS || !jobs::hasSystem()){
        for (size_t i = 0; i < count; i++){
            if (job->rebuild[i]) job->buildChunk(i);
        }
        resolve.extrudeJob = job;
        finishExtrude(entity, resolve);
    }else{
        BuildJob* raw = job.get();
        for (size_t i = 0; i < count; i++){
            if (!job->rebuild[i]) continue;
            job->tasks.push_back(jobs::submit([raw, i](){ raw->buildChunk(i); },
                                              {jobs::Lane::Background, jobs::GateClass::Default, "spline-strip-chunk"}));
        }
        resolve.extrudeJob = job;
    }
}

void SplineSystem::finishExtrude(Entity entity, Resolve& resolve){
    std::shared_ptr<BuildJob> job = std::move(resolve.extrudeJob);
    if (!job || job->generation != resolve.extrudeGeneration){
        // a newer build superseded it: its results are stale
        return;
    }
    applyExtrudeChunks(entity, resolve, *job);
}

void SplineSystem::applyExtrudeChunks(Entity entity, Resolve& resolve, BuildJob& job){
    std::vector<ExtrudeChunk> previous = std::move(resolve.extrudeChunks);
    std::vector<uint8_t> reused(previous.size(), 0);
    std::vector<ExtrudeChunk> next;
    next.reserve(job.meshes.size());
    SplineBuildStats stats = job.stats;
    stats.generation = job.generation;

    for (size_t i = 0; i < job.meshes.size(); i++){
        ExtrudeChunk chunk;
        chunk.hash = i < job.hashes.size() ? job.hashes[i] : 0;
        if (i < job.rebuild.size() && !job.rebuild[i]){
            for (size_t p = 0; p < previous.size(); p++){
                if (!reused[p] && previous[p].hash == chunk.hash){
                    chunk = previous[p];
                    reused[p] = 1;
                    break;
                }
            }
            if (chunk.entity != NULL_ENTITY){
                stats.vertices += chunk.vertices;
                stats.triangles += chunk.triangles;
                next.push_back(chunk);
                continue;
            }
            // the kept chunk is gone: build it now
            job.buildChunk(i);
        }
        ChunkMesh& mesh = job.meshes[i];
        if (mesh.vertices.empty() || mesh.indices.empty()){
            continue;
        }
        // an entity of an unused old chunk takes the new mesh
        for (size_t p = 0; p < previous.size() && chunk.entity == NULL_ENTITY; p++){
            if (!reused[p] && previous[p].entity != NULL_ENTITY){
                chunk.entity = previous[p].entity;
                reused[p] = 1;
            }
        }
        if (chunk.entity == NULL_ENTITY){
            chunk.entity = createChunkEntity(entity);
        }
        Transform& transform = scene->getComponent<Transform>(chunk.entity);
        transform.position = mesh.origin;
        transform.rotation = Quaternion();
        transform.scale = Vector3(1.0f, 1.0f, 1.0f);
        transform.needUpdate = true;
        MeshComponent& chunkMesh = scene->getComponent<MeshComponent>(chunk.entity);
        writeMesh(chunkMesh, mesh);
        if (const MeshComponent* owner = scene->findComponent<MeshComponent>(entity)){
            copyMeshLook(*owner, chunkMesh);
        }
        chunk.worldBounds = mesh.bounds;
        chunk.vertices = static_cast<uint32_t>(mesh.vertices.size());
        chunk.triangles = static_cast<uint32_t>(mesh.indices.size() / 3);
        stats.vertices += chunk.vertices;
        stats.triangles += chunk.triangles;
        stats.chunksRebuilt++;
        next.push_back(chunk);
        // the meshes are copied: free the job's
        mesh.vertices = std::vector<spline::SplineVertex>();
        mesh.indices = std::vector<uint32_t>();
    }
    for (size_t p = 0; p < previous.size(); p++){
        if (!reused[p]){
            destroyGenerated(previous[p].entity);
        }
    }
    resolve.extrudeChunks = std::move(next);
    stats.chunks = static_cast<uint32_t>(resolve.extrudeChunks.size());
    stats.length = job.stations.empty() ? 0.0f : job.stations.back().distance;
    stats.buildMs = nowMs() - job.startMs;
    resolve.extrudeStats = stats;
    resolve.extrudeWarnings = job.warnings;
    resolve.materialSynced = true;
    if (const MeshComponent* owner = scene->findComponent<MeshComponent>(entity)){
        resolve.material = owner->submeshes[0].material;
    }

    // the water surface query
    resolve.waterGrid = spline::SplineStationGridXZ();
    resolve.waterBounds = AABB();
    if (!resolve.waterStations.empty()){
        resolve.waterGrid.build(resolve.waterStations);
        for (const spline::SplineStation& s : resolve.waterStations){
            const float reach = std::max(resolve.waterHalfWidth, std::max(s.halfWidthLeft, s.halfWidthRight));
            resolve.waterBounds.merge(s.position - Vector3(reach, 1.0f, reach));
            resolve.waterBounds.merge(s.position + Vector3(reach, 1.0f, reach));
        }
    }

    updateCollision(entity, resolve, scene->findComponent<SplineExtrudeComponent>(entity));
}

void SplineSystem::syncChunkMaterials(Entity entity, Resolve& resolve){
    const MeshComponent* owner = scene->findComponent<MeshComponent>(entity);
    if (!owner || resolve.extrudeChunks.empty()){
        return;
    }
    if (resolve.materialSynced && owner->submeshes[0].material == resolve.material){
        bool same = true;
        const MeshComponent& first = scene->getComponent<MeshComponent>(resolve.extrudeChunks.front().entity);
        same = first.castShadows == owner->castShadows && first.receiveShadows == owner->receiveShadows &&
               first.receiveLights == owner->receiveLights && first.transparent == owner->transparent &&
               first.cullingMode == owner->cullingMode && first.submeshes[0].faceCulling == owner->submeshes[0].faceCulling;
        if (same){
            return;
        }
    }
    resolve.material = owner->submeshes[0].material;
    resolve.materialSynced = true;
    for (const ExtrudeChunk& chunk : resolve.extrudeChunks){
        MeshComponent& mesh = scene->getComponent<MeshComponent>(chunk.entity);
        copyMeshLook(*owner, mesh);
        if (mesh.loaded){
            mesh.needReload = true;
        }
    }
}

void SplineSystem::updateCollision(Entity entity, Resolve& resolve, const SplineExtrudeComponent* extrude){
    const SplineCollision wanted = extrude ? extrude->collision : SplineCollision::None;
#ifdef JASPERENGINE_PHYSICS_3D
    if (std::shared_ptr<PhysicsSystem> physics = scene->findSystem<PhysicsSystem>()){
        for (const ExtrudeChunk& chunk : resolve.extrudeChunks){
            const bool has = scene->findComponent<RigidbodyComponent>(chunk.entity) != nullptr;
            if (wanted == SplineCollision::Mesh){
                // a body of the chunk's current triangles: a rebuilt chunk's old body goes
                if (has){
                    physics->removeRigidbody(chunk.entity);
                }
                physics->createRigidbody(chunk.entity);
                Rigidbody body(scene, chunk.entity);
                body.setType(BodyType::STATIC);
                body.createMeshCollider();
            }else if (has){
                physics->removeRigidbody(chunk.entity);
            }
        }
    }
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    for (Entity body : resolve.chainBodies){
        destroyGenerated(body);
    }
    resolve.chainBodies.clear();
    const SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    std::shared_ptr<PhysicsSystem> physics = scene->findSystem<PhysicsSystem>();
    if (wanted == SplineCollision::Chain2D && spline && transform && physics && spline->spline.isValid()){
        // The centreline in world XY as Box2D chains, collided from both sides like Unity's
        // EdgeCollider2D. A Box2D chain is one-sided (its right) and an open one does not collide
        // on its first and last edge, so: every chain is made twice, the second reversed; the
        // line gets a ghost point beyond each open end (a loop: its neighbours across the seam);
        // it is cut into chains of at most kChainPoints points overlapping by three
        // (Box2D's smooth join, every edge an inner edge of one chain); kChainsPerBody chains
        // a body. Points stay farther apart than twice Box2D's linear slop.
        const float scale = largestScale(*transform);
        const float pointsPerMetre = physics->getPointsToMeterScale2D();
        const float minGap = 2.0f * 0.005f * pointsPerMetre;
        std::vector<spline::SplineFrame> frames;
        const float length = spline::length(spline->spline) * scale;
        const float spacing = std::max(0.05f * pointsPerMetre, minGap);
        const uint32_t samples = std::max<uint32_t>(2u, std::min<uint32_t>(20000u, static_cast<uint32_t>(std::ceil(length / spacing)) + 1u));
        spline::sampleUniform(spline->spline, samples, frames);
        std::vector<Vector3> points;
        points.reserve(frames.size());
        for (const spline::SplineFrame& f : frames){
            points.push_back(transform->modelMatrix * f.position);
        }
        const bool loop = spline->spline.effectivelyClosed();
        if (loop && points.size() > 1){
            points.pop_back();
        }
        std::vector<uint32_t> kept;
        spline::simplifyIndices(points, 0.002f * pointsPerMetre, kept);
        std::vector<Vector2> line;
        line.reserve(kept.size() + 3);
        for (uint32_t k : kept){
            const Vector2 p(points[k].x, points[k].y);
            if (line.empty() || (p - line.back()).length() > minGap){
                line.push_back(p);
            }
        }
        if (loop){
            while (line.size() > 2 && (line.front() - line.back()).length() <= minGap){
                line.pop_back();
            }
        }
        const bool usable = loop ? line.size() >= 3 : line.size() >= 2;
        std::vector<Vector2> chain;
        if (usable){
            if (loop){
                // [last, all, first, second]: every edge of the loop is an inner edge
                chain.push_back(line.back());
                chain.insert(chain.end(), line.begin(), line.end());
                chain.push_back(line[0]);
                chain.push_back(line[1]);
            }else{
                const Vector2 headStep = line[0] - line[1];
                const Vector2 tailStep = line[line.size() - 1] - line[line.size() - 2];
                chain.push_back(line[0] + headStep);
                chain.insert(chain.end(), line.begin(), line.end());
                chain.push_back(line.back() + tailStep);
            }
        }
        // the chunks of the capped collider arrays from before upstream's unbounded collider
        // lists (16 points a chain, 10 colliders a body): the generated bodies stay as they were
        constexpr size_t kChainPoints = 16;
        constexpr int kChainsPerBody = 10;
        const size_t perChain = std::max<size_t>(4, kChainPoints);
        size_t start = 0;
        Entity bodyEntity = NULL_ENTITY;
        int chainsInBody = kChainsPerBody;
        while (chain.size() >= 4 && start + 3 < chain.size()){
            const size_t end = std::min(chain.size(), start + perChain);
            if (chainsInBody + 2 > kChainsPerBody){
                bodyEntity = scene->createEntity();
                scene->addComponent<Transform>(bodyEntity);
                generatedOwners[bodyEntity] = entity;
                physics->createRigidbody2D(bodyEntity);
                Rigidbody2D(scene, bodyEntity).setType(BodyType::STATIC);
                resolve.chainBodies.push_back(bodyEntity);
                chainsInBody = 0;
            }
            std::vector<Vector2> part(chain.begin() + static_cast<std::ptrdiff_t>(start), chain.begin() + static_cast<std::ptrdiff_t>(end));
            Rigidbody2D(scene, bodyEntity).createChainCollider(part, false);
            std::reverse(part.begin(), part.end());
            Rigidbody2D(scene, bodyEntity).createChainCollider(part, false);
            chainsInBody += 2;
            if (end == chain.size()){
                break;
            }
            start = end - 3;
        }
    }
#endif
    resolve.collision = wanted;
    (void)entity;
}

// ---- Spline Instantiate

bool SplineSystem::preparePools(Entity entity, Resolve& resolve, const SplineInstantiateComponent& instantiate){
    // the pool paths in use, in pool order (items, posts, spans, gates, crests, caps)
    std::vector<std::pair<std::string, float>> wanted;
    auto add = [&wanted](const std::vector<SplinePoolEntry>& pool){
        for (size_t i = 0; i < pool.size() && i < SplineInstantiateComponent::MAX_POOL_ENTRIES; i++){
            if (!pool[i].asset.path.empty()){
                wanted.emplace_back(pool[i].asset.path, pool[i].weight);
            }
        }
    };
    if (instantiate.mode == SplineInstantiateMode::Fence){
        add(instantiate.posts); add(instantiate.spans); add(instantiate.gates); add(instantiate.crests); add(instantiate.caps);
    }else{
        add(instantiate.items);
    }
    std::vector<PoolModel> next;
    for (const auto& item : wanted){
        bool found = false;
        for (const PoolModel& p : next){
            if (p.path == item.first){ found = true; break; }
        }
        if (found) continue;
        PoolModel model;
        for (PoolModel& old : resolve.pools){
            if (old.path == item.first){
                model = old;
                old.probe = NULL_ENTITY;
                break;
            }
        }
        model.path = item.first;
        model.weight = item.second;
        model.prefab = isPrefabPath(item.first);
        next.push_back(model);
    }
    for (PoolModel& old : resolve.pools){
        destroyGenerated(old.probe);
    }
    resolve.pools = std::move(next);

    // measure every model once: loaded into a hidden probe GameObject
    std::shared_ptr<MeshSystem> meshSystem = scene->findSystem<MeshSystem>();
    for (PoolModel& model : resolve.pools){
        if (model.loaded || model.failed){
            continue;
        }
        if (model.prefab){
            // a prefab is measured by its root's mesh where it has one; else a unit box
            model.bounds = AABB(Vector3(-0.5f, 0.0f, -0.5f), Vector3(0.5f, 1.0f, 0.5f));
            model.loaded = true;
            continue;
        }
        if (!meshSystem){
            model.failed = true;
            continue;
        }
        model.probe = scene->createEntity();
        scene->addComponent<Transform>(model.probe);
        scene->addComponent<MeshComponent>(model.probe);
        scene->addComponent<ModelComponent>(model.probe);
        scene->getComponent<ModelComponent>(model.probe).mergeStaticMeshes = true;
        generatedOwners[model.probe] = entity;
        scene->getComponent<Transform>(model.probe).visible = false;
        bool ok = false;
        const std::string extension = model.path.size() > 4 ? model.path.substr(model.path.size() - 4) : std::string();
        if (extension == ".obj"){
            ok = meshSystem->loadOBJ(model.probe, model.path);
        }else{
            ok = meshSystem->loadGLTF(model.probe, model.path, false, true, false, true);
        }
        if (!ok){
            model.failed = true;
            destroyGenerated(model.probe);
            model.probe = NULL_ENTITY;
            continue;
        }
        MeshComponent& mesh = scene->getComponent<MeshComponent>(model.probe);
        meshSystem->calculateMeshAABB(mesh);
        model.bounds = mesh.verticesAABB;
        model.loaded = true;
    }
    return true;
}

void SplineSystem::scheduleInstantiate(Entity entity, Resolve& resolve, const SplineComponent& spline, const Transform& transform,
                                       const SplineInstantiateComponent& instantiate){
    std::shared_ptr<BuildJob> job = std::make_shared<BuildJob>();
    job->kind = BuildJob::Kind::Pieces;
    job->generation = ++resolve.piecesGeneration;
    job->startMs = nowMs();
    std::vector<SplineWarning>& warnings = job->warnings;
    const bool flat2D = is2D(spline);
    const float scale = largestScale(transform);

    spline::SplineStationOptions options;
    options.stationSpacing = std::max(instantiate.stationSpacing, 0.05f) / scale;
    options.conform = flat2D ? spline::SplineDrape::None : stationConform(instantiate.conform);
    options.slopeBlend = instantiate.slopeBlend;
    options.verticalOffset = instantiate.verticalOffset;
    std::vector<spline::SplineStation>& stations = job->stations;
    if (!collectStations(entity, spline, transform, options, stations, warnings)){
        applyPieces(entity, resolve, *job);
        return;
    }
    job->stats.stations = static_cast<uint32_t>(stations.size());

    spline::centerlineFromStations(stations, job->center);
    job->instantiate = instantiate;
    job->closedSpline = spline.spline.effectivelyClosed();

    // the pools as pieces, by role; poolIndex is the slot in job->paths
    auto piecesOf = [&resolve, &job](const std::vector<SplinePoolEntry>& pool, std::vector<spline::SplinePiece>& out){
        for (size_t i = 0; i < pool.size() && i < SplineInstantiateComponent::MAX_POOL_ENTRIES; i++){
            for (const PoolModel& model : resolve.pools){
                if (model.path == pool[i].asset.path && model.loaded && !model.failed){
                    out.push_back(spline::makeSplinePiece(static_cast<uint32_t>(job->paths.size()), model.bounds,
                                                          Vector3(0.0f, 0.0f, 0.0f), std::max(pool[i].weight, 0.0f)));
                    job->paths.push_back(model.path);
                    break;
                }
            }
        }
    };
    if (instantiate.mode == SplineInstantiateMode::Fence){
        piecesOf(instantiate.posts, job->posts);
        piecesOf(instantiate.spans, job->spans);
        piecesOf(instantiate.gates, job->gates);
        piecesOf(instantiate.crests, job->crests);
        piecesOf(instantiate.caps, job->caps);
        for (uint32_t s = 0; s < stations.size(); s++){
            const float t = stations[s].splineT;
            if (t >= 0.0f && t == std::floor(t)){
                job->knotStations.push_back(static_cast<float>(s));
            }
        }
        for (size_t i = 0; i < instantiate.spanOverrides.size() && i < SplineInstantiateComponent::MAX_SPAN_OVERRIDES; i++){
            const SplineSpanOverride& o = instantiate.spanOverrides[i];
            spline::SplineSpanOverride over;
            over.knotIndex = o.knot;
            over.spanOrdinal = static_cast<uint16_t>(o.span);
            over.kind = o.kind == SplineSpanOverrideKind::Gate ? spline::SplineSpanOverrideKind::Gate
                                                               : spline::SplineSpanOverrideKind::ExplicitPiece;
            over.poolSlot = static_cast<uint8_t>(o.piece);
            job->overrides.push_back(over);
        }
    }else{
        piecesOf(instantiate.items, job->items);
    }
    if (job->paths.empty()){
        addWarning(warnings, SplineWarningKind::PoolEmpty, -1, "No pool entry could be loaded: nothing is placed.");
    }

    // the estimate: the last build's pieces, else one per spacing (or post pitch)
    const float length = stations.back().distance;
    const float pitch = instantiate.mode == SplineInstantiateMode::Fence ? instantiate.postPitch : instantiate.spacing;
    const double pieces = resolve.instantiateStats.instances > 0 ? resolve.instantiateStats.instances
                        : length / std::max(pitch, 0.01f);
    const double estimateMs = pieces * SPLINE_NS_PER_PIECE * 1.0e-6;
    if (estimateMs < INLINE_BUDGET_MS || !jobs::hasSystem()){
        job->layoutPieces();
        applyPieces(entity, resolve, *job);
    }else{
        BuildJob* raw = job.get();
        job->tasks.push_back(jobs::submit([raw](){ raw->layoutPieces(); },
                                          {jobs::Lane::Background, jobs::GateClass::Default, "spline-layout"}));
        resolve.piecesJob = job;
    }
}

void SplineSystem::BuildJob::layoutPieces(){
    JE_PROFILE_SCOPE("SplineSystem::layoutPieces");
    const WorkTimer timer;
    std::vector<spline::SplinePiecePose> placed;
    const uint32_t maxPieces = SplineSystem::MAX_PIECES;
    const SplineInstantiateComponent& c = instantiate;
    if (c.mode == SplineInstantiateMode::Fence){
        spline::SplineTiltClampReport tilt;
        spline::SplineFenceLayoutParams params;
        params.knotStations = knotStations;
        params.closed = closedSpline;
        params.postPitch = c.postPitch;
        params.spanMaxStretch = c.spanMaxStretch;
        params.seed = c.seed;
        params.spanGrade = c.fenceGrade == SplineGrade::Stepped ? spline::SplineSpanGrade::Stepped : spline::SplineSpanGrade::Racked;
        params.plantMode = c.plantMode == SplinePlantMode::Pivot ? spline::SplinePlantMode::Pivot : spline::SplinePlantMode::BoundsMin;
        params.alignToSurfaceNormal = c.conform == SplineConform::TerrainHeightAndSlope;
        params.slopeBlend = c.slopeBlend;
        params.maxTiltDegrees = c.maxTilt;
        params.outTiltClamp = &tilt;
        params.postPieces = posts;
        params.spanPieces = spans;
        params.gatePieces = gates;
        params.crestPieces = crests;
        params.capPieces = caps;
        params.spanOverrides = overrides;
        params.crestPitch = c.crestPitch;
        params.maxPieces = maxPieces;
        params = spline::sanitizeFenceParams(params);
        params.outTiltClamp = &tilt;
        const spline::SplineFenceLayoutResult layout = spline::buildFenceLayout(center, params);
        spline::appendFencePiecePoses(layout, params, placed);
        layoutMessages = layout.validation;
    }else if (!items.empty()){
        spline::SplineTiltClampReport tilt;
        spline::SplineTileLayoutParams params;
        params.mode = c.mode == SplineInstantiateMode::Tile ? spline::SplineTileMode::Tile : spline::SplineTileMode::Spacing;
        params.pieces = items;
        params.spacing = std::max(c.spacing, 0.01f);
        params.fit = c.fit == SplineSpacingFit::FixedPitch ? spline::SplinePlacementFit::FixedPitch
                                                          : spline::SplinePlacementFit::FitToLength;
        params.alignToSurfaceNormal = c.conform == SplineConform::TerrainHeightAndSlope;
        params.slopeBlend = c.slopeBlend;
        params.maxTiltDegrees = c.maxTilt;
        params.plantMode = c.plantMode == SplinePlantMode::Pivot ? spline::SplinePlantMode::Pivot : spline::SplinePlantMode::BoundsMin;
        params.lateralOffset = c.lateralOffset;
        params.seed = c.seed;
        params.spacingJitterMetres = c.spacingJitter;
        params.yawJitterDegrees = c.yawJitter;
        params.lateralJitterMetres = c.lateralJitter;
        params.scaleMin = std::min(c.scaleRange.x, c.scaleRange.y);
        params.scaleMax = std::max(c.scaleRange.x, c.scaleRange.y);
        params.dropoutChance = std::clamp(c.dropout, 0.0f, 1.0f);
        params.startTrimMetres = c.endTrim;
        params.endTrimMetres = c.endTrim;
        params.maxPieces = maxPieces;
        params.outTiltClamp = &tilt;
        spline::buildTileLayout(center, params, placed);
    }
    capped = placed.size() >= maxPieces;

    // grouped by path, in pose order
    for (const spline::SplinePiecePose& pose : placed){
        if (pose.poolIndex >= paths.size()) continue;
        const std::string& path = paths[pose.poolIndex];
        auto it = std::find_if(poses.begin(), poses.end(), [&path](const auto& p){ return p.first == path; });
        if (it == poses.end()){
            poses.emplace_back(path, std::vector<spline::SplinePiecePose>());
            it = poses.end() - 1;
        }
        it->second.push_back(pose);
    }
    instances = static_cast<uint32_t>(placed.size());
}

// T3 R26: an item whose origin is over a hole goes, as dropout takes one: after the layout, so the
// items that stay keep their places and picks. On the main thread, where the terrain's maps are.
void SplineSystem::dropPiecesOverHoles(const SplineComponent& spline, BuildJob& job){
    auto terrains = scene->getComponentArray<TerrainComponent>();
    bool anyHoles = false;
    for (size_t i = 0; i < terrains->size() && !anyHoles; i++){
        anyHoles = !terrains->getComponentFromIndex(i).holeMap.empty();
    }
    if (!anyHoles){
        return;
    }
    const Entity named = spline.terrain.terrain;
    for (auto& group : job.poses){
        std::vector<spline::SplinePiecePose>& poses = group.second;
        const auto kept = std::remove_if(poses.begin(), poses.end(), [&](const spline::SplinePiecePose& pose){
            Entity terrainEntity = named;
            TerrainComponent* terrain = named != NULL_ENTITY ? scene->findComponent<TerrainComponent>(named)
                                                             : findTerrainUnder(pose.position, terrainEntity);
            const Transform* transform = terrain ? scene->findComponent<Transform>(terrainEntity) : nullptr;
            if (!terrain || !transform || terrain->holeMap.empty()){
                return false;
            }
            const Vector3 local = transform->modelMatrix.inverse() * pose.position;
            return querySurface(*terrain, local.x, local.z).hole;
        });
        job.instances -= static_cast<uint32_t>(poses.end() - kept);
        poses.erase(kept, poses.end());
    }
}

void SplineSystem::applyPieces(Entity entity, Resolve& resolve, BuildJob& job){
    if (const SplineComponent* spline = scene->findComponent<SplineComponent>(entity)){
        dropPiecesOverHoles(*spline, job);
    }
    std::vector<PieceChunk> previous = std::move(resolve.pieceChunks);
    std::vector<uint8_t> reused(previous.size(), 0);
    std::vector<PieceChunk> next;
    for (Entity root : resolve.prefabRoots){
        destroyPrefabPiece(root);
    }
    resolve.prefabRoots.clear();
    SplineBuildStats stats = job.stats;
    stats.generation = job.generation;
    std::vector<SplineWarning> warnings = job.warnings;
    for (const std::string& message : job.layoutMessages){
        addWarning(warnings, SplineWarningKind::InstanceCap, -1, message);
    }
    if (job.capped){
        addWarning(warnings, SplineWarningKind::InstanceCap, -1,
                   "More than " + std::to_string(MAX_PIECES) + " pieces: the rest are not placed.");
    }
    std::shared_ptr<MeshSystem> meshSystem = scene->findSystem<MeshSystem>();
    uint32_t prefabCount = 0;
    resolve.placements.clear();
    for (const auto& group : job.poses){
        for (const spline::SplinePiecePose& pose : group.second){
            resolve.placements.push_back({group.first, pose.position, pose.rotation, pose.scale});
        }
    }

    for (const auto& group : job.poses){
        const std::string& path = group.first;
        const std::vector<spline::SplinePiecePose>& poses = group.second;
        if (isPrefabPath(path)){
            std::vector<Matrix4> matrices;
            for (const spline::SplinePiecePose& pose : poses){
                if (prefabCount >= MAX_PREFAB_INSTANCES) break;
                matrices.push_back(Matrix4::translateMatrix(pose.position) * pose.rotation.getRotationMatrix() * Matrix4::scaleMatrix(pose.scale));
                prefabCount++;
            }
            std::vector<Entity> roots;
            const uint32_t id = PrefabManager::findPrefab(path);
            if (id == 0){
                // prefabs are built by the factories the game (or the editor's Play) registers
                addWarning(warnings, SplineWarningKind::PrefabsNotInEditor, -1,
                           "Prefab pieces are GameObjects in Play only (Bake to GameObjects places them now).");
            }else{
                for (const Matrix4& m : matrices){
                    const Entity root = PrefabManager::instantiate(id, scene);
                    if (root == NULL_ENTITY) continue;
                    if (Transform* t = scene->findComponent<Transform>(root)){
                        Vector3 scale;
                        Quaternion rotation;
                        m.decompose(t->position, scale, rotation);
                        t->rotation = rotation;
                        t->scale = scale;
                        t->needUpdate = true;
                    }
                    roots.push_back(root);
                }
            }
            for (Entity root : roots){
                generatedOwners[root] = entity;
                resolve.prefabRoots.push_back(root);
            }
            stats.instances += static_cast<uint32_t>(roots.size());
            continue;
        }
        // instanced chunks of CHUNK_MAX_INSTANCES
        for (size_t start = 0; start < poses.size(); start += CHUNK_MAX_INSTANCES){
            const size_t end = std::min(poses.size(), start + CHUNK_MAX_INSTANCES);
            PieceChunk chunk;
            chunk.path = path;
            for (size_t p = 0; p < previous.size(); p++){
                if (!reused[p] && previous[p].path == path){
                    chunk = previous[p];
                    reused[p] = 1;
                    break;
                }
            }
            if (chunk.entity == NULL_ENTITY){
                chunk.entity = createChunkEntity(entity);
                scene->addComponent<ModelComponent>(chunk.entity);
                scene->addComponent<InstancedMeshComponent>(chunk.entity);
                scene->getComponent<ModelComponent>(chunk.entity).mergeStaticMeshes = true;
                bool ok = false;
                if (meshSystem){
                    const std::string extension = path.size() > 4 ? path.substr(path.size() - 4) : std::string();
                    ok = extension == ".obj" ? meshSystem->loadOBJ(chunk.entity, path)
                                             : meshSystem->loadGLTF(chunk.entity, path, false, true, false, true);
                }
                if (!ok){
                    destroyGenerated(chunk.entity);
                    continue;
                }
            }
            InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(chunk.entity);
            const size_t count = end - start;
            if (instmesh.maxInstances < count){
                instmesh.maxInstances = static_cast<unsigned int>(std::max<size_t>(count, CHUNK_MAX_INSTANCES / 8));
                MeshComponent& mesh = scene->getComponent<MeshComponent>(chunk.entity);
                if (mesh.loaded){
                    mesh.needReload = true;
                }
            }
            instmesh.instances.clear();
            for (size_t p = start; p < end; p++){
                InstanceData data;
                data.position = poses[p].position;
                data.rotation = poses[p].rotation;
                data.scale = poses[p].scale;
                instmesh.instances.push_back(data);
            }
            instmesh.needUpdateInstances = true;
            chunk.count = static_cast<uint32_t>(count);
            stats.instances += chunk.count;
            next.push_back(chunk);
        }
    }
    if (prefabCount >= MAX_PREFAB_INSTANCES){
        addWarning(warnings, SplineWarningKind::InstanceCap, -1,
                   "Prefab pieces are capped at " + std::to_string(MAX_PREFAB_INSTANCES) + " per component.");
    }
    for (size_t p = 0; p < previous.size(); p++){
        if (!reused[p]){
            destroyGenerated(previous[p].entity);
        }
    }
    resolve.pieceChunks = std::move(next);
    stats.chunks = static_cast<uint32_t>(resolve.pieceChunks.size());
    stats.length = job.stations.empty() ? 0.0f : job.stations.back().distance;
    stats.buildMs = nowMs() - job.startMs;
    resolve.instantiateStats = stats;
    resolve.instantiateWarnings = std::move(warnings);
}

// ---- Spline Animate

void SplineSystem::updateAnimate(double dt){
    auto followers = scene->getComponentArray<SplineAnimateComponent>();
    if (followers->size() == 0){
        return;
    }
    for (size_t i = 0; i < followers->size(); i++){
        const Entity entity = followers->getEntity(i);
        SplineAnimateComponent& animate = followers->getComponentFromIndex(i);
        if (!animate.enabled || paused){
            continue;
        }
        if (!scene->findComponent<SplineComponent>(animate.spline) || !scene->isEntityActiveInHierarchy(entity)){
            continue;
        }
        if (!animate.awoken){
            animate.awoken = true;
            animate.elapsed = 0.0f;
            animate.completed = false;
            animate.playing = animate.playOnAwake;
            setFollowerDistance(entity, animate.startOffset * Spline(scene, animate.spline).getLength());
            continue;
        }
        if (!animate.playing){
            continue;
        }
        const Spline spline(scene, animate.spline);
        const float length = spline.getLength();
        if (length <= 0.0f){
            continue;
        }
        animate.elapsed += static_cast<float>(dt);
        const float total = animate.method == SplineAnimateMethod::Duration ? std::max(animate.duration, 1.0e-4f)
                                                                            : length / std::max(animate.speed, 1.0e-4f);
        const float raw = animate.elapsed / total + animate.startOffset;
        float t = 0.0f;
        bool completedNow = false;
        if (animate.loop == SplineLoop::Once){
            t = std::min(raw, 1.0f);
            animate.normalizedTime = t;
            t = easeValue(animate.easing, t);
            completedNow = raw >= 1.0f;
        }else if (animate.loop == SplineLoop::Loop){
            t = raw - std::floor(raw);
            animate.normalizedTime = t;
        }else{
            const float p = raw - 2.0f * std::floor(raw * 0.5f);
            t = p <= 1.0f ? p : 2.0f - p;
            animate.normalizedTime = t;
        }
        setFollowerDistance(entity, t * length);
        if (completedNow){
            animate.playing = false;
            animate.completed = true;
            animate.onComplete.call();
            followers = scene->getComponentArray<SplineAnimateComponent>();
        }
    }
}

void SplineSystem::setFollowerDistance(Entity follower, float distance){
    SplineAnimateComponent* animate = scene->findComponent<SplineAnimateComponent>(follower);
    Transform* transform = scene->findComponent<Transform>(follower);
    if (!animate || !transform || !scene->findComponent<SplineComponent>(animate->spline)){
        return;
    }
    const Spline spline(scene, animate->spline);
    animate->distance = distance;
    const spline::SplineFrame frame = spline.evaluateAtDistance(distance);
    const bool flat = !spline.is3D();
    const Vector3 position = frame.position + frame.right * animate->lateralOffset + frame.up * animate->verticalOffset;

    Quaternion rotation = transform->worldRotation;
    switch (animate->alignment){
        case SplineAlignment::None:
            break;
        case SplineAlignment::World:
            rotation = Quaternion();
            break;
        case SplineAlignment::SplineTangent:
        case SplineAlignment::SplineTangentAndUp:{
            if (flat){
                // 2D: about Z, local +X along travel
                const Vector3 x = Vector3(frame.forward.x, frame.forward.y, 0.0f).normalized();
                rotation = Quaternion(x, Vector3(0.0f, 0.0f, 1.0f).crossProduct(x), Vector3(0.0f, 0.0f, 1.0f));
            }else{
                // local +Z along travel, +Y up (the layouts' convention)
                Vector3 up = animate->alignment == SplineAlignment::SplineTangentAndUp ? frame.up : Vector3(0.0f, 1.0f, 0.0f);
                Vector3 x = up.crossProduct(frame.forward);
                if (x.squaredLength() < 1.0e-8f){
                    x = frame.right * -1.0f;
                }
                x.normalize();
                up = frame.forward.crossProduct(x).normalized();
                rotation = Quaternion(x, up, frame.forward);
            }
            break;
        }
    }

    // world -> the parent's space
    Vector3 localPosition = position;
    Quaternion localRotation = rotation;
    if (transform->parent != NULL_ENTITY){
        if (const Transform* parent = scene->findComponent<Transform>(transform->parent)){
            localPosition = parent->modelMatrix.inverse() * position;
            localRotation = parent->worldRotation.inverse() * rotation;
        }
    }
    if (transform->position != localPosition || transform->rotation != localRotation){
        transform->position = localPosition;
        transform->rotation = localRotation;
        transform->needUpdate = true;
    }
}

void SplineSystem::play(Entity follower){
    if (SplineAnimateComponent* animate = scene->findComponent<SplineAnimateComponent>(follower)){
        if (animate->completed){
            animate->elapsed = 0.0f;
            animate->completed = false;
        }
        animate->awoken = true;
        animate->playing = true;
    }
}

void SplineSystem::pause(Entity follower){
    if (SplineAnimateComponent* animate = scene->findComponent<SplineAnimateComponent>(follower)){
        animate->playing = false;
    }
}

void SplineSystem::restart(Entity follower){
    if (SplineAnimateComponent* animate = scene->findComponent<SplineAnimateComponent>(follower)){
        animate->elapsed = 0.0f;
        animate->completed = false;
        animate->normalizedTime = animate->startOffset;
        animate->awoken = true;
        animate->playing = true;
        if (scene->findComponent<SplineComponent>(animate->spline)){
            setFollowerDistance(follower, animate->startOffset * Spline(scene, animate->spline).getLength());
        }
    }
}

// ---- queries

std::vector<Entity> SplineSystem::getGeneratedEntities(Entity owner) const{
    std::vector<Entity> entities;
    auto it = resolves.find(owner);
    if (it == resolves.end()){
        return entities;
    }
    const Resolve& resolve = *it->second;
    for (const ExtrudeChunk& chunk : resolve.extrudeChunks) entities.push_back(chunk.entity);
    for (const PieceChunk& chunk : resolve.pieceChunks) entities.push_back(chunk.entity);
    for (const PoolModel& pool : resolve.pools){
        if (pool.probe != NULL_ENTITY) entities.push_back(pool.probe);
    }
    for (Entity root : resolve.prefabRoots) entities.push_back(root);
    for (Entity body : resolve.chainBodies) entities.push_back(body);
    return entities;
}

Entity SplineSystem::getGeneratedOwner(Entity generated) const{
    auto it = generatedOwners.find(generated);
    return it == generatedOwners.end() ? NULL_ENTITY : it->second;
}

bool SplineSystem::isGenerated(Entity entity) const{
    return generatedOwners.count(entity) != 0;
}

AABB SplineSystem::getGeneratedBounds(Entity owner) const{
    AABB bounds;
    auto it = resolves.find(owner);
    if (it == resolves.end()){
        return bounds;
    }
    for (const ExtrudeChunk& chunk : it->second->extrudeChunks){
        bounds.merge(chunk.worldBounds);
    }
    for (const PieceChunk& chunk : it->second->pieceChunks){
        if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(chunk.entity)){
            if (!mesh->worldAABB.isNull() && !mesh->worldAABB.isInfinite()){
                bounds.merge(mesh->worldAABB);
            }
        }
    }
    return bounds;
}

std::vector<SplineWarning> SplineSystem::getExtrudeWarnings(Entity owner) const{
    auto it = resolves.find(owner);
    return it == resolves.end() ? std::vector<SplineWarning>() : it->second->extrudeWarnings;
}

std::vector<SplineWarning> SplineSystem::getInstantiateWarnings(Entity owner) const{
    auto it = resolves.find(owner);
    return it == resolves.end() ? std::vector<SplineWarning>() : it->second->instantiateWarnings;
}

SplineBuildStats SplineSystem::getExtrudeStats(Entity owner) const{
    auto it = resolves.find(owner);
    if (it == resolves.end()){
        return SplineBuildStats();
    }
    SplineBuildStats stats = it->second->extrudeStats;
    stats.pending = it->second->extrudeDirty || it->second->extrudeJob != nullptr;
    return stats;
}

SplineBuildStats SplineSystem::getInstantiateStats(Entity owner) const{
    auto it = resolves.find(owner);
    if (it == resolves.end()){
        return SplineBuildStats();
    }
    SplineBuildStats stats = it->second->instantiateStats;
    stats.pending = it->second->instantiateDirty || it->second->piecesJob != nullptr;
    return stats;
}

std::vector<SplinePiecePlacement> SplineSystem::getPiecePlacements(Entity owner) const{
    auto it = resolves.find(owner);
    return it == resolves.end() ? std::vector<SplinePiecePlacement>() : it->second->placements;
}

bool SplineSystem::isRebuildPending(Entity owner) const{
    auto it = resolves.find(owner);
    if (it == resolves.end()){
        return false;
    }
    const Resolve& r = *it->second;
    return r.extrudeDirty || r.instantiateDirty || r.extrudeJob || r.piecesJob;
}

void SplineSystem::rebuildNow(Entity owner){
    SplineComponent* spline = scene->findComponent<SplineComponent>(owner);
    Transform* transform = scene->findComponent<Transform>(owner);
    if (!spline || !transform){
        return;
    }
    const bool interactive = interactiveEdit;
    const bool live = liveBuilds;
    interactiveEdit = false;
    liveBuilds = true;
    clockMs = nowMs();
    updateConsumers(owner, *spline, *transform);
    auto it = resolves.find(owner);
    if (it != resolves.end()){
        Resolve& resolve = *it->second;
        // background chunks: wait for them (a wait runs the work inline where nobody started it)
        if (resolve.extrudeJob){
            for (jobs::Task<void>& task : resolve.extrudeJob->tasks){
                task.wait();
            }
            finishExtrude(owner, resolve);
        }
        if (resolve.piecesJob){
            std::shared_ptr<BuildJob> job = std::move(resolve.piecesJob);
            for (jobs::Task<void>& task : job->tasks){
                task.wait();
            }
            if (job->generation == resolve.piecesGeneration){
                applyPieces(owner, resolve, *job);
            }
        }
    }
    interactiveEdit = interactive;
    liveBuilds = live;
}

SplineWaterSample SplineSystem::sampleWaterSurface(float x, float z, Entity owner) const{
    SplineWaterSample best;
    for (const auto& item : resolves){
        if (owner != NULL_ENTITY && item.first != owner){
            continue;
        }
        const Resolve& resolve = *item.second;
        if (resolve.waterStations.size() < 2){
            continue;
        }
        const Vector3& lo = resolve.waterBounds.getMinimum();
        const Vector3& hi = resolve.waterBounds.getMaximum();
        if (x < lo.x || x > hi.x || z < lo.z || z > hi.z){
            continue;
        }
        const spline::SplineStationHitXZ hit = resolve.waterGrid.closest(x, z);
        if (!hit.valid){
            continue;
        }
        const float reach = resolve.waterHalfWidth > 0.0f ? resolve.waterHalfWidth
                                                         : (hit.lateral < 0.0f ? hit.halfWidthLeft : hit.halfWidthRight);
        if (hit.distanceXZ > reach){
            continue;
        }
        if (!best.hit || hit.height > best.height){
            const spline::SplineStation& a = resolve.waterStations[hit.segment];
            const spline::SplineStation& b = resolve.waterStations[std::min<size_t>(hit.segment + 1, resolve.waterStations.size() - 1)];
            Vector3 forward = a.forward * (1.0f - hit.fraction) + b.forward * hit.fraction;
            forward.y = 0.0f;
            if (forward.squaredLength() > 1.0e-8f){
                forward.normalize();
            }
            best.hit = true;
            best.height = hit.height;
            best.normal = Vector3(0.0f, 1.0f, 0.0f);
            best.flow = forward * resolve.waterSpeed;
            best.owner = item.first;
        }
    }
    return best;
}

bool SplineSystem::hasWaterSurface(Entity owner) const{
    auto it = resolves.find(owner);
    return it != resolves.end() && it->second->waterStations.size() >= 2;
}

bool SplineSystem::hasWaterSurfaces() const{
    for (const auto& item : resolves){
        if (item.second->waterStations.size() >= 2) return true;
    }
    return false;
}

void SplineSystem::setInteractiveEdit(bool interactive){
    interactiveEdit = interactive;
}

bool SplineSystem::isInteractiveEdit() const{
    return interactiveEdit;
}

void SplineSystem::setLiveBuilds(bool live){
    liveBuilds = live;
}

void SplineSystem::setPaused(bool value){
    if (value != paused){
        for (auto& item : resolves){
            for (const PoolModel& pool : item.second->pools){
                if (pool.prefab){
                    item.second->instantiateDirty = true;
                    break;
                }
            }
        }
    }
    SubSystem::setPaused(value);
}

