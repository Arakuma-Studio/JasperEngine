// SPDX-License-Identifier: MIT

#include "ConvexDecomposition.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// The V-HACD implementation is compiled here and nowhere else. Its own warnings are not ours:
// silenced for this include only, so the build stays warning-free on every compiler.
#ifdef JASPERENGINE_CONVEX_DECOMPOSITION
    // the size of V-HACD's job pool (m_asyncACD), read by its patched header
    namespace jasperengine { namespace convexdecomposition { unsigned vhacdThreadCount(); } }
    #define VHACD_THREAD_COUNT ::jasperengine::convexdecomposition::vhacdThreadCount()
    #if defined(_MSC_VER)
        #pragma warning(push, 0)
    #elif defined(__clang__)
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Weverything"
    #elif defined(__GNUC__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wpragmas"
        #pragma GCC diagnostic ignored "-Wall"
        #pragma GCC diagnostic ignored "-Wextra"
        #pragma GCC diagnostic ignored "-Wpedantic"
        #pragma GCC diagnostic ignored "-Wunused-parameter"
        #pragma GCC diagnostic ignored "-Wunused-variable"
        #pragma GCC diagnostic ignored "-Wunused-but-set-variable"
        #pragma GCC diagnostic ignored "-Wunused-function"
        #pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        #pragma GCC diagnostic ignored "-Wsign-compare"
        #pragma GCC diagnostic ignored "-Wsign-conversion"
        #pragma GCC diagnostic ignored "-Wconversion"
        #pragma GCC diagnostic ignored "-Wfloat-conversion"
        #pragma GCC diagnostic ignored "-Wdouble-promotion"
        #pragma GCC diagnostic ignored "-Wfloat-equal"
        #pragma GCC diagnostic ignored "-Wuseless-cast"
        #pragma GCC diagnostic ignored "-Wold-style-cast"
        #pragma GCC diagnostic ignored "-Wcast-align"
        #pragma GCC diagnostic ignored "-Wcast-qual"
        #pragma GCC diagnostic ignored "-Wshadow"
        #pragma GCC diagnostic ignored "-Wnull-dereference"
        #pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
        #pragma GCC diagnostic ignored "-Wclass-memaccess"
        #pragma GCC diagnostic ignored "-Wmisleading-indentation"
        #pragma GCC diagnostic ignored "-Wreorder"
        #pragma GCC diagnostic ignored "-Wdeprecated-copy"
        #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
        #pragma GCC diagnostic ignored "-Woverloaded-virtual"
        #pragma GCC diagnostic ignored "-Wtype-limits"
        #pragma GCC diagnostic ignored "-Wparentheses"
        #pragma GCC diagnostic ignored "-Wformat-truncation"
        #pragma GCC diagnostic ignored "-Wlogical-op"
        #pragma GCC diagnostic ignored "-Wduplicated-cond"
        #pragma GCC diagnostic ignored "-Wmissing-declarations"
    #endif
    #define ENABLE_VHACD_IMPLEMENTATION 1
    #include "VHACD.h"
    #if defined(_MSC_VER)
        #pragma warning(pop)
    #elif defined(__clang__)
        #pragma clang diagnostic pop
    #elif defined(__GNUC__)
        #pragma GCC diagnostic pop
    #endif
#endif

using namespace jasperengine;

#ifdef JASPERENGINE_CONVEX_DECOMPOSITION
// One core less than the machine has (at least 1, at most upstream's 8), so the editor's main
// thread keeps one while a job splits hulls in parallel
unsigned jasperengine::convexdecomposition::vhacdThreadCount(){
    const unsigned cores = std::thread::hardware_concurrency();
    return std::min(cores > 1 ? cores - 1 : 1u, 8u);
}
#endif

namespace{
    // Part of every key: bump it when anything that shapes the hulls changes (V-HACD version,
    // the settings below, welding), so hulls of an older algorithm are never served
    const uint32_t ALGORITHM_VERSION = 1;

    // .hulls file: magic "JHUL", format version, key, hull count, then per hull its point count
    // and float3 points, all little-endian
    const uint32_t FILE_MAGIC = 0x4C55484Au;
    const uint32_t FILE_VERSION = 1;
    const char* const FILE_EXTENSION = ".hulls";
    // sanity bounds for reading: far above what any parameters produce
    const uint32_t MAX_FILE_HULLS = 1u << 16;
    const uint32_t MAX_FILE_POINTS = 1u << 20;

    // clear() leaves another writer's temporary file alone unless it is this old (left by a crash)
    const std::chrono::hours STALE_TEMPORARY(1);

    const char* const CANCELLED = "cancelled";
    const char* const NOT_AVAILABLE = "convex decomposition is not available in this build";

    struct WeldedMesh {
        std::vector<Vector3> positions;
        std::vector<uint32_t> indices;
    };

    struct PositionKey {
        uint32_t bits[3];
        bool operator==(const PositionKey& other) const{
            return bits[0] == other.bits[0] && bits[1] == other.bits[1] && bits[2] == other.bits[2];
        }
    };

