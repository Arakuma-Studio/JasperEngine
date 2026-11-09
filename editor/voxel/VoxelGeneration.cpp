// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "voxel/VoxelGeneration.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "io/Data.h"
#include "object/TerrainEdit.h"
#include "subsystem/VoxelSystem.h"
#include "util/MeshTriangles.h"
#include "voxel/TransvoxelMesher.h"
#include "voxel/VoxelGeneratorNoise.h"
#include "voxel/VoxelSample.h"
#include "voxel/VoxelVoxelize.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <random>

using namespace jasperengine;
using namespace jasperengine::voxel;
using namespace jasperengine::editor;

namespace {

    // R37's overhangs preset, in T2's format: distance to a height, plus domain-warped 3D noise, so a
    // height band grows cliffs and arches. @HEIGHT@, @AMPLITUDE@ and @FREQUENCY@ are the volume's.
    const char* const OVERHANGS_GRAPH = R"({
  "formatVersion": 1, "kind": "noise", "kindVersion": 1, "nextId": 20,
  "nodes": [
    {"id": "n1", "type": "position", "position": [0.0, 0.0], "params": {}},
    {"id": "n2", "type": "scale_offset", "position": [240.0, 0.0], "params": {"scale": 1.0, "offset": @NEGHEIGHT@}},
    {"id": "n3", "type": "domain_warp", "position": [240.0, 160.0], "params": {"basis": "perlin", "dimensions": "3d", "frequency": @FREQUENCY@, "amplitude": @WARP@, "octaves": 3, "seed_offset": 11}},
    {"id": "n4", "type": "fbm", "position": [480.0, 160.0], "params": {"basis": "perlin", "dimensions": "3d", "frequency": @FREQUENCY@, "octaves": 3, "lacunarity": 2.0, "gain": 0.5, "seed_offset": 23}},
    {"id": "n5", "type": "scale_offset", "position": [720.0, 160.0], "params": {"scale": @AMPLITUDE@, "offset": 0.0}},
    {"id": "n6", "type": "math", "position": [960.0, 0.0], "params": {"op": "add"}},
    {"id": "n7", "type": "output_value", "position": [1200.0, 0.0], "params": {"name": "sdf"}}
  ],
  "edges": [
    {"id": "e8", "from": {"node": "n1", "port": "y"}, "to": {"node": "n2", "port": "in"}},
    {"id": "e9", "from": {"node": "n1", "port": "out"}, "to": {"node": "n3", "port": "coord"}},
    {"id": "e10", "from": {"node": "n3", "port": "out"}, "to": {"node": "n4", "port": "coord"}},
    {"id": "e11", "from": {"node": "n4", "port": "out"}, "to": {"node": "n5", "port": "in"}},
    {"id": "e12", "from": {"node": "n2", "port": "out"}, "to": {"node": "n6", "port": "a"}},
    {"id": "e13", "from": {"node": "n5", "port": "out"}, "to": {"node": "n6", "port": "b"}},
    {"id": "e14", "from": {"node": "n6", "port": "out"}, "to": {"node": "n7", "port": "in"}}
  ],
  "variables": [], "groups": [], "viewport": {"offset": [0.0, 0.0], "zoom": 1.0},
  "metadata": {"noise": {"space": "world", "dimensions": "3d"}}
})";

    void substitute(std::string& text, const std::string& from, const std::string& to) {
        size_t at = 0;
        while ((at = text.find(from, at)) != std::string::npos) {
            text.replace(at, from.size(), to);
            at += to.size();
        }
    }

    bool readProjectFile(const std::string& path, std::string& out) {
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) return false;
        out.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        return true;
    }

    // R37's worm tunnels, at least 2.5 voxels in radius, wandering under a height. Deterministic by seed:
    // mt19937_64's bits are specified, and no std:: distribution decides the shape.
    std::vector<CarveCapsule> wormTunnels(uint64_t seed, const Int3& dim, double sizeScale) {
        std::vector<CarveCapsule> capsules;
        std::mt19937_64 random(seed * 0x9E3779B97F4A7C15ull + 1);
        const auto unit = [&random] { return static_cast<double>(random() >> 12) / 4503599627370496.0; };
        const int worms = std::max(2, static_cast<int>(std::floor(static_cast<double>(dim.x * dim.z) / (64.0 * 64.0) * 3.0 * sizeScale)));
        const double ceiling = dim.y * 0.45;
        for (int w = 0; w < worms; w++) {
            Vec3d at{unit() * dim.x, 8.0 + unit() * std::max(1.0, ceiling - 8.0), unit() * dim.z};
            double heading = unit() * 6.283185307179586;
            const double radius = 2.5 + unit() * 1.5 * sizeScale;
            const int segments = 6 + static_cast<int>(unit() * 10.0);
            for (int s = 0; s < segments; s++) {
                heading += (unit() - 0.5) * 1.4;
                // a rational point on the circle: no libm decides a block (R28), and the heading only wanders
                const double t = heading * 0.5 - 1.5707963267948966;
                const double cx = (1.0 - t * t) / (1.0 + t * t), cz = 2.0 * t / (1.0 + t * t);
                const double step = 6.0 + unit() * 8.0;
                Vec3d next{at.x + cx * step, std::clamp(at.y + (unit() - 0.5) * 4.0, 6.0, ceiling), at.z + cz * step};
                capsules.push_back(CarveCapsule{at, next, radius});
                at = next;
            }
        }
        return capsules;
    }

    // R37's cave_entrance: by construction the chamber connects to the sky (AC30)
    std::vector<CarveCapsule> entranceChain(uint64_t seed, const Int3& dim, double surfaceVoxels, double sizeScale) {
        std::mt19937_64 random(seed * 0x9E3779B97F4A7C15ull + 2);
        const auto unit = [&random] { return static_cast<double>(random() >> 12) / 4503599627370496.0; };
        std::vector<CarveCapsule> capsules;
        const double radius = 3.5 * sizeScale;
        Vec3d at{dim.x * (0.35 + unit() * 0.3), surfaceVoxels + radius, dim.z * (0.35 + unit() * 0.3)};
        const double chamberY = std::max(radius + 6.0, surfaceVoxels - 18.0 * sizeScale);
        const int segments = 4;
        for (int s = 1; s <= segments; s++) {
            const double f = static_cast<double>(s) / segments;
            Vec3d next{at.x + (unit() - 0.5) * 10.0 * sizeScale, surfaceVoxels + radius + (chamberY - surfaceVoxels - radius) * f, at.z + (unit() - 0.5) * 10.0 * sizeScale};
            capsules.push_back(CarveCapsule{at, next, radius});
            at = next;
        }
        capsules.push_back(CarveCapsule{at, Vec3d{at.x + 1.0, at.y, at.z}, radius * 2.2});
        return capsules;
    }

    uint64_t hashString(const std::string& text) {
        return fnv1a(text.data(), text.size());
    }

    // V10: the terrain's height over everything, which is what the base reads
    uint64_t terrainHash(Scene* scene, Entity terrainEntity) {
        TerrainComponent* terrain = terrainEntity != NULL_ENTITY && scene->isEntityCreated(terrainEntity) ? scene->findComponent<TerrainComponent>(terrainEntity) : nullptr;
        if (!terrain) return 0;
        const terraincore::SurfaceView view = terrainSurfaceView(*terrain);
        if (!view.pixels) return 0;
        return fnv1a(view.pixels, static_cast<size_t>(view.width) * view.height * view.channels * view.bytesPerChannel);
    }

}

