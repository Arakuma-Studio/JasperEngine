// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleMeshBuilder.h"

#include "texture/TexturePrepare.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;

namespace {

    struct WeldKey {
        TitleMeshVertex v;
        uint16_t wrap[2];

        bool operator==(const WeldKey& o) const { return std::memcmp(this, &o, sizeof(WeldKey)) == 0; }
    };
    static_assert(sizeof(WeldKey) == 28, "no padding in the weld key");

    struct WeldHash {
        size_t operator()(const WeldKey& k) const {
            // FNV-1a over the key's bytes
            uint64_t h = 0xcbf29ce484222325ull;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(&k);
            for (size_t i = 0; i < sizeof(WeldKey); i++) {
                h ^= p[i];
                h *= 0x100000001b3ull;
            }
            return (size_t)h;
        }
    };

}

std::shared_ptr<TitleMesh> jasperengine::titleart::three_d::buildTitleMeshFromSolids(const TitleSolids& solids, bool wrapStream, double size) {
    auto mesh = std::make_shared<TitleMesh>();
    mesh->spans = solids.spans;
    mesh->parts = solids.parts;
    mesh->rigs = solids.rigs;
    mesh->pivotZ = solids.pivotZ;
    mesh->slots = solids.slots;
    if (mesh->slots.size() < solids.mesh.slots.size()) mesh->slots.resize(solids.mesh.slots.size());
    const float inv = size > 0 ? (float)(1.0 / size) : 1.0f;

    std::unordered_map<WeldKey, uint32_t, WeldHash> weld;
    std::vector<WeldKey> unique;
    std::vector<uint32_t> indices;
    indices.reserve(solids.mesh.triangleCount() * 3);
    for (size_t slot = 0; slot < solids.mesh.slots.size(); slot++) {
        const std::vector<RawVertex>& soup = solids.mesh.slots[slot];
        TitleMeshSlot& range = mesh->slots[slot];
        range.firstIndex = (uint32_t)indices.size();
        const uint16_t tag = (uint16_t)((slot & 0x0FFF) | ((uint32_t)range.role << 12));
        for (size_t t = 0; t + 2 < soup.size(); t += 3) {
            // a triangle with two corners at one point covers nothing (a collapsed ring edge)
            auto same = [](const RawVertex& a, const RawVertex& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
            if (same(soup[t], soup[t + 1]) || same(soup[t + 1], soup[t + 2]) || same(soup[t], soup[t + 2])) continue;
            for (size_t c = 0; c < 3; c++) {
                const RawVertex& r = soup[t + c];
                WeldKey key;
                std::memset(&key, 0, sizeof(key));
                // -0 and 0 weld (the key compares bytes)
                key.v.position[0] = r.x == 0.0f ? 0.0f : r.x;
                key.v.position[1] = r.y == 0.0f ? 0.0f : r.y;
                key.v.position[2] = r.z == 0.0f ? 0.0f : r.z;
                const auto n = encodeTitleNormal(r.nx, r.ny, r.nz);
                key.v.normal[0] = n[0];
                key.v.normal[1] = n[1];
                key.v.ids[0] = r.span;
                key.v.ids[1] = r.span;
                key.v.ids[2] = r.part;
                key.v.ids[3] = tag;
                if (wrapStream) {
                    const bool cap = std::abs(r.nz) > 0.999f;
                    key.wrap[0] = floatToHalf(cap ? r.x * inv : r.arc * inv);
                    key.wrap[1] = floatToHalf(cap ? r.y * inv : -r.z * inv);
                }
                auto [it, inserted] = weld.emplace(key, (uint32_t)unique.size());
                if (inserted) unique.push_back(key);
                indices.push_back(it->second);
            }
        }
        range.indexCount = (uint32_t)indices.size() - range.firstIndex;
    }
    const size_t vertexCount = unique.size();
    std::vector<float> positions(vertexCount * 3);
    for (size_t i = 0; i < vertexCount; i++) std::memcpy(&positions[i * 3], unique[i].v.position, sizeof(float) * 3);

    if (!indices.empty()) {
        mesh->stats.acmrBefore = meshopt_analyzeVertexCache(indices.data(), indices.size(), vertexCount, 32, 0, 0).acmr;
        std::vector<uint32_t> scratch;
        for (const TitleMeshSlot& range : mesh->slots) {
            if (!range.indexCount) continue;
            uint32_t* sub = indices.data() + range.firstIndex;
            scratch.assign(sub, sub + range.indexCount);
            meshopt_optimizeVertexCache(sub, scratch.data(), range.indexCount, vertexCount);
            scratch.assign(sub, sub + range.indexCount);
            meshopt_optimizeOverdraw(sub, scratch.data(), range.indexCount, positions.data(), vertexCount, sizeof(float) * 3, 1.05f);
        }
        std::vector<uint32_t> remap(vertexCount);
        const size_t used = meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(), indices.size(), vertexCount);
        meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
        std::vector<WeldKey> fetched(used);
        meshopt_remapVertexBuffer(fetched.data(), unique.data(), vertexCount, sizeof(WeldKey), remap.data());
        unique = std::move(fetched);
        mesh->stats.acmrAfter = meshopt_analyzeVertexCache(indices.data(), indices.size(), unique.size(), 32, 0, 0).acmr;
    }

    mesh->vertices.resize(unique.size());
    if (wrapStream) mesh->wrap.resize(unique.size() * 2);
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (size_t i = 0; i < unique.size(); i++) {
        mesh->vertices[i] = unique[i].v;
        if (wrapStream) {
            mesh->wrap[i * 2] = unique[i].wrap[0];
            mesh->wrap[i * 2 + 1] = unique[i].wrap[1];
        }
        for (int c = 0; c < 3; c++) {
            lo[c] = std::min(lo[c], unique[i].v.position[c]);
            hi[c] = std::max(hi[c], unique[i].v.position[c]);
        }
    }
    if (!unique.empty()) {
        mesh->boundsMin = {lo[0], lo[1], lo[2]};
        mesh->boundsMax = {hi[0], hi[1], hi[2]};
    }
    mesh->wideIndices = unique.size() > 65535;
    if (mesh->wideIndices) {
        mesh->indices32 = std::move(indices);
    } else {
        mesh->indices16.assign(indices.begin(), indices.end());
    }
    mesh->stats.triangles = (uint32_t)(mesh->indexCount() / 3);
    mesh->stats.vertices = (uint32_t)mesh->vertices.size();
    mesh->stats.solids = solids.solids;
    mesh->stats.fallbackUsed = solids.fallbackUsed;
    return mesh;
}