    struct PositionKeyHash {
        size_t operator()(const PositionKey& key) const{
            uint64_t h = key.bits[0];
            h = h * 0x9E3779B97F4A7C15ull ^ key.bits[1];
            h = h * 0x9E3779B97F4A7C15ull ^ key.bits[2];
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };

    uint32_t floatBits(float value){
        if (value == 0.0f){
            value = 0.0f;   // -0 welds with +0
        }
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    float bitsFloat(uint32_t bits){
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // Copies of a position map to its first index, in first-seen order. Triangles that collapse,
    // reference an index out of range or are cut short are left out.
    WeldedMesh weld(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices){
        WeldedMesh mesh;
        std::unordered_map<PositionKey, uint32_t, PositionKeyHash> firstIndex;
        firstIndex.reserve(positions.size());
        std::vector<uint32_t> remap(positions.size());
        for (size_t i = 0; i < positions.size(); i++){
            const Vector3& p = positions[i];
            const PositionKey key{{floatBits(p.x), floatBits(p.y), floatBits(p.z)}};
            auto inserted = firstIndex.emplace(key, static_cast<uint32_t>(mesh.positions.size()));
            if (inserted.second){
                mesh.positions.emplace_back(bitsFloat(key.bits[0]), bitsFloat(key.bits[1]), bitsFloat(key.bits[2]));
            }
            remap[i] = inserted.first->second;
        }

        mesh.indices.reserve(indices.size() - indices.size() % 3);
        for (size_t t = 0; t + 3 <= indices.size(); t += 3){
            if (indices[t] >= positions.size() || indices[t + 1] >= positions.size() || indices[t + 2] >= positions.size()){
                continue;
            }
            const uint32_t a = remap[indices[t]];
            const uint32_t b = remap[indices[t + 1]];
            const uint32_t c = remap[indices[t + 2]];
            if (a == b || b == c || a == c){
                continue;
            }
            mesh.indices.push_back(a);
            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
        }
        return mesh;
    }

    // ---- xxHash64 (Yann Collet's algorithm), over a little-endian byte buffer ---------------

    const uint64_t XXH_PRIME1 = 0x9E3779B185EBCA87ull;
    const uint64_t XXH_PRIME2 = 0xC2B2AE3D27D4EB4Full;
    const uint64_t XXH_PRIME3 = 0x165667B19E3779F9ull;
    const uint64_t XXH_PRIME4 = 0x85EBCA77C2B2AE63ull;
    const uint64_t XXH_PRIME5 = 0x27D4EB2F165667C5ull;

    uint64_t rotl64(uint64_t value, int bits){
        return (value << bits) | (value >> (64 - bits));
    }

    uint64_t readLE64(const uint8_t* p){
        uint64_t value = 0;
        for (int i = 7; i >= 0; i--){
            value = (value << 8) | p[i];
        }
        return value;
    }

    uint32_t readLE32(const uint8_t* p){
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    uint64_t xxhRound(uint64_t acc, uint64_t input){
        acc += input * XXH_PRIME2;
        acc = rotl64(acc, 31);
        return acc * XXH_PRIME1;
    }

    uint64_t xxhMerge(uint64_t acc, uint64_t value){
        acc ^= xxhRound(0, value);
        return acc * XXH_PRIME1 + XXH_PRIME4;
    }

    uint64_t xxh64(const uint8_t* data, size_t size, uint64_t seed){
        const uint8_t* p = data;
        const uint8_t* const end = data + size;
        uint64_t h;
        if (size >= 32){
            uint64_t v1 = seed + XXH_PRIME1 + XXH_PRIME2;
            uint64_t v2 = seed + XXH_PRIME2;
            uint64_t v3 = seed;
            uint64_t v4 = seed - XXH_PRIME1;
            const uint8_t* const limit = end - 32;
            do{
                v1 = xxhRound(v1, readLE64(p));
                v2 = xxhRound(v2, readLE64(p + 8));
                v3 = xxhRound(v3, readLE64(p + 16));
                v4 = xxhRound(v4, readLE64(p + 24));
                p += 32;
            }while (p <= limit);
            h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
            h = xxhMerge(h, v1);
            h = xxhMerge(h, v2);
            h = xxhMerge(h, v3);
            h = xxhMerge(h, v4);
        }else{
            h = seed + XXH_PRIME5;
        }
        h += static_cast<uint64_t>(size);
        while (p + 8 <= end){
            h ^= xxhRound(0, readLE64(p));
            h = rotl64(h, 27) * XXH_PRIME1 + XXH_PRIME4;
            p += 8;
        }
        if (p + 4 <= end){
            h ^= static_cast<uint64_t>(readLE32(p)) * XXH_PRIME1;
            h = rotl64(h, 23) * XXH_PRIME2 + XXH_PRIME3;
            p += 4;
        }
        while (p < end){
            h ^= static_cast<uint64_t>(*p) * XXH_PRIME5;
            h = rotl64(h, 11) * XXH_PRIME1;
            p++;
        }
        h ^= h >> 33;
        h *= XXH_PRIME2;
        h ^= h >> 29;
        h *= XXH_PRIME3;
        h ^= h >> 32;
        return h;
    }

    // ---- Little-endian serialization --------------------------------------------------------

    void putU32(std::vector<uint8_t>& out, uint32_t value){
        for (int i = 0; i < 4; i++){
            out.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }
    }

    void putU64(std::vector<uint8_t>& out, uint64_t value){
        for (int i = 0; i < 8; i++){
            out.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }
    }

    uint64_t hashWelded(const WeldedMesh& mesh, const DecompositionParams& params){
        std::vector<uint8_t> bytes;
        bytes.reserve(48 + mesh.positions.size() * 12 + mesh.indices.size() * 4);
        putU32(bytes, ALGORITHM_VERSION);
        putU32(bytes, params.maxHulls);
        putU32(bytes, params.resolution);
        putU32(bytes, params.maxVerticesPerHull);
        putU64(bytes, mesh.positions.size());
        for (const Vector3& p : mesh.positions){
            putU32(bytes, floatBits(p.x));
            putU32(bytes, floatBits(p.y));
            putU32(bytes, floatBits(p.z));
        }
        putU64(bytes, mesh.indices.size());
        for (uint32_t index : mesh.indices){
            putU32(bytes, index);
        }
        return xxh64(bytes.data(), bytes.size(), 0);
    }

    // ---- Disk entries ----------------------------------------------------------------------

    std::string keyHex(uint64_t key){
        static const char digits[] = "0123456789abcdef";
        std::string hex(16, '0');
        for (int i = 15; i >= 0; i--){
            hex[static_cast<size_t>(i)] = digits[key & 0xF];
            key >>= 4;
        }
        return hex;
    }

    // "<16 hex>.hulls" -> key; false for any other name
    bool parseEntryName(const std::string& name, uint64_t& key){
        const size_t extensionLength = std::strlen(FILE_EXTENSION);
        if (name.size() != 16 + extensionLength || name.compare(16, extensionLength, FILE_EXTENSION) != 0){
            return false;
        }
        uint64_t value = 0;
        for (size_t i = 0; i < 16; i++){
            const char c = name[i];
            uint64_t digit;
            if (c >= '0' && c <= '9'){
                digit = static_cast<uint64_t>(c - '0');
            }else if (c >= 'a' && c <= 'f'){
                digit = static_cast<uint64_t>(c - 'a' + 10);
            }else if (c >= 'A' && c <= 'F'){
                digit = static_cast<uint64_t>(c - 'A' + 10);
            }else{
                return false;
            }
            value = (value << 4) | digit;
        }
        key = value;
        return true;
    }

    // A temporary file this cache (any instance, any process) writes: "<16 hex>.hulls.<id>.tmp"
    bool isTemporaryName(const std::string& name){
        uint64_t key;
        const size_t entryLength = 16 + std::strlen(FILE_EXTENSION);
        return name.size() > entryLength + 4 && name[entryLength] == '.' &&
               parseEntryName(name.substr(0, entryLength), key) &&
               name.compare(name.size() - 4, 4, ".tmp") == 0;
    }

    std::vector<uint8_t> encodeHulls(uint64_t key, const std::vector<ConvexHullPoints>& hulls){
        std::vector<uint8_t> bytes;
        putU32(bytes, FILE_MAGIC);
        putU32(bytes, FILE_VERSION);
        putU64(bytes, key);
        putU32(bytes, static_cast<uint32_t>(hulls.size()));
        for (const ConvexHullPoints& hull : hulls){
            putU32(bytes, static_cast<uint32_t>(hull.points.size()));
            for (const Vector3& p : hull.points){
                putU32(bytes, floatBits(p.x));
                putU32(bytes, floatBits(p.y));
                putU32(bytes, floatBits(p.z));
            }
        }
        return bytes;
    }

    // False unless bytes are exactly one well-formed entry for key
    bool decodeHulls(const std::vector<uint8_t>& bytes, uint64_t key, std::vector<ConvexHullPoints>& out){
        size_t offset = 0;
        auto u32 = [&](uint32_t& value){
            if (bytes.size() - offset < 4){
                return false;
            }
            value = readLE32(bytes.data() + offset);
            offset += 4;
            return true;
        };
        uint32_t magic, version, keyLow, keyHigh, hullCount;
        if (!u32(magic) || magic != FILE_MAGIC || !u32(version) || version != FILE_VERSION ||
            !u32(keyLow) || !u32(keyHigh) || ((static_cast<uint64_t>(keyHigh) << 32) | keyLow) != key ||
            !u32(hullCount) || hullCount > MAX_FILE_HULLS){
            return false;
        }
        std::vector<ConvexHullPoints> hulls(hullCount);
        for (ConvexHullPoints& hull : hulls){
            uint32_t pointCount;
            if (!u32(pointCount) || pointCount > MAX_FILE_POINTS || (bytes.size() - offset) / 12 < pointCount){
                return false;
            }
            hull.points.resize(pointCount);
            for (Vector3& p : hull.points){
                uint32_t x = 0, y = 0, z = 0;
                u32(x);
                u32(y);
                u32(z);
                p = Vector3(bitsFloat(x), bitsFloat(y), bitsFloat(z));
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)){
                    return false;
                }
            }
        }
        if (offset != bytes.size()){
            return false;
        }
        out = std::move(hulls);
        return true;
    }

    // A name no other writer uses, in this process or another one: a random token per process
    // and a counter per write
    std::string temporarySuffix(){
        static const uint64_t processId = [](){
            std::random_device device;
            return (static_cast<uint64_t>(device()) << 32) ^ device();
        }();
        static std::atomic<uint64_t> counter{0};
        return "." + keyHex(processId) + "-" + std::to_string(counter++) + ".tmp";
    }

#ifdef JASPERENGINE_CONVEX_DECOMPOSITION
    bool validParams(const DecompositionParams& params, std::string* error){
        const char* problem = nullptr;
        if (params.maxHulls < 1){
            problem = "maxHulls must be at least 1";
        }else if (params.resolution < 10000 || params.resolution > 64000000){
            problem = "resolution must be between 10000 and 64000000 voxels";
        }else if (params.maxVerticesPerHull < 4){
            problem = "maxVerticesPerHull must be at least 4";
        }
        if (problem && error){
            *error = problem;
        }
        return !problem;
    }

    // True when every vertex lies on one plane (or line, or point), within a millionth of the
    // mesh size: the first tetrahedron of a hull, from the extremes, has no height
    bool hasNoVolume(const std::vector<Vector3>& points){
        const double FLAT_TOLERANCE = 1e-6;
        auto sub = [](const Vector3& a, const Vector3& b){
            return std::array<double, 3>{double(a.x) - double(b.x), double(a.y) - double(b.y), double(a.z) - double(b.z)};
        };
        auto cross = [](const std::array<double, 3>& a, const std::array<double, 3>& b){
            return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        };
        auto dot = [](const std::array<double, 3>& a, const std::array<double, 3>& b){
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        };

        const Vector3& a = points[0];
        size_t b = 0;
        double best = 0.0;
        for (size_t i = 1; i < points.size(); i++){
            const auto d = sub(points[i], a);
            if (dot(d, d) > best){
                best = dot(d, d);
                b = i;
            }
        }
        const double size = std::sqrt(best);
        if (!(size > 0.0)){
            return true;
        }
        const double tolerance = size * FLAT_TOLERANCE;

        const auto ab = sub(points[b], a);
        size_t c = 0;
        best = 0.0;
        for (size_t i = 0; i < points.size(); i++){
            const auto n = cross(ab, sub(points[i], a));
            if (dot(n, n) > best){
                best = dot(n, n);
                c = i;
            }
        }
        // distance of c from the line ab
        if (std::sqrt(best) / size <= tolerance){
            return true;
        }

        auto normal = cross(ab, sub(points[c], a));
        const double length = std::sqrt(dot(normal, normal));
        for (double& v : normal){
            v /= length;
        }
        for (const Vector3& p : points){
            if (std::fabs(dot(normal, sub(p, a))) > tolerance){
                return false;
            }
        }
        return true;
    }

    // Where a running decomposition registers its V-HACD instance, so another thread can stop it
    struct ActiveSlot {
        std::mutex* mutex = nullptr;
        VHACD::IVHACD** instance = nullptr;
    };

    // Polled by V-HACD between stages and while it splits: the cancel flag stops it even when it
    // was raised before V-HACD reset its own (Compute() clears it on entry)
    class CancelPoll: public VHACD::IVHACD::IUserCallback {
    public:
        CancelPoll(const std::atomic<bool>* cancelFlag, VHACD::IVHACD* instance): cancel(cancelFlag), vhacd(instance) {}

        void Update(const double, const double, const char* const, const char*) override{
            if (cancel && cancel->load()){
                vhacd->Cancel();
            }
        }

    private:
        const std::atomic<bool>* cancel;
        VHACD::IVHACD* vhacd;
    };
#else
    struct ActiveSlot {};
#endif

    bool decomposeWelded(const WeldedMesh& mesh, const DecompositionParams& params, std::vector<ConvexHullPoints>& out,
                         const std::atomic<bool>* cancel, std::string* error, const ActiveSlot& slot){
        out.clear();
        auto fail = [error](const std::string& message){
            if (error){
                *error = message;
            }
            return false;
        };

#ifndef JASPERENGINE_CONVEX_DECOMPOSITION
        (void)mesh;
        (void)params;
        (void)cancel;
        (void)slot;
        return fail(NOT_AVAILABLE);
#else
        if (!validParams(params, error)){
            return false;
        }
        if (mesh.indices.empty()){
            return fail("mesh has no triangles");
        }
        // only the vertices the triangles use: a stray one would skew V-HACD's normalization
        const uint32_t UNUSED = UINT32_MAX;
        std::vector<uint32_t> compact(mesh.positions.size(), UNUSED);
        std::vector<Vector3> usedPositions;
        std::vector<uint32_t> triangles;
        triangles.reserve(mesh.indices.size());
        for (uint32_t index : mesh.indices){
            if (compact[index] == UNUSED){
                compact[index] = static_cast<uint32_t>(usedPositions.size());
                usedPositions.push_back(mesh.positions[index]);
            }
            triangles.push_back(compact[index]);
        }
        if (usedPositions.size() < 4){
            return fail("mesh has fewer than 4 distinct vertices");
        }
        if (hasNoVolume(usedPositions)){
            return fail("mesh has no volume");
        }
        if (cancel && cancel->load()){
            return fail(CANCELLED);
        }

        std::vector<double> points;
        points.reserve(usedPositions.size() * 3);
        for (const Vector3& p : usedPositions){
            points.push_back(p.x);
            points.push_back(p.y);
            points.push_back(p.z);
        }

        VHACD::IVHACD* vhacd = VHACD::CreateVHACD();
        if (!vhacd){
            return fail("V-HACD could not be created");
        }
        CancelPoll poll(cancel, vhacd);

        VHACD::IVHACD::Parameters vparams;
        vparams.m_callback = &poll;
        vparams.m_maxConvexHulls = params.maxHulls;
        vparams.m_resolution = params.resolution;
        vparams.m_maxNumVerticesPerCH = params.maxVerticesPerHull;
        vparams.m_fillMode = VHACD::FillMode::FLOOD_FILL;
        vparams.m_shrinkWrap = true;
        // V-HACD's job pool splits and merges hulls in parallel while Compute() still blocks here:
        // 2-3x faster on concave meshes, and the hulls are bit-identical to a single-threaded run
        // whatever the thread count (README.jasper.md)
        vparams.m_asyncACD = true;

        if (slot.mutex){
            std::lock_guard<std::mutex> lock(*slot.mutex);
            *slot.instance = vhacd;
        }
        const bool computed = vhacd->Compute(points.data(), static_cast<uint32_t>(usedPositions.size()),
                                             triangles.data(), static_cast<uint32_t>(triangles.size() / 3), vparams);
        if (slot.mutex){
            std::lock_guard<std::mutex> lock(*slot.mutex);
            *slot.instance = nullptr;
        }

        bool ok = true;
        std::string message;
        if (cancel && cancel->load()){
            ok = false;
            message = CANCELLED;
        }else if (!computed){
            ok = false;
            message = "V-HACD failed";
        }else{
            const uint32_t count = vhacd->GetNConvexHulls();
            for (uint32_t i = 0; i < count && ok; i++){
                VHACD::IVHACD::ConvexHull hull;
                if (!vhacd->GetConvexHull(i, hull)){
                    ok = false;
                    message = "V-HACD did not return hull " + std::to_string(i);
                    break;
                }
                // a hull of fewer than 4 points is flat: no collision shape can be built from it
                if (hull.m_points.size() < 4){
                    continue;
                }
                ConvexHullPoints hullPoints;
                hullPoints.points.reserve(hull.m_points.size());
                for (const VHACD::Vertex& v : hull.m_points){
                    const Vector3 p(static_cast<float>(v.mX), static_cast<float>(v.mY), static_cast<float>(v.mZ));
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)){
                        ok = false;
                        message = "V-HACD returned a non-finite point";
                        break;
                    }
                    hullPoints.points.push_back(p);
                }
                out.push_back(std::move(hullPoints));
            }
            if (ok && out.empty()){
                ok = false;
                message = "V-HACD produced no hulls";
            }
        }
        vhacd->Release();

