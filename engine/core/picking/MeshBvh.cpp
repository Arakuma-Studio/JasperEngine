// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's MeshBvh (Engine/Modules/MeshPicking/Source/MeshBvh.cpp): the
// binned-SAH binary build, its greedy collapse into 8-wide nodes, the 8-bit conservative
// quantization, the near-first traversal and the serialized layout. Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md).

#include "MeshBvh.h"
#include "jobs/JobSystem.h"
#include "MeshBvhSimd.h"
#include "BinnedSah.h"

#include "Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#if JE_PICK_HAS_AVX2
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

using namespace jasperengine;
using namespace jasperengine::picking;

namespace {

    constexpr float FLOAT_MAX = std::numeric_limits<float>::max();

    // the traversal's stack: a tree is at most MAX_SAH_DEPTH + log2(triangles) binary levels,
    // so an 8-wide path pushes far fewer than this
    constexpr int TRAVERSAL_STACK = 512;

    inline bool finite(const Vector3& v){
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    struct Box {
        float mn[3] = { FLOAT_MAX,  FLOAT_MAX,  FLOAT_MAX};
        float mx[3] = {-FLOAT_MAX, -FLOAT_MAX, -FLOAT_MAX};

        void expand(const Vector3& p){
            if (!finite(p)) return;
            mn[0] = p.x < mn[0] ? p.x : mn[0]; mx[0] = p.x > mx[0] ? p.x : mx[0];
            mn[1] = p.y < mn[1] ? p.y : mn[1]; mx[1] = p.y > mx[1] ? p.y : mx[1];
            mn[2] = p.z < mn[2] ? p.z : mn[2]; mx[2] = p.z > mx[2] ? p.z : mx[2];
        }
        void expand(const Box& b){
            for (int a = 0; a < 3; a++){
                mn[a] = b.mn[a] < mn[a] ? b.mn[a] : mn[a];
                mx[a] = b.mx[a] > mx[a] ? b.mx[a] : mx[a];
            }
        }
        void expand(const float* box){
            for (int a = 0; a < 3; a++){
                mn[a] = box[a] < mn[a] ? box[a] : mn[a];
                mx[a] = box[3 + a] > mx[a] ? box[3 + a] : mx[a];
            }
        }
        bool empty() const { return mn[0] > mx[0]; }
        float area() const { return boxSurfaceArea(mn, mx); }
    };

    // A leaf's box grows a little past its triangles, so a ray along one of its faces (an
    // axis-aligned ray of an orthographic view, exactly on a vertex line) is strictly inside:
    // the slab test of a zero direction component then never sees 0 times its huge reciprocal
    inline float margin(float x){
        const float relative = std::fabs(x) * (1.0f / 1048576.0f);
        return relative > 1e-20f ? relative : 1e-20f;
    }

    inline void widen(Box& box){
        if (box.empty()){
            return;
        }
        for (int a = 0; a < 3; a++){
            box.mn[a] -= margin(box.mn[a]);
            box.mx[a] += margin(box.mx[a]);
        }
    }

    inline float decode(float boxMin, uint8_t q, float scale){
        return boxMin + (float)q * scale;
    }

    // a per-axis step such that boxMin + 255 * scale reaches boxMax
    float nodeScale(float boxMin, float boxMax){
        if (!(boxMax > boxMin)){
            return 0.0f;
        }
        float scale = (boxMax - boxMin) / 255.0f;
        if (scale <= 0.0f){
            scale = std::numeric_limits<float>::denorm_min();
        }
        // boxMin + 255 * scale rounds (at boxMin's magnitude) below boxMax by at most a few
        // ulps of the larger of the two: grow by that much at once, then relatively
        if (decode(boxMin, 255, scale) < boxMax){
            const float magnitude = std::max(std::fabs(boxMin), std::fabs(boxMax));
            const float ulp = std::nextafter(magnitude, FLOAT_MAX) - magnitude;
            scale += 2.0f * ulp / 255.0f;
            for (int i = 0; i < 64 && decode(boxMin, 255, scale) < boxMax; i++){
                scale *= 1.0f + 1.0f / 65536.0f;
            }
        }
        if (decode(boxMin, 255, scale) < boxMax){
            scale = boxMax - boxMin; // degenerate tiny extent: one step covers it
        }
        return scale;
    }

    // conservative 8-bit bounds of [lo, hi] in a node: decode(qMin) <= lo, decode(qMax) >= hi
    void quantize(float boxMin, float scale, float lo, float hi, uint8_t& qMin, uint8_t& qMax){
        if (scale <= 0.0f){
            qMin = 0;
            qMax = 0;
            return;
        }
        const float fMin = std::floor((lo - boxMin) / scale);
        const float fMax = std::ceil((hi - boxMin) / scale);
        int a = fMin <= 0.0f ? 0 : (fMin >= 255.0f ? 255 : (int)fMin);
        int b = fMax <= 0.0f ? 0 : (fMax >= 255.0f ? 255 : (int)fMax);
        while (a > 0 && decode(boxMin, (uint8_t)a, scale) > lo) a--;
        while (b < 255 && decode(boxMin, (uint8_t)b, scale) < hi) b++;
        qMin = (uint8_t)a;
        qMax = (uint8_t)b;
    }

    Box triangleBox(const MeshBvh::Triangle& t){
        Box box;
        box.expand(t.v0);
        box.expand(t.v1);
        box.expand(t.v2);
        return box;
    }

    inline Vector3 centroid(const MeshBvh::Triangle& t){
        return Vector3((t.v0.x + t.v1.x + t.v2.x) * (1.0f / 3.0f),
                       (t.v0.y + t.v1.y + t.v2.y) * (1.0f / 3.0f),
                       (t.v0.z + t.v1.z + t.v2.z) * (1.0f / 3.0f));
    }

    // the binary intermediate of the build
    struct BinaryNode {
        Box box;
        uint32_t left = 0;
        uint32_t right = 0;
        uint32_t first = 0;
        uint32_t count = 0; // > 0: leaf
    };

    // directions with a zero component get a finite reciprocal: the slab maths never sees
    // an infinity times zero
    inline float safeInverse(float d){
        constexpr float EPS = 1e-30f;
        if (std::fabs(d) < EPS){
            d = d < 0.0f ? -EPS : EPS;
        }
        return 1.0f / d;
    }

    struct FaceTest {
        std::span<const PickFaces> faces;
        PickFaces uniform = PickFaces::BOTH;
        bool isUniform = true;

        FaceTest(std::span<const PickFaces> list): faces(list){
            if (!faces.empty()){
                uniform = faces[0];
                isUniform = std::all_of(faces.begin(), faces.end(), [&](PickFaces f){ return f == uniform; });
            }
        }

        static bool accepts(PickFaces mode, float det){
            return mode == PickFaces::BOTH || (mode == PickFaces::FRONT ? det > 0.0f : det < 0.0f);
        }
    };

    std::atomic<int> simdPath{-1};

    PickSimdPath detectSimdPath(){
#if JE_PICK_HAS_NEON
        PickSimdPath path = PickSimdPath::NEON;
#elif JE_PICK_HAS_AVX2
        PickSimdPath path = avx2Usable(queryCpuFeatures()) ? PickSimdPath::AVX2 : PickSimdPath::SCALAR;
#else
        PickSimdPath path = PickSimdPath::SCALAR;
#endif
        if (const char* forced = std::getenv("JASPER_PICK_SIMD")){
            const std::string name(forced);
            PickSimdPath wanted = path;
            if (name == "scalar") wanted = PickSimdPath::SCALAR;
            else if (name == "avx2") wanted = PickSimdPath::AVX2;
            else if (name == "neon") wanted = PickSimdPath::NEON;
            else Log::warn("JASPER_PICK_SIMD=%s is not scalar, avx2 or neon: picking uses %s", forced, pickSimdPathName(path));
            if (wanted != path){
                if (isPickSimdPathAvailable(wanted)){
                    path = wanted;
                }else{
                    Log::warn("JASPER_PICK_SIMD=%s is not available here: picking uses %s", forced, pickSimdPathName(path));
                }
            }
        }
        return path;
    }

}

// ---- SIMD dispatch

CpuFeatures picking::queryCpuFeatures(){
    CpuFeatures features;
#if JE_PICK_HAS_AVX2
#if defined(_MSC_VER) && !defined(__clang__)
    int info[4] = {0, 0, 0, 0};
    __cpuid(info, 0);
    const int maxLeaf = info[0];
    __cpuid(info, 1);
    features.osxsave = (info[2] & (1 << 27)) != 0;
    features.avx = (info[2] & (1 << 28)) != 0;
    if (features.osxsave){
        const unsigned long long xcr0 = _xgetbv(0);
        features.ymmEnabled = (xcr0 & 0x6) == 0x6;
    }
    if (maxLeaf >= 7){
        __cpuidex(info, 7, 0);
        features.avx2 = (info[1] & (1 << 5)) != 0;
    }
#else
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    const unsigned int maxLeaf = __get_cpuid_max(0, nullptr);
    if (maxLeaf >= 1 && __get_cpuid(1, &eax, &ebx, &ecx, &edx)){
        features.osxsave = (ecx & (1u << 27)) != 0;
        features.avx = (ecx & (1u << 28)) != 0;
    }
    if (features.osxsave){
        unsigned int xcr0Low = 0, xcr0High = 0;
        __asm__ volatile("xgetbv" : "=a"(xcr0Low), "=d"(xcr0High) : "c"(0));
        features.ymmEnabled = (xcr0Low & 0x6u) == 0x6u;
    }
    if (maxLeaf >= 7){
        __cpuid_count(7, 0, eax, ebx, ecx, edx);
        features.avx2 = (ebx & (1u << 5)) != 0;
    }
#endif
#endif
    return features;
}

bool picking::avx2Usable(const CpuFeatures& features){
    return features.osxsave && features.avx && features.ymmEnabled && features.avx2;
}

bool jasperengine::isPickSimdPathAvailable(PickSimdPath path){
    switch (path){
        case PickSimdPath::SCALAR: return true;
        case PickSimdPath::AVX2:
#if JE_PICK_HAS_AVX2
            return avx2Usable(queryCpuFeatures());
#else
            return false;
#endif
        case PickSimdPath::NEON:
            return JE_PICK_HAS_NEON != 0;
    }
    return false;
}

const char* jasperengine::pickSimdPathName(PickSimdPath path){
    switch (path){
        case PickSimdPath::SCALAR: return "scalar";
        case PickSimdPath::AVX2: return "avx2";
        case PickSimdPath::NEON: return "neon";
    }
    return "scalar";
}

PickSimdPath jasperengine::getPickSimdPath(){
    int path = simdPath.load(std::memory_order_acquire);
    if (path < 0){
        path = (int)detectSimdPath();
        int expected = -1;
        if (!simdPath.compare_exchange_strong(expected, path, std::memory_order_acq_rel)){
            path = expected;
        }
    }
    return (PickSimdPath)path;
}

bool jasperengine::setPickSimdPath(PickSimdPath path){
    if (!isPickSimdPathAvailable(path)){
        return false;
    }
    simdPath.store((int)path, std::memory_order_release);
    return true;
}

SlabTest8 picking::activeSlabTest(){
    switch (getPickSimdPath()){
#if JE_PICK_HAS_AVX2
        case PickSimdPath::AVX2: return &slabTest8Avx2;
#endif
#if JE_PICK_HAS_NEON
        case PickSimdPath::NEON: return &slabTest8Neon;
#endif
        default: return &slabTest8Scalar;
    }
}

uint32_t picking::slabTest8Scalar(const MeshBvh::Node& node, const SlabRay& ray, float tBest, float* tEnter){
    uint32_t bits = 0;
    for (int s = 0; s < 8; s++){
        const float loX = decode(node.boxMin[0], node.qMinX[s], node.scale[0]);
        const float loY = decode(node.boxMin[1], node.qMinY[s], node.scale[1]);
        const float loZ = decode(node.boxMin[2], node.qMinZ[s], node.scale[2]);
        const float hiX = decode(node.boxMin[0], node.qMaxX[s], node.scale[0]);
        const float hiY = decode(node.boxMin[1], node.qMaxY[s], node.scale[1]);
        const float hiZ = decode(node.boxMin[2], node.qMaxZ[s], node.scale[2]);

        const float t1x = (loX - ray.origin[0]) * ray.invDir[0];
        const float t2x = (hiX - ray.origin[0]) * ray.invDir[0];
        const float t1y = (loY - ray.origin[1]) * ray.invDir[1];
        const float t2y = (hiY - ray.origin[1]) * ray.invDir[1];
        const float t1z = (loZ - ray.origin[2]) * ray.invDir[2];
        const float t2z = (hiZ - ray.origin[2]) * ray.invDir[2];

        const float nearX = t1x < t2x ? t1x : t2x;
        const float farX = t1x > t2x ? t1x : t2x;
        const float nearY = t1y < t2y ? t1y : t2y;
        const float farY = t1y > t2y ? t1y : t2y;
        const float nearZ = t1z < t2z ? t1z : t2z;
        const float farZ = t1z > t2z ? t1z : t2z;

        const float nearXY = nearX > nearY ? nearX : nearY;
        const float tMin = nearXY > nearZ ? nearXY : nearZ;
        const float farXY = farX < farY ? farX : farY;
        const float tMax = (farXY < farZ ? farXY : farZ) * SLAB_ROBUST;

        tEnter[s] = tMin;
        if (tMin <= tMax && tMax >= 0.0f && tMin <= tBest){
            bits |= 1u << s;
        }
    }
    return bits & node.valid;
}

// ---- brute force

bool jasperengine::rayTriangle(const Vector3& origin, const Vector3& direction,
                               const Vector3& v0, const Vector3& v1, const Vector3& v2,
                               float& t, float& u, float& v, float& det){
    const Vector3 e1 = v1 - v0;
    const Vector3 e2 = v2 - v0;
    const Vector3 p = direction.crossProduct(e2);
    det = e1.dotProduct(p);
    if (det == 0.0f || !std::isfinite(det)){
        return false;
    }

    const float invDet = 1.0f / det;
    const Vector3 s = origin - v0;
    u = s.dotProduct(p) * invDet;
    if (!(u >= 0.0f && u <= 1.0f)){
        return false;
    }

    const Vector3 q = s.crossProduct(e1);
    v = direction.dotProduct(q) * invDet;
    if (!(v >= 0.0f && u + v <= 1.0f)){
        return false;
    }

    t = e2.dotProduct(q) * invDet;
    return t > 0.0f && std::isfinite(t);
}

namespace {

