// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshLodPool.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <mutex>

using namespace jasperengine;

namespace{
    // normal xyz and uv deltas weighed against position error
    const float ATTRIBUTE_WEIGHTS[5] = {0.5f, 0.5f, 0.5f, 0.3f, 0.3f};
    // rounding between copies of a vertex, as a share of the mesh size
    const float WELD_TOLERANCE = 2e-6f;

    bool isBuilt(const jobs::Task<std::shared_ptr<MeshLodData>>& task){
        return task.isReady() && task.status() == jobs::TaskStatus::Completed;
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
}

MeshLodRecipe MeshLodRecipe::runtime(){
    // levels keeping 1/2, 1/4 and 1/8 of the source's triangles, as far as the error allows
    return MeshLodRecipe();
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
    if (it == getMap().end() || !isBuilt(it->second)){
        return nullptr;
    }
    return it->second.get();
}

bool MeshLodPool::has(uint64_t key){
    std::lock_guard<std::mutex> lock(getMutex());
    return getMap().count(key) > 0;
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
    getMap()[key] = jobs::submit([shared]() {
        return buildLevels(*shared, MeshLodRecipe::runtime());
    }, options);
}

// levels are appended while they keep shrinking; a result without any means the source
// could not be reduced and stays at numLevels 1
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

    std::vector<uint32_t> level(indexCount);
    size_t previousCount = indexCount;

    // the error budget in mesh units (the levels' errors are absolute)
    const float targetError = recipe.targetError >= FLT_MAX ? FLT_MAX
        : recipe.targetError * meshopt_simplifyScale(positions.data(), vertexCount, sizeof(float) * 3);
    const unsigned int options = meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPrune |
                                 (recipe.permissive ? (unsigned int)meshopt_SimplifyPermissive : 0u);
    const unsigned int levels = std::min<unsigned int>(recipe.levels, MAX_MESH_LODS - 1);
    float share = 1.0f;

    for (unsigned int l = 1; l <= levels; l++){
        // every level is cut from the source, so its error is a true deviation
        share *= recipe.ratio;
        const size_t target = static_cast<size_t>(indexCount * share) / 3 * 3;
        float error = 0.0f;
        size_t count;
        if (source.attributeCount > 0){
            count = meshopt_simplifyWithAttributes(level.data(), source.indices.data(), indexCount,
                positions.data(), vertexCount, sizeof(float) * 3,
                source.attributes.data(), sizeof(float) * source.attributeCount,
                ATTRIBUTE_WEIGHTS, source.attributeCount, nullptr, target, targetError, options, &error);
        }else{
            count = meshopt_simplify(level.data(), source.indices.data(), indexCount,
                positions.data(), vertexCount, sizeof(float) * 3, target, targetError, options, &error);
        }

        if (count < 3 || count > previousCount * 0.9f){
            break;
        }

        meshopt_optimizeVertexCache(level.data(), level.data(), count, vertexCount);

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
    }

    return data;
}

void MeshLodPool::clear(){
    std::lock_guard<std::mutex> lock(getMutex());
    for (auto& it : getMap()){
        // a build not started yet is dropped, a running one finishes
        it.second.cancel();
        it.second.wait();
    }
    getMap().clear();
}

void MeshLodPool::clearUnused(){
    std::lock_guard<std::mutex> lock(getMutex());
    for (auto it = getMap().begin(); it != getMap().end();){
        if (it->second.isReady() && (!isBuilt(it->second) || it->second.get().use_count() <= 1)){
            it = getMap().erase(it);
        }else{
            ++it;
        }
    }
}