        if (!ok){
            out.clear();
            return fail(message);
        }
        return true;
#endif
    }

    // The checks of decompose() that welding hides: weld() drops what they catch
    bool validInput(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices, std::string* error){
        const char* problem = nullptr;
        if (indices.size() % 3 != 0){
            problem = "index count is not a multiple of 3";
        }
        for (size_t i = 0; i < indices.size() && !problem; i++){
            if (indices[i] >= positions.size()){
                problem = "triangle index out of range";
            }
        }
        for (size_t i = 0; i < positions.size() && !problem; i++){
            if (!std::isfinite(positions[i].x) || !std::isfinite(positions[i].y) || !std::isfinite(positions[i].z)){
                problem = "mesh has a non-finite position";
            }
        }
        if (problem && error){
            *error = problem;
        }
        return !problem;
    }
}

DecompositionParams DecompositionParams::fromQuality(DecompositionQuality quality){
    DecompositionParams p;
    switch (quality){
        case DecompositionQuality::LOW:
            p.maxHulls = 4;
            p.resolution = 100000;
            p.maxVerticesPerHull = 32;
            break;
        case DecompositionQuality::MEDIUM:
            p.maxHulls = 8;
            p.resolution = 400000;
            p.maxVerticesPerHull = 64;
            break;
        case DecompositionQuality::HIGH:
            p.maxHulls = 16;
            p.resolution = 1000000;
            p.maxVerticesPerHull = 64;
            break;
    }
    return p;
}