    // the closest-hit rule shared by the brute force and the tree: nearer, or as near and a
    // lower triangle
    inline bool closer(float t, uint32_t triangle, float bestT, uint32_t bestTriangle){
        return t < bestT || (t == bestT && triangle < bestTriangle);
    }

    inline bool hitOrder(const MeshBvhHit& a, const MeshBvhHit& b){
        return a.t < b.t || (a.t == b.t && a.triangle < b.triangle);
    }

    void keepNearest(std::vector<MeshBvhHit>& hits, uint32_t limit){
        std::sort(hits.begin(), hits.end(), hitOrder);
        if (hits.size() > limit){
            hits.resize(limit);
        }
    }

}

bool jasperengine::raycastTriangles(const PickGeometry& geometry, const MeshBvhRay& ray, MeshBvhHit& hit){
    const FaceTest faceTest(ray.faces);
    const uint32_t count = geometry.getTriangleCount();
    const Vector3* positions = geometry.positions.data();
    const uint32_t* indices = geometry.indices.data();
    const size_t vertexCount = geometry.positions.size();

    float bestT = ray.tMax;
    uint32_t bestTriangle = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++){
        const uint32_t a = indices[i * 3], b = indices[i * 3 + 1], c = indices[i * 3 + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount){
            continue;
        }
        float t, u, v, det;
        if (!rayTriangle(ray.origin, ray.direction, positions[a], positions[b], positions[c], t, u, v, det) || t > ray.tMax){
            continue;
        }
        if (!closer(t, i, bestT, bestTriangle)){
            continue;
        }
        const uint32_t submesh = geometry.submeshOf(i);
        const PickFaces mode = faceTest.isUniform ? faceTest.uniform : (submesh < ray.faces.size() ? ray.faces[submesh] : PickFaces::BOTH);
        if (!FaceTest::accepts(mode, det)){
            continue;
        }
        bestT = t;
        bestTriangle = i;
        hit = {i, submesh, t, u, v, det > 0.0f, positions[a], positions[b], positions[c]};
    }
    return bestTriangle != UINT32_MAX;
}