namespace {

    // A Noise Graph's base on the volume's lattice; the seed a generation records, plus one
    std::shared_ptr<const BaseSource> graphBase(const std::string& json, uint64_t seed, double voxelSize, std::string& error) {
        const std::shared_ptr<const noise::CompiledGraph> compiled = compileVoxelGraph(json, error);
        if (!compiled) return nullptr;
        NoiseBaseOptions options;
        options.graph = compiled;
        options.seed = static_cast<uint32_t>(seed) + 1;
        options.voxelSize = voxelSize;
        options.graphScale = 1.0;
        return makeNoiseBase(options);
    }

    // The presets made of ops and graphs, from what a generation records (R37): caves and overhangs
    std::shared_ptr<const BaseSource> presetBase(const std::string& preset, uint64_t seed, const VolumeDesc& desc, double sizeScale, std::string& error) {
        const double voxelSize = desc.voxelSize;
        const double height = desc.dimensions.y * 0.5 * voxelSize;
        if (preset == "caves") {
            return makeCarvedBase(makeSolidGroundBase(height, voxelSize, 0), wormTunnels(seed, desc.dimensions, sizeScale));
        }
        if (preset == "overhangs") {
            std::string json = OVERHANGS_GRAPH;
            const double amplitude = 10.0 * voxelSize * sizeScale;
            substitute(json, "@NEGHEIGHT@", std::to_string(-height));
            substitute(json, "@AMPLITUDE@", std::to_string(amplitude));
            substitute(json, "@WARP@", std::to_string(amplitude * 0.5));
            substitute(json, "@FREQUENCY@", std::to_string(0.05 / (voxelSize * sizeScale)));
            return graphBase(json, seed, voxelSize, error);
        }
        return nullptr;
    }

}