bool DecompositionParams::operator==(const DecompositionParams& o) const{
    return maxHulls == o.maxHulls && resolution == o.resolution && maxVerticesPerHull == o.maxVerticesPerHull;
}

uint64_t jasperengine::hashDecompositionInput(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices,
                                              const DecompositionParams& params){
    return hashWelded(weld(positions, indices), params);
}

struct ConvexDecompositionCache::Impl {
    struct Job {
        uint64_t key;
        WeldedMesh mesh;
        DecompositionParams params;
        bool urgent;
    };

    std::string directory;

    // state: everything below, except the disk and the worker thread object
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::unordered_map<uint64_t, std::vector<ConvexHullPoints>> memory;
    std::unordered_map<uint64_t, std::string> failures;
    // keys whose file was not on disk when last looked for: status() and find() skip the disk for
    // them until request(), store() or clear()
    std::unordered_set<uint64_t> diskMisses;
    std::deque<Job> queue;
    std::unordered_set<uint64_t> queued;
    bool running = false;
    uint64_t runningKey = 0;
    std::atomic<bool> runningCancel{false};
#ifdef JASPERENGINE_CONVEX_DECOMPOSITION
    VHACD::IVHACD* runningInstance = nullptr;
#endif
    bool stopping = false;
    std::atomic<uint64_t> completions{0};