void jasperengine::raycastAllTriangles(const PickGeometry& geometry, const MeshBvhRay& ray, std::vector<MeshBvhHit>& hits, uint32_t limit){
    hits.clear();
    const FaceTest faceTest(ray.faces);
    const uint32_t count = geometry.getTriangleCount();
    const size_t vertexCount = geometry.positions.size();
    for (uint32_t i = 0; i < count; i++){
        const uint32_t a = geometry.indices[i * 3], b = geometry.indices[i * 3 + 1], c = geometry.indices[i * 3 + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount){
            continue;
        }
        float t, u, v, det;
        if (!rayTriangle(ray.origin, ray.direction, geometry.positions[a], geometry.positions[b], geometry.positions[c], t, u, v, det) ||
                t > ray.tMax){
            continue;
        }
        const uint32_t submesh = geometry.submeshOf(i);
        const PickFaces mode = faceTest.isUniform ? faceTest.uniform : (submesh < ray.faces.size() ? ray.faces[submesh] : PickFaces::BOTH);
        if (!FaceTest::accepts(mode, det)){
            continue;
        }
        hits.push_back({i, submesh, t, u, v, det > 0.0f, geometry.positions[a], geometry.positions[b], geometry.positions[c]});
    }
    keepNearest(hits, limit);
}