std::shared_ptr<const BaseSource> jasperengine::editor::resolveVoxelBase(const GeneratorDesc& generator, const VolumeDesc& desc) {
    std::string error;
    // what no description rebuilds is in the file's blocks, every one of them (prepare's bake)
    if (generator.kind == "baked") return makeEmptyBase();
    if (generator.kind == "preset") return presetBase(generator.preset, generator.seed, desc, generator.graphScale, error);
    if (generator.kind == "noise_graph") {
        std::string json;
        if (generator.sourcePath.empty() || !readProjectFile(generator.sourcePath, json)) return nullptr;
        return graphBase(json, generator.seed, desc.voxelSize, error);
    }
    // a Terrain's is VoxelSystem's, once the volume is attached to it
    return nullptr;
}

struct VoxelGeneration::State {
    Entity owner = NULL_ENTITY;
    VoxelGenerateRequest request;
    std::shared_ptr<VoxelAsset> asset;
    std::shared_ptr<const BaseSource> base;
    GeneratorDesc generator;
    std::unique_ptr<Store> working;
    VoxelGenerateResult result;
    std::atomic<bool> cancelled{false};
    // the base cannot be rebuilt from the file's description: every block goes in the file
    bool bake = false;
};

bool jasperengine::editor::isVoxelBaseStale(Scene* scene, Entity owner) {
    std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
    const VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
    std::shared_ptr<VoxelAsset> asset = system ? system->getAsset(owner) : nullptr;
    if (!asset || !component || asset->generator.sourceHash == 0) return false;
    if (asset->generator.kind == "noise_graph") {
        std::string json;
        return !readProjectFile(asset->generator.sourcePath, json) || hashString(json) != asset->generator.sourceHash;
    }
    if (asset->generator.kind == "terrain") {
        return terrainHash(scene, component->terrain) != asset->generator.sourceHash;
    }
    return false;
}

const std::vector<VoxelPresetInfo>& jasperengine::editor::voxelPresets() {
    static const std::vector<VoxelPresetInfo> presets = {
        {"empty", "Empty", "Nothing: sculpt from air"},
        {"solid_ground", "Solid Ground", "Solid under half the volume's height"},
        {"caves", "Caves", "Worm tunnels through solid ground"},
        {"overhangs", "Overhangs", "Cliffs and arches in a height band"},
        {"cave_entrance", "Cave Entrance", "A Terrain base with a tunnel from the surface to a chamber"},
    };
    return presets;
}