    // the worker: started by request(), joined by cancelAll(); request() and cancelAll() hold it
    // throughout, so a worker is never started while another is being stopped
    std::mutex lifecycle;
    std::thread worker;

    // reads, writes and deletions of entry files; diskError too
    mutable std::mutex disk;
    std::string diskError;   // why the last write failed both tries; "" after one succeeded

    std::string entryPath(uint64_t key) const{
        return (std::filesystem::path(directory) / (keyHex(key) + FILE_EXTENSION)).string();
    }

    // Urgent jobs queue ahead of every other one, in the order they were made urgent. Caller holds
    // the state lock.
    void enqueueLocked(Job job){
        auto position = queue.end();
        if (job.urgent){
            position = std::find_if(queue.begin(), queue.end(), [](const Job& queuedJob){ return !queuedJob.urgent; });
        }
        queued.insert(job.key);
        queue.insert(position, std::move(job));
    }

    // A queued job that is not urgent becomes urgent. Caller holds the state lock.
    void promoteLocked(uint64_t key){
        auto it = std::find_if(queue.begin(), queue.end(), [key](const Job& job){ return job.key == key; });
        if (it == queue.end() || it->urgent){
            return;
        }
        Job job = std::move(*it);
        queue.erase(it);
        job.urgent = true;
        enqueueLocked(std::move(job));
    }