// ---- build

MeshBvh MeshBvh::build(const PickGeometry& geometry){
    MeshBvh bvh;
    bvh.submeshFirstTriangle = geometry.submeshFirstTriangle;

    const uint32_t count = geometry.getTriangleCount();
    const size_t vertexCount = geometry.positions.size();
    std::vector<Triangle> source;
    source.reserve(count);
    for (uint32_t i = 0; i < count; i++){
        const uint32_t a = geometry.indices[i * 3], b = geometry.indices[i * 3 + 1], c = geometry.indices[i * 3 + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount){
            continue;
        }
        Triangle triangle{geometry.positions[a], geometry.positions[b], geometry.positions[c], i};
        if (!finite(triangle.v0) || !finite(triangle.v1) || !finite(triangle.v2)){
            continue;
        }
        source.push_back(triangle);
    }

    Box rootBox;
    for (const Triangle& triangle : source){
        rootBox.expand(triangleBox(triangle));
    }
    if (!rootBox.empty()){
        bvh.bounds = AABB(rootBox.mn[0], rootBox.mn[1], rootBox.mn[2], rootBox.mx[0], rootBox.mx[1], rootBox.mx[2]);
    }

    if (source.size() < BRUTE_FORCE_TRIANGLES){
        bvh.triangles = std::move(source);
        return bvh;
    }

    const size_t n = source.size();
    std::vector<float> centroids(n * 3);
    std::vector<float> boxes(n * 6);
    for (size_t t = 0; t < n; t++){
        const Vector3 c = centroid(source[t]);
        centroids[t * 3 + 0] = c.x;
        centroids[t * 3 + 1] = c.y;
        centroids[t * 3 + 2] = c.z;
        const Box box = triangleBox(source[t]);
        for (int a = 0; a < 3; a++){
            boxes[t * 6 + a] = box.mn[a];
            boxes[t * 6 + 3 + a] = box.mx[a];
        }
    }
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < (uint32_t)n; i++){
        order[i] = i;
    }

    // binary binned-SAH tree, built with an explicit stack
    std::vector<BinaryNode> binary;
    binary.reserve(n / 2 + 1);
    binary.push_back(BinaryNode());
    struct Work { uint32_t node, first, count, depth; };
    std::vector<Work> work;
    work.push_back({0u, 0u, (uint32_t)n, 0u});
    while (!work.empty()){
        const Work item = work.back();
        work.pop_back();

        Box box;
        float cmn[3] = { FLOAT_MAX,  FLOAT_MAX,  FLOAT_MAX};
        float cmx[3] = {-FLOAT_MAX, -FLOAT_MAX, -FLOAT_MAX};
        for (uint32_t i = 0; i < item.count; i++){
            const uint32_t id = order[item.first + i];
            box.expand(&boxes[(size_t)id * 6]);
            for (int a = 0; a < 3; a++){
                const float c = centroids[(size_t)id * 3 + a];
                cmn[a] = c < cmn[a] ? c : cmn[a];
                cmx[a] = c > cmx[a] ? c : cmx[a];
            }
        }
        binary[item.node].box = box;
        binary[item.node].first = item.first;

        if (item.count <= LEAF_SIZE){
            binary[item.node].count = item.count;
            continue;
        }

        uint32_t leftCount = 0;
        bool split = false;
        if (item.depth < MAX_SAH_DEPTH){
            BinnedSahRange range;
            range.order = std::span<const uint32_t>(order).subspan(item.first, item.count);
            range.centroids = centroids;
            range.bounds = boxes;
            for (int a = 0; a < 3; a++){
                range.centroidMin[a] = cmn[a];
                range.centroidMax[a] = cmx[a];
            }
            const BinnedSahCandidate candidate = findBestBinnedSahSplit(range);
            if (candidate.found){
                uint32_t* begin = order.data() + item.first;
                uint32_t* end = begin + item.count;
                uint32_t* middle = std::partition(begin, end, [&](uint32_t id){
                    return binnedSahBinOf(centroids[(size_t)id * 3 + candidate.axis], candidate.axisMin, candidate.scale) <= candidate.bin;
                });
                leftCount = (uint32_t)(middle - begin);
                split = leftCount > 0 && leftCount < item.count;
            }
        }
        if (!split){
            // the median of the widest centroid axis (deep or degenerate ranges): keeps the
            // depth logarithmic
            int axis = 0;
            for (int a = 1; a < 3; a++){
                if (cmx[a] - cmn[a] > cmx[axis] - cmn[axis]) axis = a;
            }
            leftCount = item.count / 2;
            uint32_t* begin = order.data() + item.first;
            std::nth_element(begin, begin + leftCount, begin + item.count, [&](uint32_t x, uint32_t y){
                const float cx = centroids[(size_t)x * 3 + axis];
                const float cy = centroids[(size_t)y * 3 + axis];
                return cx < cy || (cx == cy && x < y);
            });
        }

        const uint32_t left = (uint32_t)binary.size();
        binary.push_back(BinaryNode());
        const uint32_t right = (uint32_t)binary.size();
        binary.push_back(BinaryNode());
        binary[item.node].left = left;
        binary[item.node].right = right;
        work.push_back({right, item.first + leftCount, item.count - leftCount, item.depth + 1});
        work.push_back({left, item.first, leftCount, item.depth + 1});
    }
    centroids.clear();
    centroids.shrink_to_fit();
    boxes.clear();
    boxes.shrink_to_fit();

    // the triangles in leaf order: a leaf slot names a range of them
    bvh.triangles.resize(n);
    for (size_t i = 0; i < n; i++){
        bvh.triangles[i] = source[order[i]];
    }
    source.clear();
    source.shrink_to_fit();
    order.clear();
    order.shrink_to_fit();

    // collapse: each 8-wide node takes up to 8 binary descendants, expanding the largest
    // inner one first (children are made after their parent, so their index is higher)
    bvh.nodes.reserve(binary.size() / 4 + 1);
    bvh.nodes.emplace_back();
    struct Collapse { uint32_t binaryNode, out; };
    std::vector<Collapse> pending;
    pending.push_back({0u, 0u});
    while (!pending.empty()){
        const Collapse item = pending.back();
        pending.pop_back();

        uint32_t slots[8];
        int slotCount = 1;
        slots[0] = item.binaryNode;
        if (binary[item.binaryNode].count == 0){
            slotCount = 0;
            slots[slotCount++] = binary[item.binaryNode].left;
            slots[slotCount++] = binary[item.binaryNode].right;
            while (slotCount < 8){
                int best = -1;
                float bestArea = -1.0f;
                for (int i = 0; i < slotCount; i++){
                    const BinaryNode& candidate = binary[slots[i]];
                    if (candidate.count > 0) continue;
                    const float area = candidate.box.area();
                    if (area > bestArea){
                        bestArea = area;
                        best = i;
                    }
                }
                if (best < 0) break;
                const BinaryNode& expanded = binary[slots[best]];
                const uint32_t left = expanded.left, right = expanded.right;
                for (int i = slotCount; i > best + 1; i--){
                    slots[i] = slots[i - 1];
                }
                slots[best] = left;
                slots[best + 1] = right;
                slotCount++;
            }
        }

        std::memset(static_cast<void*>(&bvh.nodes[item.out]), 0, sizeof(Node));
        for (int i = 0; i < slotCount; i++){
            const BinaryNode& child = binary[slots[i]];
            uint32_t first = child.first;
            uint16_t count = (uint16_t)child.count;
            if (child.count == 0){
                first = (uint32_t)bvh.nodes.size();
                bvh.nodes.emplace_back();
                pending.push_back({slots[i], first});
            }
            // indexed again: the push may have moved the nodes
            Node& node = bvh.nodes[item.out];
            node.first[i] = first;
            node.count[i] = count;
            node.valid |= (uint8_t)(1u << i);
        }
    }
    bvh.nodes.shrink_to_fit();

    // the boxes, bottom up, quantized against their parent's
    bvh.refitNodes();
    return bvh;
}

