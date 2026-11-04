// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshLodPool.h"

#include "meshoptimizer.h"
#include "tiny_gltf.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <mutex>

using namespace jasperengine;

namespace{
    // what a unit of each attribute weighs against a unit of position error (meshoptimizer's
    // attribute metric): normals are unit vectors, UVs span the texture, bone weights sum to 1
    const float NORMAL_WEIGHT = 0.5f;
    const float UV_WEIGHT = 1.0f;
    const float SKIN_WEIGHT = 1.0f;
    // rounding between copies of a vertex, as a share of the mesh size
    const float WELD_TOLERANCE = 2e-6f;
    // a level must drop at least this share of the level before it
    const float LEVEL_SHRINK = 0.9f;

    std::atomic<uint64_t> buildCount{0};

    bool isSettled(const MeshLodEntry& entry){
        return entry.data || entry.task.isReady();
    }

    // meshoptimizer only joins bitwise equal positions, so seam copies off by rounding would crack
    std::vector<float> weldPositions(std::vector<float> positions){
        float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (size_t v = 0; v < positions.size(); v += 3){
            for (int a = 0; a < 3; a++){
                lo[a] = std::min(lo[a], positions[v + a]);
                hi[a] = std::max(hi[a], positions[v + a]);
            }
        }
        const float tolerance = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) * WELD_TOLERANCE;
        if (!std::isnormal(tolerance)){
            return positions;
        }

        auto cell = [&](float value, int axis){
            return static_cast<int64_t>(std::floor((value - lo[axis]) / (tolerance * 2.0f)));
        };
        auto key = [](int64_t x, int64_t y, int64_t z){
            return (x & 0x1FFFFF) | ((y & 0x1FFFFF) << 21) | ((z & 0x1FFFFF) << 42);
        };
        auto within = [tolerance](const float* a, const float* b){
            return std::fabs(a[0] - b[0]) <= tolerance && std::fabs(a[1] - b[1]) <= tolerance && std::fabs(a[2] - b[2]) <= tolerance;
        };

        // copies snap to the first vertex seen within the tolerance
        std::unordered_map<int64_t, size_t> grid;
        for (size_t v = 0; v < positions.size(); v += 3){
            float* p = &positions[v];
            const float* match = nullptr;
            for (int64_t x = cell(p[0] - tolerance, 0); x <= cell(p[0] + tolerance, 0); x++){
                for (int64_t y = cell(p[1] - tolerance, 1); y <= cell(p[1] + tolerance, 1); y++){
                    for (int64_t z = cell(p[2] - tolerance, 2); z <= cell(p[2] + tolerance, 2); z++){
                        auto it = grid.find(key(x, y, z));
                        if (it != grid.end() && within(p, &positions[it->second])){
                            match = &positions[it->second];
                        }
                    }
                }
            }
            if (match){
                std::copy_n(match, 3, p);
            }else{
                grid.emplace(key(cell(p[0], 0), cell(p[1], 1), cell(p[2], 2)), v);
            }
        }