    // Queued or running. Caller holds the state lock.
    bool pendingLocked(uint64_t key) const{
        return queued.count(key) > 0 || (running && runningKey == key);
    }

    void stopRunningLocked(){
        runningCancel = true;
#ifdef JASPERENGINE_CONVEX_DECOMPOSITION
        if (runningInstance){
            runningInstance->Cancel();
        }
#endif
    }

    // Reads key's file; a file that does not decode is deleted. No lock held by the caller.
    bool loadFromDisk(uint64_t key, std::vector<ConvexHullPoints>& out) const{
        if (directory.empty()){
            return false;
        }
        std::lock_guard<std::mutex> lock(disk);
        const std::string path = entryPath(key);
        // not a file (missing, or a directory in its way): a miss, and nothing to delete
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)){
            return false;
        }
        std::vector<uint8_t> bytes;
        try{
            std::ifstream in(path, std::ios::binary);
            if (!in){
                return false;
            }
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (in.bad()){
                return false;
            }
        }catch (const std::exception&){
            // libstdc++ throws on a read error whatever the stream's exception mask: a miss, the
            // file may be fine for the next reader
            return false;
        }
        if (!decodeHulls(bytes, key, out)){
            std::filesystem::remove(path, ec);
            return false;
        }
        return true;
    }

    // Written aside and renamed into place: a reader never sees half a file. No lock held.
    void writeToDisk(uint64_t key, const std::vector<ConvexHullPoints>& hulls){
        if (directory.empty()){
            return;
        }
        const std::vector<uint8_t> bytes = encodeHulls(key, hulls);
        std::lock_guard<std::mutex> lock(disk);
        const std::string path = entryPath(key);
        std::string problem;
        // a second try, under a new name, before giving up on the file (the hulls stay in memory)
        for (int attempt = 0; attempt < 2; attempt++){
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            const std::string temporary = path + temporarySuffix();
            bool written;
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                out.close();
                written = static_cast<bool>(out);
            }
            if (!written){
                problem = "could not write " + temporary;
            }else{
                std::filesystem::rename(temporary, path, ec);
                if (!ec){
                    diskError.clear();
                    return;
                }
                problem = "could not rename " + temporary + ": " + ec.message();
            }
            std::filesystem::remove(temporary, ec);
        }
        diskError = problem;
    }

    // Entry files of the directory, by key
    template <typename Visit>
    void scanDisk(Visit visit) const{
        if (directory.empty()){
            return;
        }
        std::error_code ec;
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)){
            uint64_t key;
            std::error_code fileError;
            if (parseEntryName(it->path().filename().string(), key) && it->is_regular_file(fileError)){
                visit(key, *it);
            }
        }
    }

    void storeHulls(uint64_t key, const std::vector<ConvexHullPoints>& hulls){
        {
            std::lock_guard<std::mutex> lock(mutex);
            memory[key] = hulls;
            failures.erase(key);
            diskMisses.erase(key);
        }
        writeToDisk(key, hulls);
    }

    void workerLoop(){
        std::unique_lock<std::mutex> lock(mutex);
        while (true){
            wake.wait(lock, [this]{ return stopping || !queue.empty(); });
            if (stopping){
                return;
            }
            Job job = std::move(queue.front());
            queue.pop_front();
            queued.erase(job.key);
            running = true;
            runningKey = job.key;
            runningCancel = false;
            lock.unlock();

            std::vector<ConvexHullPoints> hulls;
            std::string error;
            ActiveSlot slot;
#ifdef JASPERENGINE_CONVEX_DECOMPOSITION
            slot.mutex = &mutex;
            slot.instance = &runningInstance;
#endif
            bool ok = decomposeWelded(job.mesh, job.params, hulls, &runningCancel, &error, slot);
            if (runningCancel.load()){
                ok = false;
                error = CANCELLED;
            }
            if (ok){
                storeHulls(job.key, hulls);
            }

            lock.lock();
            running = false;
            if (!ok){
                failures[job.key] = error;
            }
            completions++;
        }
    }
};