namespace {
    // a triangle's corners from `positions` (leafCorners order); one out of range keeps the
    // triangle as it was
    inline void loadCorners(MeshBvh::Triangle& triangle, std::span<const Vector3> positions, const uint32_t* corner){
        const uint32_t a = corner[0], b = corner[1], c = corner[2];
        if (a >= positions.size() || b >= positions.size() || c >= positions.size()){
            return;
        }
        triangle.v0 = positions[a];
        triangle.v1 = positions[b];
        triangle.v2 = positions[c];
    }
}

void MeshBvh::refitNodes(std::span<const Vector3> positions, std::span<const uint32_t> corners){
    // the leaf slots' boxes first, the bulk of the work: each node's on its own, so a large
    // mesh's (a posed copy, R30) on the workers, its triangles' new corners read on the way
    const size_t cornerCount = std::min(triangles.size(), corners.size() / 3);
    auto leafBoxes = [&](size_t index, Box* slotBoxes){
        const Node& node = nodes[index];
        for (int s = 0; s < 8; s++){
            if (!(node.valid & (1u << s)) || node.count[s] == 0){
                continue;
            }
            for (uint32_t t = node.first[s]; t < node.first[s] + node.count[s]; t++){
                Triangle& triangle = triangles[t];
                if (t < cornerCount){
                    loadCorners(triangle, positions, corners.data() + (size_t)t * 3);
                }
                slotBoxes[s].expand(triangleBox(triangle));
            }
            widen(slotBoxes[s]);
        }
    };
    constexpr size_t NODE_CHUNK = 256;
    std::vector<std::array<Box, 8>> leafSlotBoxes;
    if (triangles.size() > 16384){
        leafSlotBoxes.resize(nodes.size());
        jobs::parallelFor((nodes.size() + NODE_CHUNK - 1) / NODE_CHUNK, [&](size_t chunk){
            for (size_t index = chunk * NODE_CHUNK; index < std::min(nodes.size(), (chunk + 1) * NODE_CHUNK); index++){
                leafBoxes(index, leafSlotBoxes[index].data());
            }
        });
    }

    // full-precision box of each node, filled children first (they come after their parent)
    std::vector<Box> nodeBoxes(nodes.size());
    for (size_t index = nodes.size(); index-- > 0;){
        Node& node = nodes[index];
        Box slotBoxes[8];
        if (leafSlotBoxes.empty()){
            leafBoxes(index, slotBoxes);
        }else{
            std::copy(leafSlotBoxes[index].begin(), leafSlotBoxes[index].end(), slotBoxes);
        }
        Box box;
        for (int s = 0; s < 8; s++){
            if (!(node.valid & (1u << s))){
                continue;
            }
            if (node.count[s] == 0){
                slotBoxes[s] = nodeBoxes[node.first[s]];
            }
            box.expand(slotBoxes[s]);
        }
        if (box.empty()){
            // nothing finite below (a posed copy gone to NaN): a box no ray meets
            for (int a = 0; a < 3; a++){
                box.mn[a] = box.mx[a] = 0.0f;
            }
        }
        for (int a = 0; a < 3; a++){
            node.boxMin[a] = box.mn[a];
            node.scale[a] = nodeScale(box.mn[a], box.mx[a]);
        }
        for (int s = 0; s < 8; s++){
            if (!(node.valid & (1u << s)) || slotBoxes[s].empty()){
                // an empty or unused slot: an inverted box, never hit
                node.qMinX[s] = node.qMinY[s] = node.qMinZ[s] = 255;
                node.qMaxX[s] = node.qMaxY[s] = node.qMaxZ[s] = 0;
                continue;
            }
            quantize(node.boxMin[0], node.scale[0], slotBoxes[s].mn[0], slotBoxes[s].mx[0], node.qMinX[s], node.qMaxX[s]);
            quantize(node.boxMin[1], node.scale[1], slotBoxes[s].mn[1], slotBoxes[s].mx[1], node.qMinY[s], node.qMaxY[s]);
            quantize(node.boxMin[2], node.scale[2], slotBoxes[s].mn[2], slotBoxes[s].mx[2], node.qMinZ[s], node.qMaxZ[s]);
        }
        // what a parent quantizes this node against: the box it decodes to (it encloses
        // the true one)
        Box decoded;
        for (int a = 0; a < 3; a++){
            decoded.mn[a] = node.boxMin[a];
            decoded.mx[a] = decode(node.boxMin[a], 255, node.scale[a]);
        }
        nodeBoxes[index] = box.empty() ? box : decoded;
    }
    if (!nodeBoxes.empty() && !nodeBoxes[0].empty()){
        const Box& root = nodeBoxes[0];
        bounds = AABB(root.mn[0], root.mn[1], root.mn[2], root.mx[0], root.mx[1], root.mx[2]);
    }
}