        return positions;
    }

    struct Point{
        double x, y, z;
    };

    Point pointOf(const std::vector<float>& positions, uint32_t index){
        const float* p = &positions[(size_t)index * 3];
        return {p[0], p[1], p[2]};
    }

    Point sub(const Point& a, const Point& b){ return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    double dot(const Point& a, const Point& b){ return a.x * b.x + a.y * b.y + a.z * b.z; }

    // squared distance from p to the triangle abc (Ericson, Real-Time Collision Detection 5.1.5)
    double pointTriangleDistance2(const Point& p, const Point& a, const Point& b, const Point& c){
        const Point ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
        const double d1 = dot(ab, ap), d2 = dot(ac, ap);
        auto at = [&](const Point& q){ const Point d = sub(p, q); return dot(d, d); };
        if (d1 <= 0.0 && d2 <= 0.0) return at(a);
        const Point bp = sub(p, b);
        const double d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0.0 && d4 <= d3) return at(b);
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0){
            const double v = d1 / (d1 - d3);
            return at({a.x + ab.x * v, a.y + ab.y * v, a.z + ab.z * v});
        }
        const Point cp = sub(p, c);
        const double d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0.0 && d5 <= d6) return at(c);
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0){
            const double w = d2 / (d2 - d6);
            return at({a.x + ac.x * w, a.y + ac.y * w, a.z + ac.z * w});
        }
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0){
            const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            return at({b.x + (c.x - b.x) * w, b.y + (c.y - b.y) * w, b.z + (c.z - b.z) * w});
        }
        const double denom = 1.0 / (va + vb + vc);
        const double v = vb * denom, w = vc * denom;
        return at({a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w});
    }

    // The triangles of a surface binned in a uniform grid: the nearest one to a point, searched
    // in shells of cells around it until no unvisited cell can be nearer
    class TriangleGrid{
    public:
        TriangleGrid(const std::vector<float>& positions, const std::vector<uint32_t>& indices)
            : positions(positions), indices(indices){
            const size_t triangles = indices.size() / 3;
            for (int a = 0; a < 3; a++){
                lo[a] = DBL_MAX;
                hi[a] = -DBL_MAX;
            }
            for (uint32_t index : indices){
                const Point p = pointOf(positions, index);
                lo[0] = std::min(lo[0], p.x); hi[0] = std::max(hi[0], p.x);
                lo[1] = std::min(lo[1], p.y); hi[1] = std::max(hi[1], p.y);
                lo[2] = std::min(lo[2], p.z); hi[2] = std::max(hi[2], p.z);
            }
            const double extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
            const double perAxis = std::clamp(std::round(std::cbrt((double)std::max<size_t>(triangles, 1)) * 2.0), 1.0, 128.0);
            cell = (extent > 0.0) ? extent / perAxis : 1.0;
            for (int a = 0; a < 3; a++)
                dims[a] = std::max(1, (int)std::ceil((hi[a] - lo[a]) / cell));

            // counted, then filled: each triangle in every cell its bounds overlap
            const size_t cells = (size_t)dims[0] * dims[1] * dims[2];
            start.assign(cells + 1, 0);
            auto forCells = [&](size_t t, auto&& fn){
                int from[3], to[3];
                triangleCells(t, from, to);
                for (int z = from[2]; z <= to[2]; z++)
                    for (int y = from[1]; y <= to[1]; y++)
                        for (int x = from[0]; x <= to[0]; x++)
                            fn(cellIndex(x, y, z));
            };
            for (size_t t = 0; t < triangles; t++)
                forCells(t, [&](size_t c){ start[c + 1]++; });
            for (size_t c = 0; c < cells; c++)
                start[c + 1] += start[c];
            items.resize(start[cells]);
            std::vector<uint32_t> fill(start.begin(), start.end() - 1);
            for (size_t t = 0; t < triangles; t++)
                forCells(t, [&](size_t c){ items[fill[c]++] = (uint32_t)t; });
        }

        bool empty() const{
            return indices.size() < 3;
        }

        double distance(const Point& p) const{
            int center[3];
            const double coords[3] = {p.x, p.y, p.z};
            for (int a = 0; a < 3; a++)
                center[a] = std::clamp((int)std::floor((coords[a] - lo[a]) / cell), 0, dims[a] - 1);

            double best = DBL_MAX;
            for (int r = 0;; r++){
                for (int z = center[2] - r; z <= center[2] + r; z++){
                    if (z < 0 || z >= dims[2]) continue;
                    for (int y = center[1] - r; y <= center[1] + r; y++){
                        if (y < 0 || y >= dims[1]) continue;
                        for (int x = center[0] - r; x <= center[0] + r; x++){
                            if (x < 0 || x >= dims[0]) continue;
                            // the shell only
                            if (std::max({std::abs(x - center[0]), std::abs(y - center[1]), std::abs(z - center[2])}) != r) continue;
                            const size_t c = cellIndex(x, y, z);
                            for (uint32_t i = start[c]; i < start[c + 1]; i++){
                                const size_t t = items[i];
                                best = std::min(best, pointTriangleDistance2(p, pointOf(positions, indices[t * 3]),
                                    pointOf(positions, indices[t * 3 + 1]), pointOf(positions, indices[t * 3 + 2])));
                            }
                        }
                    }
                }
                // the nearest a cell outside the box of shells 0..r can be
                double bound = DBL_MAX;
                for (int a = 0; a < 3; a++){
                    if (center[a] + r + 1 < dims[a])
                        bound = std::min(bound, lo[a] + (center[a] + r + 1) * cell - coords[a]);
                    if (center[a] - r > 0)
                        bound = std::min(bound, coords[a] - (lo[a] + (center[a] - r) * cell));
                }
                if (bound == DBL_MAX)
                    break;
                bound = std::max(bound, 0.0);
                if (best <= bound * bound)
                    break;
            }
            return std::sqrt(best);
        }

    private:
        const std::vector<float>& positions;
        const std::vector<uint32_t>& indices;
        double lo[3];
        double hi[3];
        double cell = 1.0;
        int dims[3] = {1, 1, 1};
        std::vector<uint32_t> start;
        std::vector<uint32_t> items;

        size_t cellIndex(int x, int y, int z) const{
            return ((size_t)z * dims[1] + y) * dims[0] + x;
        }

        void triangleCells(size_t t, int from[3], int to[3]) const{
            const Point v[3] = {pointOf(positions, indices[t * 3]), pointOf(positions, indices[t * 3 + 1]), pointOf(positions, indices[t * 3 + 2])};
            for (int a = 0; a < 3; a++){
                double mn = DBL_MAX, mx = -DBL_MAX;
                for (const Point& q : v){
                    const double c = (a == 0) ? q.x : (a == 1) ? q.y : q.z;
                    mn = std::min(mn, c);
                    mx = std::max(mx, c);
                }
                from[a] = std::clamp((int)std::floor((mn - lo[a]) / cell), 0, dims[a] - 1);
                to[a] = std::clamp((int)std::floor((mx - lo[a]) / cell), 0, dims[a] - 1);
            }
        }
    };

    // the farthest the level's surface is from the source's (sampled at its centroids and edge
    // midpoints) and the source's from the level's (at its vertices and centroids)
    double deviation(const std::vector<float>& positions, const std::vector<uint32_t>& source, const TriangleGrid& sourceGrid,
                     const std::vector<uint32_t>& level){
        if (level.size() < 3 || sourceGrid.empty())
            return 0.0;
        double worst = 0.0;
        const TriangleGrid levelGrid(positions, level);
        std::vector<bool> seen(positions.size() / 3, false);
        for (size_t t = 0; t + 2 < source.size(); t += 3){
            const Point a = pointOf(positions, source[t]), b = pointOf(positions, source[t + 1]), c = pointOf(positions, source[t + 2]);
            for (int k = 0; k < 3; k++){
                const uint32_t v = source[t + k];
                if (!seen[v]){
                    seen[v] = true;
                    worst = std::max(worst, levelGrid.distance(pointOf(positions, v)));
                }
            }
            worst = std::max(worst, levelGrid.distance({(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0, (a.z + b.z + c.z) / 3.0}));
        }
        for (size_t t = 0; t + 2 < level.size(); t += 3){
            const Point a = pointOf(positions, level[t]), b = pointOf(positions, level[t + 1]), c = pointOf(positions, level[t + 2]);
            const Point samples[4] = {
                {(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0, (a.z + b.z + c.z) / 3.0},
                {(a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5},
                {(b.x + c.x) * 0.5, (b.y + c.y) * 0.5, (b.z + c.z) * 0.5},
                {(c.x + a.x) * 0.5, (c.y + a.y) * 0.5, (c.z + a.z) * 0.5}};
            for (const Point& s : samples)
                worst = std::max(worst, sourceGrid.distance(s));
        }
        return worst;
    }

    // the attribute seams a permissive pass keeps: vertices sharing a position with a vertex of
    // other texture coordinates (meshopt_SimplifyVertex_Protect)
    std::vector<unsigned char> protectedSeams(const std::vector<float>& positions, const std::vector<float>& uvs){
        const size_t vertexCount = positions.size() / 3;
        std::vector<unsigned char> locks(vertexCount, 0);
        if (uvs.size() != vertexCount * 2)
            return locks;
        std::vector<unsigned int> remap(vertexCount);
        meshopt_generatePositionRemap(remap.data(), positions.data(), vertexCount, sizeof(float) * 3);
        for (size_t v = 0; v < vertexCount; v++){
            const size_t r = remap[v];
            if (r != v && (uvs[v * 2] != uvs[r * 2] || uvs[v * 2 + 1] != uvs[r * 2 + 1])){
                locks[v] |= meshopt_SimplifyVertex_Protect;
                locks[r] |= meshopt_SimplifyVertex_Protect;
            }
        }
        return locks;
    }

    size_t readGltfIndex(const unsigned char* data, int componentType){
        switch (componentType){
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return *data;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:{ uint16_t v; std::memcpy(&v, data, 2); return v; }
            default:{ uint32_t v; std::memcpy(&v, data, 4); return v; }
        }
    }
}

MeshLodRecipe MeshLodRecipe::runtime(){
    return forReduction(MeshLodReduction::BALANCED, MAX_MESH_LODS - 1);
}

MeshLodRecipe MeshLodRecipe::forReduction(MeshLodReduction reduction, unsigned int levels){
    MeshLodRecipe recipe;
    recipe.levels = std::clamp(levels, 1u, (unsigned int)MAX_MESH_LODS - 1);
    switch (reduction){
        case MeshLodReduction::PRESERVE_DETAIL:
            recipe.ratio = 0.6f;
            recipe.targetError = 0.01f;
            break;
        case MeshLodReduction::BALANCED:
            recipe.ratio = 0.5f;
            recipe.targetError = 0.02f;
            break;
        case MeshLodReduction::SMALLEST:
            recipe.ratio = 0.35f;
            recipe.targetError = 0.05f;
            recipe.permissive = true;
            break;
    }
    return recipe;
}

lods_t& MeshLodPool::getMap(){
    static lods_t* map = new lods_t();
    return *map;
}

std::mutex& MeshLodPool::getMutex(){
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}

// FNV-1a over 8-byte words, cheap enough to run on every mesh load
uint64_t MeshLodPool::hash(uint64_t seed, const void* data, size_t size){
    uint64_t h = seed ? seed : 14695981039346656037ull;
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    size_t i = 0;
    for (; i + 8 <= size; i += 8){
        uint64_t word;
        std::memcpy(&word, bytes + i, 8);
        h = (h ^ word) * 1099511628211ull;
    }
    for (; i < size; i++){
        h = (h ^ bytes[i]) * 1099511628211ull;
    }
    return h ? h : 1;
}

std::shared_ptr<MeshLodData> MeshLodPool::get(uint64_t key){
    std::lock_guard<std::mutex> lock(getMutex());
    auto it = getMap().find(key);
    if (it == getMap().end() || !isSettled(it->second)){
        return nullptr;
    }
    MeshLodEntry& entry = it->second;
    if (!entry.data){
        // a build that failed or was cancelled settles to no levels: nothing waits for it forever
        entry.data = (entry.task.status() == jobs::TaskStatus::Completed && entry.task.get())
            ? entry.task.get() : std::make_shared<MeshLodData>();
        entry.task.reset();
    }
    return entry.data;
}

bool MeshLodPool::has(uint64_t key){
    std::lock_guard<std::mutex> lock(getMutex());
    return getMap().count(key) > 0;
}

bool MeshLodPool::wait(uint64_t key){
    jobs::Task<std::shared_ptr<MeshLodData>> task;
    {
        std::lock_guard<std::mutex> lock(getMutex());
        auto it = getMap().find(key);
        if (it == getMap().end())
            return false;
        task = it->second.task;
    }
    // outside the lock: the build itself never takes it, but get() on another thread may
    task.wait();
    return true;
}

void MeshLodPool::build(uint64_t key, MeshLodSource&& source){
    std::lock_guard<std::mutex> lock(getMutex());
    if (getMap().count(key) > 0){
        return;
    }

    // single-thread mode runs it from the frame's pump instead
    auto shared = std::make_shared<MeshLodSource>(std::move(source));
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.gate = jobs::GateClass::Heavy;
    options.name = "mesh-lod-simplify";
    options.tags = jobs::Tag::Asset;
    buildCount++;
    getMap()[key].task = jobs::submit([shared]() {
        return buildLevels(*shared, MeshLodRecipe::runtime());
    }, options);
}

void MeshLodPool::prefill(uint64_t key, std::shared_ptr<MeshLodData> data){
    std::lock_guard<std::mutex> lock(getMutex());
    MeshLodEntry& entry = getMap()[key];
    if (entry.task.isValid() && !entry.task.isReady()){
        return;
    }
    entry.task.reset();
    entry.data = data ? std::move(data) : std::make_shared<MeshLodData>();
}

uint64_t MeshLodPool::getBuildCount(){
    return buildCount.load();
}

std::vector<float> MeshLodPool::attributeWeights(bool normals, bool uvs, bool skinned){
    std::vector<float> weights;
    if (normals) weights.insert(weights.end(), 3, NORMAL_WEIGHT);
    if (uvs) weights.insert(weights.end(), 2, UV_WEIGHT);
    if (skinned) weights.insert(weights.end(), 4, SKIN_WEIGHT);
    return weights;
}

float MeshLodPool::measureDeviation(const std::vector<float>& positions, const std::vector<uint32_t>& source, const std::vector<uint32_t>& level){
    const TriangleGrid sourceGrid(positions, source);
    return (float)deviation(positions, source, sourceGrid, level);
}

std::shared_ptr<MeshLodData> MeshLodPool::buildLevels(const MeshLodSource& source, const MeshLodRecipe& recipe){
    auto data = std::make_shared<MeshLodData>();
    data->indexType = source.indexType;

    const size_t vertexCount = source.positions.size() / 3;
    const size_t indexCount = source.indices.size();
    if (vertexCount < 8 || indexCount < 12 * 3 || indexCount % 3 != 0){
        return data;
    }

    const size_t indexSize = (source.indexType == AttributeDataType::UNSIGNED_INT) ? 4 : 2;

    // only guides the collapses, levels still index the source vertices
    const std::vector<float> positions = weldPositions(source.positions);

    // the attributes present, each with its own weight (a mesh without normals keeps its UVs'
    // weight; a skinned mesh keeps its bone weights so collapses stay within a joint's influence)
    const bool normals = source.normals.size() == vertexCount * 3;
    const bool uvs = source.uvs.size() == vertexCount * 2;
    const bool skinned = source.skinWeights.size() == vertexCount * 4;
    const std::vector<float> weights = attributeWeights(normals, uvs, skinned);
    const size_t attributeCount = weights.size();
    std::vector<float> attributes;
    attributes.reserve(vertexCount * attributeCount);
    for (size_t v = 0; v < vertexCount; v++){
        if (normals) attributes.insert(attributes.end(), &source.normals[v * 3], &source.normals[v * 3] + 3);
        if (uvs) attributes.insert(attributes.end(), &source.uvs[v * 2], &source.uvs[v * 2] + 2);
        if (skinned) attributes.insert(attributes.end(), &source.skinWeights[v * 4], &source.skinWeights[v * 4] + 4);
    }

    // the error budget in mesh units (ErrorAbsolute); a level stops short of its triangle target
    // rather than deviate more
    const float scale = meshopt_simplifyScale(positions.data(), vertexCount, sizeof(float) * 3);
    const float targetError = std::max(recipe.targetError, 0.0f) * scale;
    // (not SimplifyErrorClamped: selection reads the geometric error, so the budget is what keeps
    // a flat mesh's texture coordinates and normals, whose position error is none)
    unsigned int options = meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPrune;
    if (skinned)
        options |= meshopt_SimplifyRegularize;     // even triangles deform better
    std::vector<unsigned char> seamLocks;

    auto simplify = [&](std::vector<uint32_t>& out, size_t target, unsigned int flags, const unsigned char* locks){
        float resultError = 0.0f;
        if (attributeCount > 0){
            return meshopt_simplifyWithAttributes(out.data(), source.indices.data(), indexCount,
                positions.data(), vertexCount, sizeof(float) * 3,
                attributes.data(), sizeof(float) * attributeCount, weights.data(), attributeCount,
                locks, target, targetError, flags, &resultError);
        }
        if (locks){
            return meshopt_simplifyWithAttributes(out.data(), source.indices.data(), indexCount,
                positions.data(), vertexCount, sizeof(float) * 3, nullptr, 0, nullptr, 0,
                locks, target, targetError, flags, &resultError);
        }
        return meshopt_simplify(out.data(), source.indices.data(), indexCount,
            positions.data(), vertexCount, sizeof(float) * 3, target, targetError, flags, &resultError);
    };

    const TriangleGrid sourceGrid(source.positions, source.indices);
    std::vector<uint32_t> level(indexCount);
    size_t previousCount = indexCount;
    float previousError = 0.0f;
    float share = 1.0f;
    const unsigned int levels = std::min<unsigned int>(std::max(recipe.levels, 1u), MAX_MESH_LODS - 1);

    for (unsigned int l = 1; l <= levels; l++){
        // every level is cut from the source, so its error is a deviation from the source
        share *= std::clamp(recipe.ratio, 0.05f, 0.95f);
        const size_t target = static_cast<size_t>(indexCount * share) / 3 * 3;
        unsigned int flags = options | (recipe.permissive ? (unsigned int)meshopt_SimplifyPermissive : 0u);
        if (recipe.permissive && seamLocks.empty())
            seamLocks = protectedSeams(positions, source.uvs);
        size_t count = simplify(level, target, flags, recipe.permissive ? seamLocks.data() : nullptr);

        // stuck on attribute seams (a faceted or UV-split mesh): collapse across them, keeping
        // the UV seams
        if (!recipe.permissive && count > previousCount * LEVEL_SHRINK){
            if (seamLocks.empty())
                seamLocks = protectedSeams(positions, source.uvs);
            std::vector<uint32_t> retry(indexCount);
            const size_t retryCount = simplify(retry, target, flags | meshopt_SimplifyPermissive, seamLocks.data());
            if (retryCount >= 3 && retryCount < count){
                level.swap(retry);
                count = retryCount;
            }
        }

        if (count < 3 || count > previousCount * LEVEL_SHRINK){
            break;
        }

        meshopt_optimizeVertexCache(level.data(), level.data(), count, vertexCount);

        const std::vector<uint32_t> levelIndices(level.begin(), level.begin() + (std::ptrdiff_t)count);
        // never below a finer level's: selection walks the levels until one exceeds the budget
        const float error = std::max(previousError, (float)deviation(source.positions, source.indices, sourceGrid, levelIndices));

        MeshLodLevel& lod = data->levels[l];
        lod.indexBase = static_cast<unsigned int>(data->indices.size() / indexSize);
        lod.indexCount = static_cast<unsigned int>(count);
        lod.error = error;

        const size_t offset = data->indices.size();
        data->indices.resize(offset + count * indexSize);
        if (indexSize == 4){
            std::memcpy(data->indices.data() + offset, level.data(), count * 4);
        }else{
            uint16_t* out = reinterpret_cast<uint16_t*>(data->indices.data() + offset);
            for (size_t i = 0; i < count; i++){
                out[i] = static_cast<uint16_t>(level[i]);
            }
        }

        data->numLevels = l + 1;
        previousCount = count;
        previousError = error;
    }

    return data;
}

std::shared_ptr<MeshLodData> MeshLodPool::readGltfLevels(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
                                                          AttributeDataType indexType, size_t vertexCount, int& recipe){
    recipe = 0;
    auto ext = primitive.extensions.find(MESH_LODS_GLTF_EXTENSION);
    if (ext == primitive.extensions.end() || !ext->second.IsObject())
        return nullptr;
    const tinygltf::Value& value = ext->second;
    if (value.Has("recipe") && value.Get("recipe").IsNumber())
        recipe = (int)value.Get("recipe").GetNumberAsInt();
    if (!value.Has("levels") || !value.Get("levels").IsArray())
        return nullptr;

    auto data = std::make_shared<MeshLodData>();
    data->indexType = indexType;
    data->baked = true;
    const size_t indexSize = (indexType == AttributeDataType::UNSIGNED_INT) ? 4 : 2;
    const tinygltf::Value& levels = value.Get("levels");
    float previousError = 0.0f;
    for (size_t l = 0; l < levels.ArrayLen() && data->numLevels < MAX_MESH_LODS; l++){
        const tinygltf::Value& entry = levels.Get((int)l);
        if (!entry.IsObject() || !entry.Has("indices") || !entry.Get("indices").IsNumber())
            return nullptr;
        const int accessorIndex = (int)entry.Get("indices").GetNumberAsInt();
        const float error = entry.Has("error") && entry.Get("error").IsNumber() ? (float)entry.Get("error").GetNumberAsDouble() : -1.0f;
        if (accessorIndex < 0 || accessorIndex >= (int)model.accessors.size() || !std::isfinite(error) || error < 0.0f)
            return nullptr;
        const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
        if (accessor.bufferView < 0 || accessor.bufferView >= (int)model.bufferViews.size() || accessor.type != TINYGLTF_TYPE_SCALAR ||
                accessor.count < 3 || accessor.count % 3 != 0)
            return nullptr;
        const int component = accessor.componentType;
        if (component != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && component != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                component != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
            return nullptr;
        const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
        if (view.buffer < 0 || view.buffer >= (int)model.buffers.size())
            return nullptr;
        const size_t elementSize = (size_t)tinygltf::GetComponentSizeInBytes((uint32_t)component);
        const size_t stride = view.byteStride ? view.byteStride : elementSize;
        const size_t begin = view.byteOffset + accessor.byteOffset;
        const std::vector<unsigned char>& bytes = model.buffers[view.buffer].data;
        if (begin + (accessor.count - 1) * stride + elementSize > bytes.size() || begin + (accessor.count - 1) * stride + elementSize > view.byteOffset + view.byteLength)
            return nullptr;

        MeshLodLevel& lod = data->levels[data->numLevels];
        lod.indexBase = static_cast<unsigned int>(data->indices.size() / indexSize);
        lod.indexCount = static_cast<unsigned int>(accessor.count);
        lod.error = std::max(previousError, error);
        previousError = lod.error;
        const size_t offset = data->indices.size();
        data->indices.resize(offset + accessor.count * indexSize);
        for (size_t i = 0; i < accessor.count; i++){
            const size_t index = readGltfIndex(bytes.data() + begin + i * stride, component);
            if (index >= vertexCount || (indexSize == 2 && index > 0xFFFF))
                return nullptr;
            if (indexSize == 4){
                const uint32_t v = (uint32_t)index;
                std::memcpy(data->indices.data() + offset + i * 4, &v, 4);
            }else{
                const uint16_t v = (uint16_t)index;
                std::memcpy(data->indices.data() + offset + i * 2, &v, 2);
            }
        }
        data->numLevels++;
    }
    return data;
}

void MeshLodPool::clear(){
    std::lock_guard<std::mutex> lock(getMutex());
    for (auto& it : getMap()){
        // a build not started yet is dropped, a running one finishes
        it.second.task.cancel();
        it.second.task.wait();
    }
    getMap().clear();
}

void MeshLodPool::clearUnused(){
    std::lock_guard<std::mutex> lock(getMutex());
    for (auto it = getMap().begin(); it != getMap().end();){
        MeshLodEntry& entry = it->second;
        if (!isSettled(entry)){
            ++it;
            continue;
        }
        const std::shared_ptr<MeshLodData>* held = entry.data ? &entry.data
            : (entry.task.status() == jobs::TaskStatus::Completed ? &entry.task.get() : nullptr);
        if (!held || !*held || held->use_count() <= 1){
            it = getMap().erase(it);
        }else{
            ++it;
        }
    }
}