std::shared_ptr<VoxelGeneration> VoxelGeneration::prepare(Scene* scene, Entity owner, const VoxelGenerateRequest& request, VoxelGenerateResult& result) {
    result = VoxelGenerateResult{};
    const auto fail = [&result](const char* code, std::string message) -> std::shared_ptr<VoxelGeneration> {
        result.ok = false;
        result.error = code;
        result.message = std::move(message);
        return nullptr;
    };
    std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
    VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
    if (!system || !component) return fail("not_loaded", "the GameObject has no Voxel Volume");
    std::shared_ptr<VoxelAsset> asset = system->getAsset(owner);
    if (!asset || !asset->store) return fail("not_loaded", "the volume's file did not load: fix it first");

    const VolumeDesc& desc = asset->store->desc();
    const double voxelSize = desc.voxelSize;
    GeneratorDesc generator;
    generator.seed = request.seed;
    generator.blendWidth = request.blendWidth;
    std::shared_ptr<const BaseSource> base;
    bool nonWatertight = false;

    const auto noiseBase = [&](const std::string& json, std::string& error) { return graphBase(json, request.seed, voxelSize, error); };
    const auto terrainBase = [&](Entity terrain) { return system->makeTerrainBaseFor(owner, terrain); };
    const Entity terrain = request.terrain != NULL_ENTITY ? request.terrain : component->terrain;

    if (request.source == "noise_graph") {
        std::string json;
        if (request.graphPath.empty() || !readProjectFile(request.graphPath, json)) return fail("missing_asset", "no Noise Graph at '" + request.graphPath + "': pass graph, a .noisegraph path");
        std::string error;
        base = noiseBase(json, error);
        if (!base) return fail("invalid_graph", error);
        generator.kind = "noise_graph";
        generator.sourcePath = request.graphPath;
        generator.sourceHash = hashString(json);
    } else if (request.source == "terrain") {
        base = terrainBase(terrain);
        if (!base) return fail("missing_asset", "no Terrain to read: pass terrain, or attach one with terrain on the component");
        generator.kind = "terrain";
        generator.sourceHash = terrainHash(scene, terrain);
    } else if (request.source == "mesh") {
        std::vector<Vector3> vertices;
        std::vector<uint32_t> indices;
        if (request.mesh != NULL_ENTITY) {
            MeshComponent* mesh = scene->findComponent<MeshComponent>(request.mesh);
            if (!mesh) return fail("missing_asset", "that GameObject has no Mesh");
            MeshTriangles::appendMeshTriangles(mesh, Matrix4(), vertices, indices);
        }
        if (indices.empty()) return fail("missing_asset", "the mesh has no triangles: pass mesh, a GameObject with a loaded Mesh");
        std::vector<float> positions;
        positions.reserve(vertices.size() * 3);
        for (const Vector3& v : vertices) {
            positions.push_back(v.x);
            positions.push_back(v.y);
            positions.push_back(v.z);
        }
        VoxelizeOptions options;
        options.voxelSize = voxelSize;
        options.transform = request.meshTransform;
        const std::shared_ptr<StampField> field = voxelizeMesh(positions, indices, options);
        // A base's field is on the volume's lattice: its offset is where the mesh's origin went
        field->originOffset[0] = field->originOffset[1] = field->originOffset[2] = 0.0;
        nonWatertight = field->nonWatertight;
        base = makeFieldBase(field, voxelSize, 0);
        generator.kind = "mesh";
        generator.sourceHash = fnv1a(positions.data(), positions.size() * sizeof(float));
    } else if (request.source == "preset") {
        generator.kind = "preset";
        generator.preset = request.preset;
        const double height = desc.dimensions.y * 0.5 * voxelSize;
        if (request.preset == "empty") {
            generator.kind = "empty";
            base = makeEmptyBase();
        } else if (request.preset == "solid_ground") {
            generator.kind = "solid_ground";
            generator.height = height;
            base = makeSolidGroundBase(height, voxelSize, 0);
        } else if (request.preset == "caves" || request.preset == "overhangs") {
            std::string error;
            base = presetBase(request.preset, request.seed, desc, request.sizeScale, error);
            if (!base) return fail("invalid_graph", error);
            // what rebuilds it when the file is opened again (resolveVoxelBase)
            generator.graphScale = request.sizeScale;
        } else if (request.preset == "cave_entrance") {
            const std::shared_ptr<const BaseSource> ground = terrainBase(terrain);
            if (!ground) return fail("missing_asset", "cave_entrance digs from a Terrain: pass terrain");
            // Where the terrain's surface is at the footprint's centre, in voxels
            int16_t probe[BLOCK_SAMPLES];
            uint16_t probeMaterial[BLOCK_SAMPLES];
            double surface = desc.dimensions.y * 0.5;
            for (int by = 0; by * BLOCK_SIZE < desc.dimensions.y; by++) {
                ground->evaluate(Int3{(desc.dimensions.x / 2) & ~15, by * BLOCK_SIZE, (desc.dimensions.z / 2) & ~15}, 1, probe, probeMaterial);
                bool found = false;
                for (int y = 0; y < BLOCK_SIZE; y++) {
                    if (probe[sampleIndex(0, y, 0)] >= 0) {
                        surface = by * BLOCK_SIZE + y;
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            base = makeCarvedBase(ground, entranceChain(request.seed, desc.dimensions, surface, request.sizeScale));
        } else {
            return fail("invalid_source", "no preset named '" + request.preset + "': use caves, overhangs, cave_entrance, empty or solid_ground");
        }
    } else {
        return fail("invalid_source", "source is one of noise_graph, terrain, mesh, preset");
    }

    // R35: with a blend, another base fades to exact terrain before the box ends
    if (request.blendWidth > 0.0 && request.source != "terrain") {
        const std::shared_ptr<const BaseSource> ground = terrainBase(terrain);
        if (ground) {
            generator.otherKind = generator.kind;
            generator.otherPath = generator.sourcePath;
            base = makeBlendedBase(base, ground, desc.dimensions, request.blendWidth / voxelSize);
        }
    }

    auto generation = std::make_shared<VoxelGeneration>();
    generation->state = std::make_shared<State>();
    State& state = *generation->state;
    state.owner = owner;
    state.request = request;
    state.asset = asset;
    state.base = base;
    state.generator = generator;
    state.working = asset->store->clone();
    state.working->replaceBase(base);
    if (request.keepEdits) {
        state.working->resetUnmodifiedToBase();
    } else {
        state.working->resetToBase();
    }
    // A GameObject's mesh, a Terrain's entrance, a blend, or a Terrain the volume is not attached to: no
    // description makes it again, so every block goes in the file over an empty base, and the generator
    // says so, keeping what it was made from
    state.bake = request.source == "mesh" || request.preset == "cave_entrance" || !generator.otherKind.empty() ||
                 (request.source == "terrain" && component->terrain != terrain);
    if (state.bake) {
        if (state.generator.otherKind.empty()) state.generator.otherKind = state.generator.kind;
        state.generator.kind = "baked";
    }
    state.result.ok = true;
    state.result.nonWatertight = nonWatertight;
    state.result.estimate = request.dryRun;
    state.result.baseChanged = asset->generator.kind != generator.kind || asset->generator.sourceHash != generator.sourceHash || asset->generator.seed != generator.seed;
    result = state.result;
    return generation;
}

void VoxelGeneration::run(const std::function<bool()>& cancelled, const std::function<void(float)>& progress) {
    State& data = *state;
    Store& store = *data.working;
    int64_t triangles = 0;
    int64_t nonUniform = 0;
    // A dry run looks at the coarsest level only, and says it is an estimate (R71)
    const int firstLevel = data.request.dryRun ? store.levels() - 1 : 0;
    const int lastLevel = store.levels() - 1;
    int64_t total = 0, done = 0;
    for (int level = firstLevel; level <= lastLevel; level++) {
        const Int3 count = store.blockCount(level);
        total += static_cast<int64_t>(count.x) * count.y * count.z;
    }
    for (int level = lastLevel; level >= firstLevel; level--) {
        const Int3 count = store.blockCount(level);
        const int span = BLOCK_SIZE << level;
        for (int z = 0; z < count.z; z++) {
            for (int y = 0; y < count.y; y++) {
                for (int x = 0; x < count.x; x++) {
                    if (cancelled && cancelled()) {
                        data.cancelled.store(true);
                        return;
                    }
                    done++;
                    if (data.request.hasRegion) {
                        const IntBox block{Int3{x * span, y * span, z * span}, Int3{(x + 1) * span - 1, (y + 1) * span - 1, (z + 1) * span - 1}};
                        if (!block.intersects(data.request.region)) continue;
                    }
                    const BlockKey key{level, x, y, z};
                    const BlockContent content = store.content(key);
                    if (!content.isFullyUniform()) nonUniform++;
                    if (data.bake && !data.request.dryRun) store.setLoadedSlot(key, content, true);
                    // Triangles from the coarsest level, which is cheap and scales by area
                    if (level == lastLevel) {
                        const auto view = store.readView(key, 0);
                        MeshRequest request;
                        request.key = key;
                        request.mode = MeshMode::CollisionOnly;
                        MeshOut mesh;
                        meshBlock(*view, request, mesh);
                        store.publish(view->takeResolved());
                        triangles += mesh.triangleCount;
                    }
                }
            }
        }
        if (progress && total > 0) progress(static_cast<float>(done) / static_cast<float>(total));
    }
    data.result.blocksNonUniform = nonUniform;
    data.result.memory = store.memory().hotBytes;
    data.result.trianglesEstimate = triangles << (2 * lastLevel);
    if (!data.request.dryRun) data.result.contentHash = contentHash(store);
}

VoxelGenerateResult VoxelGeneration::finish(Scene* scene, VoxelGenerateUndo* undo) {
    State& data = *state;
    VoxelGenerateResult out = data.result;
    if (data.cancelled.load()) {
        out.ok = false;
        out.error = "cancelled";
        out.message = "the generation was cancelled and nothing changed";
        return out;
    }
    if (data.request.dryRun) return out;
    Store& target = *data.asset->store;
    Store& source = *data.working;
    if (undo) {
        undo->oldGenerator = data.asset->generator;
        undo->oldBase = target.base();
    }
    for (int level = 0; level < target.levels(); level++) {
        const Int3 count = target.blockCount(level);
        for (int z = 0; z < count.z; z++) {
            for (int y = 0; y < count.y; y++) {
                for (int x = 0; x < count.x; x++) {
                    const BlockKey key{level, x, y, z};
                    if (undo) {
                        SlotChange change;
                        change.key = key;
                        change.before = target.slot(key);
                        change.after = source.slot(key);
                        undo->slots.push_back(std::move(change));
                    }
                    target.applySlot(key, source.slot(key));
                }
            }
        }
    }
    target.replaceBase(data.bake ? makeEmptyBase() : data.base);
    target.bumpVersion();
    data.asset->generator = data.generator;
    data.asset->baseResolved = true;
    data.asset->dirty = true;
    if (std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>()) {
        const Int3 dim = target.desc().dimensions;
        system->notifyChanged(&target, IntBox{Int3{0, 0, 0}, Int3{dim.x - 1, dim.y - 1, dim.z - 1}});
    }
    return out;
}

void VoxelGeneration::applyUndo(Scene* scene, Entity owner, const VoxelGenerateUndo& undo, bool undoing) {
    std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
    if (!system) return;
    std::shared_ptr<VoxelAsset> asset = system->getAsset(owner);
    if (!asset || !asset->store) return;
    Store& target = *asset->store;
    for (const SlotChange& change : undo.slots) target.applySlot(change.key, undoing ? change.before : change.after);
    if (undoing) {
        target.replaceBase(undo.oldBase);
        asset->generator = undo.oldGenerator;
    }
    target.bumpVersion();
    asset->dirty = true;
    const Int3 dim = target.desc().dimensions;
    system->notifyChanged(&target, IntBox{Int3{0, 0, 0}, Int3{dim.x - 1, dim.y - 1, dim.z - 1}});
}