std::vector<uint32_t> MeshBvh::leafCorners(std::span<const uint32_t> indices) const{
    std::vector<uint32_t> corners(triangles.size() * 3, UINT32_MAX);
    for (size_t t = 0; t < triangles.size(); t++){
        const size_t corner = (size_t)triangles[t].index * 3;
        if (corner + 2 < indices.size()){
            corners[t * 3 + 0] = indices[corner];
            corners[t * 3 + 1] = indices[corner + 1];
            corners[t * 3 + 2] = indices[corner + 2];
        }
    }
    return corners;
}

void MeshBvh::refit(std::span<const Vector3> positions, std::span<const uint32_t> indices){
    refitCorners(positions, leafCorners(indices));
}

void MeshBvh::refitCorners(std::span<const Vector3> positions, std::span<const uint32_t> corners){
    if (nodes.empty()){
        const size_t count = std::min(triangles.size(), corners.size() / 3);
        Box box;
        for (size_t t = 0; t < triangles.size(); t++){
            if (t < count){
                loadCorners(triangles[t], positions, corners.data() + t * 3);
            }
            box.expand(triangleBox(triangles[t]));
        }
        bounds = box.empty() ? AABB() : AABB(box.mn[0], box.mn[1], box.mn[2], box.mx[0], box.mx[1], box.mx[2]);
        return;
    }
    refitNodes(positions, corners);
}

// ---- queries

uint32_t MeshBvh::submeshOf(uint32_t triangle) const{
    if (submeshFirstTriangle.size() < 2){
        return 0;
    }
    auto it = std::upper_bound(submeshFirstTriangle.begin(), submeshFirstTriangle.end() - 1, triangle);
    return it == submeshFirstTriangle.begin() ? 0u : (uint32_t)(it - submeshFirstTriangle.begin()) - 1u;
}

size_t MeshBvh::memoryBytes() const{
    return triangles.capacity() * sizeof(Triangle) + nodes.capacity() * sizeof(Node) +
           submeshFirstTriangle.capacity() * sizeof(uint32_t) + sizeof(*this);
}

bool MeshBvh::raycastBrute(const MeshBvhRay& ray, MeshBvhHit& hit) const{
    const FaceTest faceTest(ray.faces);
    float bestT = ray.tMax;
    uint32_t bestTriangle = UINT32_MAX;
    for (const Triangle& triangle : triangles){
        float t, u, v, det;
        if (!rayTriangle(ray.origin, ray.direction, triangle.v0, triangle.v1, triangle.v2, t, u, v, det) || t > ray.tMax ||
                !closer(t, triangle.index, bestT, bestTriangle)){
            continue;
        }
        const uint32_t submesh = submeshOf(triangle.index);
        const PickFaces mode = faceTest.isUniform ? faceTest.uniform : (submesh < ray.faces.size() ? ray.faces[submesh] : PickFaces::BOTH);
        if (!FaceTest::accepts(mode, det)){
            continue;
        }
        bestT = t;
        bestTriangle = triangle.index;
        hit = {triangle.index, submesh, t, u, v, det > 0.0f, triangle.v0, triangle.v1, triangle.v2};
    }
    return bestTriangle != UINT32_MAX;
}