ConvexDecompositionCache::ConvexDecompositionCache(std::string directory): impl(new Impl){
    impl->directory = std::move(directory);
}

ConvexDecompositionCache::~ConvexDecompositionCache(){
    cancelAll();
}

const std::string& ConvexDecompositionCache::getDirectory() const{
    return impl->directory;
}

bool ConvexDecompositionCache::find(uint64_t key, std::vector<ConvexHullPoints>& out) const{
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        auto it = impl->memory.find(key);
        if (it != impl->memory.end()){
            out = it->second;
            return true;
        }
        if (impl->directory.empty() || impl->diskMisses.count(key)){
            return false;
        }
    }
    std::vector<ConvexHullPoints> hulls;
    const bool loaded = impl->loadFromDisk(key, hulls);
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!loaded){
        // unless it landed in memory meanwhile (a job or store()), it is a miss until request()
        if (!impl->memory.count(key)){
            impl->diskMisses.insert(key);
        }
        return false;
    }
    out = impl->memory.emplace(key, std::move(hulls)).first->second;
    return true;
}

void ConvexDecompositionCache::store(uint64_t key, const std::vector<ConvexHullPoints>& hulls){
    impl->storeHulls(key, hulls);
}

DecompositionStatus ConvexDecompositionCache::status(uint64_t key) const{
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->memory.count(key)){
            return DecompositionStatus::READY;
        }
        if (impl->pendingLocked(key)){
            return DecompositionStatus::PENDING;
        }
        if (impl->failures.count(key)){
            return DecompositionStatus::FAILED;
        }
    }
    std::vector<ConvexHullPoints> hulls;
    return find(key, hulls) ? DecompositionStatus::READY : DecompositionStatus::NONE;
}

uint64_t ConvexDecompositionCache::request(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices,
                                           const DecompositionParams& params){
    return request(positions, indices, params, false);
}

uint64_t ConvexDecompositionCache::request(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices,
                                           const DecompositionParams& params, bool urgent){
    WeldedMesh mesh = weld(positions, indices);
    const uint64_t key = hashWelded(mesh, params);

    // cached, pending or failed for good: nothing to do, except an urgent request moving its
    // queued job up
    auto settled = [this, key, urgent](){
        if (impl->memory.count(key)){
            return true;
        }
        if (impl->pendingLocked(key)){
            if (urgent){
                impl->promoteLocked(key);
            }
            return true;
        }
        auto failure = impl->failures.find(key);
        return failure != impl->failures.end() && failure->second != CANCELLED;
    };
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (settled()){
            return key;
        }
        // another process may have written it since status() or find() last missed
        impl->diskMisses.erase(key);
    }
    std::vector<ConvexHullPoints> hulls;
    if (find(key, hulls)){
        return key;
    }

    // invalid input fails here, as decompose() would, without a trip through the worker
    std::string error;
#ifdef JASPERENGINE_CONVEX_DECOMPOSITION
    const bool refused = !validInput(positions, indices, &error);
#else
    const bool refused = true;
    error = NOT_AVAILABLE;