bool MeshBvh::raycast(const MeshBvhRay& ray, MeshBvhHit& hit) const{
    if (nodes.empty()){
        return raycastBrute(ray, hit);
    }

    const FaceTest faceTest(ray.faces);
    SlabRay slab;
    slab.origin[0] = ray.origin.x;
    slab.origin[1] = ray.origin.y;
    slab.origin[2] = ray.origin.z;
    slab.invDir[0] = safeInverse(ray.direction.x);
    slab.invDir[1] = safeInverse(ray.direction.y);
    slab.invDir[2] = safeInverse(ray.direction.z);
    const SlabTest8 slabTest = activeSlabTest();

    float bestT = ray.tMax;
    uint32_t bestTriangle = UINT32_MAX;

    uint32_t stack[TRAVERSAL_STACK];
    int top = 0;
    stack[top++] = 0;
    while (top > 0){
        const Node& node = nodes[stack[--top]];
        // a box entered up to rounding past the best hit is still opened
        const float bound = bestT * SLAB_ROBUST;
        float tEnter[8];
        const uint32_t bits = slabTest(node, slab, bound, tEnter);
        if (bits == 0){
            continue;
        }

        // the hit slots near first
        uint8_t order[8];
        int hitCount = 0;
        for (int s = 0; s < 8; s++){
            if (!(bits & (1u << s))) continue;
            int j = hitCount++;
            while (j > 0 && tEnter[order[j - 1]] > tEnter[s]){
                order[j] = order[j - 1];
                j--;
            }
            order[j] = (uint8_t)s;
        }

        // leaves first so the best t tightens before inner children are queued
        for (int i = 0; i < hitCount; i++){
            const int s = order[i];
            if (node.count[s] == 0 || tEnter[s] > bestT * SLAB_ROBUST) continue;
            for (uint32_t t = node.first[s]; t < node.first[s] + node.count[s]; t++){
                const Triangle& triangle = triangles[t];
                float tHit, u, v, det;
                if (!rayTriangle(ray.origin, ray.direction, triangle.v0, triangle.v1, triangle.v2, tHit, u, v, det) ||
                        tHit > ray.tMax || !closer(tHit, triangle.index, bestT, bestTriangle)){
                    continue;
                }
                uint32_t submesh = 0;
                PickFaces mode = faceTest.uniform;
                if (!faceTest.isUniform){
                    submesh = submeshOf(triangle.index);
                    mode = submesh < ray.faces.size() ? ray.faces[submesh] : PickFaces::BOTH;
                }
                if (!FaceTest::accepts(mode, det)){
                    continue;
                }
                bestT = tHit;
                bestTriangle = triangle.index;
                hit = {triangle.index, submesh, tHit, u, v, det > 0.0f, triangle.v0, triangle.v1, triangle.v2};
            }
        }
        // inner children far first, so the nearest is popped next
        for (int i = hitCount - 1; i >= 0; i--){
            const int s = order[i];
            if (node.count[s] != 0 || tEnter[s] > bestT * SLAB_ROBUST) continue;
            if (top < TRAVERSAL_STACK){
                stack[top++] = node.first[s];
            }else{
                Log::error("MeshBvh: traversal stack overflow (a tree deeper than its build allows)");
            }
        }
    }

    if (bestTriangle != UINT32_MAX && faceTest.isUniform){
        hit.submesh = submeshOf(hit.triangle);
    }
    return bestTriangle != UINT32_MAX;
}

void MeshBvh::raycastAll(const MeshBvhRay& ray, std::vector<MeshBvhHit>& hits) const{
    hits.clear();
    const FaceTest faceTest(ray.faces);
    auto test = [&](const Triangle& triangle){
        float t, u, v, det;
        if (!rayTriangle(ray.origin, ray.direction, triangle.v0, triangle.v1, triangle.v2, t, u, v, det) || t > ray.tMax){
            return;
        }
        const uint32_t submesh = submeshOf(triangle.index);
        const PickFaces mode = faceTest.isUniform ? faceTest.uniform : (submesh < ray.faces.size() ? ray.faces[submesh] : PickFaces::BOTH);
        if (FaceTest::accepts(mode, det)){
            hits.push_back({triangle.index, submesh, t, u, v, det > 0.0f, triangle.v0, triangle.v1, triangle.v2});
        }
    };

    if (nodes.empty()){
        for (const Triangle& triangle : triangles){
            test(triangle);
        }
        keepNearest(hits, MAX_ALL_HITS);
        return;
    }

    SlabRay slab;
    slab.origin[0] = ray.origin.x;
    slab.origin[1] = ray.origin.y;
    slab.origin[2] = ray.origin.z;
    slab.invDir[0] = safeInverse(ray.direction.x);
    slab.invDir[1] = safeInverse(ray.direction.y);
    slab.invDir[2] = safeInverse(ray.direction.z);
    const SlabTest8 slabTest = activeSlabTest();

    uint32_t stack[TRAVERSAL_STACK];
    int top = 0;
    stack[top++] = 0;
    while (top > 0){
        const Node& node = nodes[stack[--top]];
        float tEnter[8];
        const uint32_t bits = slabTest(node, slab, ray.tMax * SLAB_ROBUST, tEnter);
        for (int s = 0; s < 8; s++){
            if (!(bits & (1u << s))) continue;
            if (node.count[s] > 0){
                for (uint32_t t = node.first[s]; t < node.first[s] + node.count[s]; t++){
                    test(triangles[t]);
                }
            }else if (top < TRAVERSAL_STACK){
                stack[top++] = node.first[s];
            }else{
                Log::error("MeshBvh: traversal stack overflow (a tree deeper than its build allows)");
            }
        }
    }
    keepNearest(hits, MAX_ALL_HITS);
}

// ---- serialization

namespace {

    struct SerialHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t triangleCount;
        uint32_t nodeCount;
        uint32_t submeshCount;
        uint32_t reserved;
    };
    static_assert(sizeof(SerialHeader) == 24, "MeshBvh serial header layout: bump SERIAL_VERSION on a change");

    void append(std::vector<uint8_t>& out, const void* data, size_t size){
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
    }

    const uint8_t* take(std::span<const uint8_t> bytes, size_t& offset, size_t size){
        if (size > bytes.size() || offset > bytes.size() - size){
            return nullptr;
        }
        const uint8_t* p = bytes.data() + offset;
        offset += size;
        return p;
    }

}

void MeshBvh::serialize(std::vector<uint8_t>& out) const{
    out.clear();
    out.reserve(sizeof(SerialHeader) + triangles.size() * sizeof(uint32_t) +
                submeshFirstTriangle.size() * sizeof(uint32_t) + nodes.size() * sizeof(Node));

    SerialHeader header{};
    header.magic = SERIAL_MAGIC;
    header.version = SERIAL_VERSION;
    header.triangleCount = (uint32_t)triangles.size();
    header.nodeCount = (uint32_t)nodes.size();
    header.submeshCount = (uint32_t)submeshFirstTriangle.size();
    append(out, &header, sizeof(header));
    for (const Triangle& triangle : triangles){
        append(out, &triangle.index, sizeof(uint32_t));
    }
    if (!submeshFirstTriangle.empty()){
        append(out, submeshFirstTriangle.data(), submeshFirstTriangle.size() * sizeof(uint32_t));
    }
    if (!nodes.empty()){
        append(out, nodes.data(), nodes.size() * sizeof(Node));
    }
}