#endif

    std::lock_guard<std::mutex> lifecycle(impl->lifecycle);
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (settled()){
        return key;
    }
    impl->failures.erase(key);
    if (refused){
        impl->failures[key] = error;
        impl->completions++;
        return key;
    }
#ifdef JASPERENGINE_CONVEX_DECOMPOSITION
    impl->enqueueLocked(Impl::Job{key, std::move(mesh), params, urgent});
    if (!impl->worker.joinable()){
        Impl* state = impl.get();
        impl->worker = std::thread([state](){ state->workerLoop(); });
    }
    impl->wake.notify_one();
#endif
    return key;
}

void ConvexDecompositionCache::prioritize(uint64_t key){
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->promoteLocked(key);
}

void ConvexDecompositionCache::cancel(uint64_t key){
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->queued.count(key)){
        impl->queue.erase(std::remove_if(impl->queue.begin(), impl->queue.end(),
                                         [key](const Impl::Job& job){ return job.key == key; }),
                          impl->queue.end());
        impl->queued.erase(key);
        impl->failures[key] = CANCELLED;
        impl->completions++;
    }else if (impl->running && impl->runningKey == key){
        impl->stopRunningLocked();
    }
}

void ConvexDecompositionCache::cancelAll(){
    std::lock_guard<std::mutex> lifecycle(impl->lifecycle);
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        for (const Impl::Job& job : impl->queue){
            impl->failures[job.key] = CANCELLED;
            impl->completions++;
        }
        impl->queue.clear();
        impl->queued.clear();
        if (impl->running){
            impl->stopRunningLocked();
        }
        impl->stopping = true;
        worker = std::move(impl->worker);
    }
    impl->wake.notify_all();
    if (worker.joinable()){
        worker.join();
    }
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->stopping = false;
}

size_t ConvexDecompositionCache::pendingCount() const{
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->queue.size() + (impl->running ? 1 : 0);
}

uint64_t ConvexDecompositionCache::completionVersion() const{
    return impl->completions.load();
}

std::string ConvexDecompositionCache::lastDiskError() const{
    std::lock_guard<std::mutex> lock(impl->disk);
    return impl->diskError;
}

std::string ConvexDecompositionCache::failureReason(uint64_t key) const{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->memory.count(key) || impl->pendingLocked(key)){
        return "";
    }
    auto it = impl->failures.find(key);
    return it != impl->failures.end() ? it->second : "";
}

void ConvexDecompositionCache::clear(){
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->memory.clear();
        impl->failures.clear();
        impl->diskMisses.clear();
    }
    if (impl->directory.empty()){
        return;
    }
    // Temporary files belong to writers still busy, in another process (a CLI run sharing the
    // project's cache) or another instance: deleting one would make its rename fail. This
    // instance has none in flight (its writes hold the disk lock), so only ones a crash left,
    // old enough, go.
    std::lock_guard<std::mutex> lock(impl->disk);
    impl->diskError.clear();
    std::vector<std::filesystem::path> doomed;
    const auto staleBefore = std::filesystem::file_time_type::clock::now() - STALE_TEMPORARY;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(impl->directory, ec), end; !ec && it != end; it.increment(ec)){
        uint64_t key;
        const std::string name = it->path().filename().string();
        std::error_code timeError;
        if (parseEntryName(name, key) ||
            (isTemporaryName(name) && it->last_write_time(timeError) < staleBefore && !timeError)){
            doomed.push_back(it->path());
        }
    }
    for (const std::filesystem::path& path : doomed){
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
    }
}

size_t ConvexDecompositionCache::entryCount() const{
    std::unordered_set<uint64_t> keys;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        for (const auto& entry : impl->memory){
            keys.insert(entry.first);
        }
    }
    std::lock_guard<std::mutex> lock(impl->disk);
    impl->scanDisk([&keys](uint64_t key, const std::filesystem::directory_entry&){
        keys.insert(key);
    });
    return keys.size();
}

size_t ConvexDecompositionCache::bytesOnDisk() const{
    size_t bytes = 0;
    std::lock_guard<std::mutex> lock(impl->disk);
    impl->scanDisk([&bytes](uint64_t, const std::filesystem::directory_entry& entry){
        std::error_code ec;
        const uintmax_t size = entry.file_size(ec);
        if (!ec){
            bytes += static_cast<size_t>(size);
        }
    });
    return bytes;
}

bool ConvexDecompositionCache::decompose(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices,
                                         const DecompositionParams& params, std::vector<ConvexHullPoints>& out,
                                         const std::atomic<bool>* cancel, std::string* error){
    out.clear();
    if (!available()){
        if (error){
            *error = NOT_AVAILABLE;
        }
        return false;
    }
    if (!validInput(positions, indices, error)){
        return false;
    }
    return decomposeWelded(weld(positions, indices), params, out, cancel, error, ActiveSlot());
}

bool ConvexDecompositionCache::available(){
#ifdef JASPERENGINE_CONVEX_DECOMPOSITION
    return true;
#else
    return false;
#endif
}