bool MeshBvh::deserialize(std::span<const uint8_t> bytes, const PickGeometry& geometry){
    triangles.clear();
    nodes.clear();
    submeshFirstTriangle.clear();
    bounds = AABB();

    auto fail = [this](){
        triangles.clear();
        nodes.clear();
        submeshFirstTriangle.clear();
        bounds = AABB();
        return false;
    };

    size_t offset = 0;
    const uint8_t* headerBytes = take(bytes, offset, sizeof(SerialHeader));
    if (!headerBytes){
        return fail();
    }
    SerialHeader header;
    std::memcpy(&header, headerBytes, sizeof(header));
    const uint32_t geometryTriangles = geometry.getTriangleCount();
    if (header.magic != SERIAL_MAGIC || header.version != SERIAL_VERSION || header.triangleCount > geometryTriangles ||
            header.submeshCount != geometry.submeshFirstTriangle.size()){
        return fail();
    }

    const uint8_t* indexBytes = take(bytes, offset, (size_t)header.triangleCount * sizeof(uint32_t));
    const uint8_t* submeshBytes = take(bytes, offset, (size_t)header.submeshCount * sizeof(uint32_t));
    const uint8_t* nodeBytes = take(bytes, offset, (size_t)header.nodeCount * sizeof(Node));
    if (!indexBytes || !submeshBytes || !nodeBytes || offset != bytes.size()){
        return fail();
    }

    submeshFirstTriangle.resize(header.submeshCount);
    std::memcpy(submeshFirstTriangle.data(), submeshBytes, (size_t)header.submeshCount * sizeof(uint32_t));
    if (submeshFirstTriangle != geometry.submeshFirstTriangle){
        return fail();
    }

    // each geometry triangle at most once
    const size_t vertexCount = geometry.positions.size();
    std::vector<uint8_t> seen(geometryTriangles, 0);
    for (uint32_t i = 0; i < header.triangleCount; i++){
        uint32_t index;
        std::memcpy(&index, indexBytes + (size_t)i * sizeof(uint32_t), sizeof(uint32_t));
        if (index >= geometryTriangles || seen[index]){
            return fail();
        }
        seen[index] = 1;
    }
    // the triangles: reads all over the geometry's indices and positions (leaf order), on the
    // workers for a large mesh
    triangles.resize(header.triangleCount);
    std::atomic<bool> outOfRange{false};
    auto readTriangles = [&](size_t begin, size_t end){
        for (size_t i = begin; i < end; i++){
            uint32_t index;
            std::memcpy(&index, indexBytes + i * sizeof(uint32_t), sizeof(uint32_t));
            const uint32_t* corner = geometry.indices.data() + (size_t)index * 3;
            if (corner[0] >= vertexCount || corner[1] >= vertexCount || corner[2] >= vertexCount){
                outOfRange.store(true, std::memory_order_relaxed);
                return;
            }
            triangles[i] = {geometry.positions[corner[0]], geometry.positions[corner[1]], geometry.positions[corner[2]], index};
        }
    };
    constexpr size_t TRIANGLE_CHUNK = 16384;
    if (header.triangleCount > TRIANGLE_CHUNK * 2){
        jobs::parallelFor((header.triangleCount + TRIANGLE_CHUNK - 1) / TRIANGLE_CHUNK, [&](size_t chunk){
            readTriangles(chunk * TRIANGLE_CHUNK, std::min<size_t>(header.triangleCount, (chunk + 1) * TRIANGLE_CHUNK));
        });
    }else{
        readTriangles(0, header.triangleCount);
    }
    if (outOfRange.load(std::memory_order_relaxed)){
        return fail();
    }
    // a geometry triangle the file leaves out must be one the build leaves out (not finite)
    for (uint32_t index = 0; index < geometryTriangles; index++){
        if (seen[index]) continue;
        const uint32_t a = geometry.indices[(size_t)index * 3], b = geometry.indices[(size_t)index * 3 + 1], c = geometry.indices[(size_t)index * 3 + 2];
        if (a < vertexCount && b < vertexCount && c < vertexCount &&
                finite(geometry.positions[a]) && finite(geometry.positions[b]) && finite(geometry.positions[c])){
            return fail();
        }
    }

    if (header.nodeCount == 0){
        if (header.triangleCount >= BRUTE_FORCE_TRIANGLES){
            return fail();
        }
        Box box;
        for (const Triangle& triangle : triangles){
            box.expand(triangleBox(triangle));
        }
        if (!box.empty()){
            bounds = AABB(box.mn[0], box.mn[1], box.mn[2], box.mx[0], box.mx[1], box.mx[2]);
        }
        return true;
    }

    nodes.resize(header.nodeCount);
    std::memcpy(static_cast<void*>(nodes.data()), nodeBytes, (size_t)header.nodeCount * sizeof(Node));

    // a tree: every node but the root referenced once, by a lower node; every triangle in
    // exactly one leaf
    std::vector<uint8_t> referenced(header.nodeCount, 0);
    std::vector<uint8_t> covered(header.triangleCount, 0);
    for (uint32_t index = 0; index < header.nodeCount; index++){
        const Node& node = nodes[index];
        for (int s = 0; s < 8; s++){
            if (!(node.valid & (1u << s))) continue;
            if (node.count[s] > 0){
                const uint64_t end = (uint64_t)node.first[s] + node.count[s];
                if (end > header.triangleCount){
                    return fail();
                }
                for (uint32_t t = node.first[s]; t < (uint32_t)end; t++){
                    if (covered[t]) return fail();
                    covered[t] = 1;
                }
            }else{
                const uint32_t child = node.first[s];
                if (child <= index || child >= header.nodeCount || referenced[child]){
                    return fail();
                }
                referenced[child] = 1;
            }
        }
    }
    for (uint32_t index = 1; index < header.nodeCount; index++){
        if (!referenced[index]) return fail();
    }
    for (uint32_t t = 0; t < header.triangleCount; t++){
        if (!covered[t]) return fail();
    }

    // the boxes are never taken from the file: recomputed from the triangles they bound
    refitNodes();
    return true;
}
