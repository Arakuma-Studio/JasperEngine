// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelSample.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

using namespace jasperengine::voxel;

namespace {

    // Reads samples, keeping the last block, which is what a march and a trilinear read touch next
    class Cursor {
    public:
        explicit Cursor(Store& store): store(store), samples(store.levelSamples(0)), closed(store.desc().bounds == BoundsMode::Closed) {}

        const BlockContent& blockAt(int bx, int by, int bz) {
            const BlockKey key{0, bx, by, bz};
            if (key != current) {
                current = key;
                content = store.content(key);
            }
            return content;
        }
        int16_t sdf(int x, int y, int z) {
            if (closed && (x >= samples.x || y >= samples.y || z >= samples.z)) return SDF_AIR;
            x = std::clamp(x, 0, samples.x - 1);
            y = std::clamp(y, 0, samples.y - 1);
            z = std::clamp(z, 0, samples.z - 1);
            return blockAt(x >> BLOCK_SHIFT, y >> BLOCK_SHIFT, z >> BLOCK_SHIFT).sdfAt(sampleIndex(x & 15, y & 15, z & 15));
        }
        uint16_t material(int x, int y, int z) {
            x = std::clamp(x, 0, samples.x - 1);
            y = std::clamp(y, 0, samples.y - 1);
            z = std::clamp(z, 0, samples.z - 1);
            return blockAt(x >> BLOCK_SHIFT, y >> BLOCK_SHIFT, z >> BLOCK_SHIFT).materialAt(sampleIndex(x & 15, y & 15, z & 15));
        }
        // Trilinear distance in voxels
        double value(double x, double y, double z) {
            const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y)), iz = static_cast<int>(std::floor(z));
            const double fx = x - ix, fy = y - iy, fz = z - iz;
            const double c00 = sdf(ix, iy, iz) * (1.0 - fx) + sdf(ix + 1, iy, iz) * fx;
            const double c10 = sdf(ix, iy + 1, iz) * (1.0 - fx) + sdf(ix + 1, iy + 1, iz) * fx;
            const double c01 = sdf(ix, iy, iz + 1) * (1.0 - fx) + sdf(ix + 1, iy, iz + 1) * fx;
            const double c11 = sdf(ix, iy + 1, iz + 1) * (1.0 - fx) + sdf(ix + 1, iy + 1, iz + 1) * fx;
            const double c0 = c00 * (1.0 - fy) + c10 * fy;
            const double c1 = c01 * (1.0 - fy) + c11 * fy;
            return (c0 * (1.0 - fz) + c1 * fz) / SDF_UNITS_PER_VOXEL;
        }

        Store& store;
        Int3 samples;

    private:
        bool closed;
        BlockKey current{-1, 0, 0, 0};
        BlockContent content;
    };

    constexpr double RAY_PRECISION_VOXELS = 1.0 / 64.0;

    int dominantSlot(uint16_t material) {
        return materialWeightB(material) > 127 ? materialSlotB(material) : materialSlotA(material);
    }

}

RaycastHit jasperengine::voxel::raycast(Store& store, const Vec3d& origin, const Vec3d& direction, double maxDistance) {
    RaycastHit hit;
    const double voxelSize = store.desc().voxelSize;
    const double directionLength = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!(directionLength > 0.0) || !(maxDistance > 0.0)) return hit;
    const Vec3d dir{direction.x / directionLength, direction.y / directionLength, direction.z / directionLength};
    const Vec3d start{origin.x / voxelSize, origin.y / voxelSize, origin.z / voxelSize};
    const double limit = maxDistance / voxelSize;
    const Int3 dim = store.desc().dimensions;

    // Clip to the volume: outside it is air, so the march starts where the ray enters
    double t0 = 0.0, t1 = limit;
    const double from[3] = {start.x, start.y, start.z};
    const double along[3] = {dir.x, dir.y, dir.z};
    const double high[3] = {static_cast<double>(dim.x), static_cast<double>(dim.y), static_cast<double>(dim.z)};
    for (int axis = 0; axis < 3; axis++) {
        if (along[axis] == 0.0) {
            if (from[axis] < 0.0 || from[axis] > high[axis]) return hit;
            continue;
        }
        double ta = (0.0 - from[axis]) / along[axis];
        double tb = (high[axis] - from[axis]) / along[axis];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    }
    if (t0 > t1) return hit;

    Cursor cursor(store);
    const auto position = [&](double t) { return Vec3d{start.x + dir.x * t, start.y + dir.y * t, start.z + dir.z * t}; };
    const auto valueOf = [&](double t) {
        const Vec3d p = position(t);
        return cursor.value(p.x, p.y, p.z);
    };

    double t = t0;
    double previousValue = valueOf(t);
    if (previousValue < 0.0) {
        // Starting inside solid hits at once
        hit.hit = true;
        hit.distance = t * voxelSize;
    } else {
        while (t < t1) {
            const Vec3d p = position(t);
            double step = previousValue;
            // A noise base is no true SDF (Risks): a step never goes past one voxel
            if (step > 1.0) step = 1.0;
            if (step < RAY_PRECISION_VOXELS) step = RAY_PRECISION_VOXELS;
            // A uniform air block can be crossed in one go
            const int bx = static_cast<int>(std::floor(p.x)) >> BLOCK_SHIFT;
            const int by = static_cast<int>(std::floor(p.y)) >> BLOCK_SHIFT;
            const int bz = static_cast<int>(std::floor(p.z)) >> BLOCK_SHIFT;
            const Int3 count = store.blockCount(0);
            if (bx >= 0 && by >= 0 && bz >= 0 && bx < count.x && by < count.y && bz < count.z) {
                const BlockContent& block = cursor.blockAt(bx, by, bz);
                if (block.sdfIsUniform() && block.sdfValueIfUniform() >= SDF_UNITS_PER_VOXEL * 2) {
                    double exit = t1;
                    const int coords[3] = {bx, by, bz};
                    for (int axis = 0; axis < 3; axis++) {
                        if (along[axis] > 0.0) exit = std::min(exit, (((coords[axis] + 1) << BLOCK_SHIFT) - from[axis]) / along[axis]);
                        else if (along[axis] < 0.0) exit = std::min(exit, ((coords[axis] << BLOCK_SHIFT) - from[axis]) / along[axis]);
                    }
                    // Stop one voxel short: the next block's samples decide the crossing
                    const double jump = exit - t - 1.0;
                    if (jump > step) step = jump;
                }
            }
            double nextT = t + step;
            if (nextT > t1) nextT = t1;
            const double nextValue = valueOf(nextT);
            if (nextValue < 0.0) {
                // Bisection: a crossing is verified, so an inexact field is slower, not wrong
                double low = t, high2 = nextT;
                while (high2 - low > RAY_PRECISION_VOXELS * 0.25) {
                    const double middle = (low + high2) * 0.5;
                    if (valueOf(middle) < 0.0) high2 = middle; else low = middle;
                }
                hit.hit = true;
                hit.distance = high2 * voxelSize;
                t = high2;
                break;
            }
            previousValue = nextValue;
            t = nextT;
            if (nextT >= t1) break;
        }
    }
    if (!hit.hit) return hit;
    const Vec3d p = position(hit.distance / voxelSize);
    hit.point = Vec3d{p.x * voxelSize, p.y * voxelSize, p.z * voxelSize};
    const double gx = cursor.value(p.x + 0.5, p.y, p.z) - cursor.value(p.x - 0.5, p.y, p.z);
    const double gy = cursor.value(p.x, p.y + 0.5, p.z) - cursor.value(p.x, p.y - 0.5, p.z);
    const double gz = cursor.value(p.x, p.y, p.z + 0.5) - cursor.value(p.x, p.y, p.z - 0.5);
    const double length = std::sqrt(gx * gx + gy * gy + gz * gz);
    if (length > 0.0) hit.normal = Vec3d{gx / length, gy / length, gz / length};
    hit.slot = dominantSlot(cursor.material(static_cast<int>(std::floor(p.x + 0.5)), static_cast<int>(std::floor(p.y + 0.5)), static_cast<int>(std::floor(p.z + 0.5))));
    return hit;
}

double jasperengine::voxel::valueAt(Store& store, const Vec3d& point) {
    const double voxelSize = store.desc().voxelSize;
    const Vec3d p{point.x / voxelSize, point.y / voxelSize, point.z / voxelSize};
    const Int3 dim = store.desc().dimensions;
    if (p.x < 0.0 || p.y < 0.0 || p.z < 0.0 || p.x > dim.x || p.y > dim.y || p.z > dim.z) return SDF_BAND_VOXELS * voxelSize;
    Cursor cursor(store);
    return cursor.value(p.x, p.y, p.z) * voxelSize;
}

MaterialSample jasperengine::voxel::materialAtPoint(Store& store, const Vec3d& point) {
    const double voxelSize = store.desc().voxelSize;
    const uint16_t material = store.materialAt(0, static_cast<int>(std::floor(point.x / voxelSize + 0.5)), static_cast<int>(std::floor(point.y / voxelSize + 0.5)), static_cast<int>(std::floor(point.z / voxelSize + 0.5)));
    MaterialSample out;
    const int a = materialSlotA(material), b = materialSlotB(material), w = materialWeightB(material);
    if (w > 127) {
        out.slot = b;
        out.second = a;
        out.weight = 255 - w;
    } else {
        out.slot = a;
        out.second = b;
        out.weight = w;
    }
    return out;
}

uint64_t jasperengine::voxel::blockContentHash(const BlockContent& content) {
    // Explicit little-endian bytes, sample by sample: the same on every target, however the block is held
    uint64_t hash = FNV_OFFSET;
    for (int i = 0; i < BLOCK_SAMPLES; i++) {
        const uint16_t sdf = static_cast<uint16_t>(content.sdfAt(i));
        const uint16_t material = content.materialAt(i);
        const uint8_t bytes[4] = {static_cast<uint8_t>(sdf), static_cast<uint8_t>(sdf >> 8), static_cast<uint8_t>(material), static_cast<uint8_t>(material >> 8)};
        hash = fnv1a(bytes, 4, hash);
    }
    return hash;
}

uint64_t jasperengine::voxel::contentHash(const Store& store, const BlockReader& reader) {
    const Int3 count = store.blockCount(0);
    uint64_t hash = FNV_OFFSET;
    std::unordered_map<uint32_t, uint64_t> uniformHashes;
    for (int z = 0; z < count.z; z++) {
        for (int y = 0; y < count.y; y++) {
            for (int x = 0; x < count.x; x++) {
                const BlockKey key{0, x, y, z};
                const BlockContent content = reader ? reader(key) : store.readBlock(key);
                uint64_t blockHash = 0;
                if (content.sdfIsUniform() && content.materialIsUniform()) {
                    const uint32_t code = static_cast<uint32_t>(static_cast<uint16_t>(content.sdfValueIfUniform())) | (static_cast<uint32_t>(content.materialValueIfUniform()) << 16);
                    const auto found = uniformHashes.find(code);
                    if (found != uniformHashes.end()) {
                        blockHash = found->second;
                    } else {
                        blockHash = blockContentHash(content);
                        uniformHashes.emplace(code, blockHash);
                    }
                } else {
                    blockHash = blockContentHash(content);
                }
                uint8_t bytes[8];
                for (int i = 0; i < 8; i++) bytes[i] = static_cast<uint8_t>(blockHash >> (8 * i));
                hash = fnv1a(bytes, 8, hash);
            }
        }
    }
    return hash;
}

ConnectivityResult jasperengine::voxel::connectivity(Store& store, const Int3* from, const Int3* to) {
    // Union-find over the blocks' air pieces: a uniform air block is one node, a mixed block has one
    // node per air component inside it, and blocks then join across shared faces
    const Int3 count = store.blockCount(0);
    const Int3 samples = store.levelSamples(0);
    struct BlockInfo {
        int firstNode = -1;
        int nodeCount = 0;
        int labels = -1;   // index into labelStore, -1 when uniform
    };
    std::vector<BlockInfo> blocks(static_cast<size_t>(count.x) * count.y * count.z);
    std::vector<std::vector<uint16_t>> labelStore;
    std::vector<int> parent;
    std::vector<AirComponent> nodes;
    const auto blockIndex = [&](int x, int y, int z) { return static_cast<size_t>(x) + static_cast<size_t>(count.x) * (static_cast<size_t>(y) + static_cast<size_t>(count.y) * static_cast<size_t>(z)); };
    const auto findRoot = [&](int node) {
        while (parent[static_cast<size_t>(node)] != node) {
            parent[static_cast<size_t>(node)] = parent[static_cast<size_t>(parent[static_cast<size_t>(node)])];
            node = parent[static_cast<size_t>(node)];
        }
        return node;
    };
    const auto unite = [&](int a, int b) {
        a = findRoot(a);
        b = findRoot(b);
        if (a != b) parent[static_cast<size_t>(std::max(a, b))] = std::min(a, b);
    };
    std::vector<int> stack;
    for (int z = 0; z < count.z; z++) {
        for (int y = 0; y < count.y; y++) {
            for (int x = 0; x < count.x; x++) {
                const BlockContent content = store.content(BlockKey{0, x, y, z});
                BlockInfo& info = blocks[blockIndex(x, y, z)];
                const Int3 origin{x << BLOCK_SHIFT, y << BLOCK_SHIFT, z << BLOCK_SHIFT};
                if (content.sdfIsUniform()) {
                    if (content.sdfValueIfUniform() < 0) continue;
                    info.firstNode = static_cast<int>(nodes.size());
                    info.nodeCount = 1;
                    AirComponent node;
                    node.voxels = static_cast<int64_t>(BLOCK_SAMPLES);
                    node.bounds.include(origin);
                    node.bounds.include(Int3{origin.x + 15, origin.y + 15, origin.z + 15});
                    node.openToSky = origin.y + 15 >= samples.y - 1;
                    nodes.push_back(node);
                    parent.push_back(info.firstNode);
                    continue;
                }
                std::vector<uint16_t> labels(BLOCK_SAMPLES, 0xFFFF);
                const int16_t* sdf = content.data->sdf();
                int localNodes = 0;
                info.firstNode = static_cast<int>(nodes.size());
                for (int i = 0; i < BLOCK_SAMPLES; i++) {
                    if (sdf[i] < 0 || labels[static_cast<size_t>(i)] != 0xFFFF) continue;
                    AirComponent node;
                    labels[static_cast<size_t>(i)] = static_cast<uint16_t>(localNodes);
                    stack.push_back(i);
                    while (!stack.empty()) {
                        const int at = stack.back();
                        stack.pop_back();
                        const int lx = at & 15, ly = (at >> 4) & 15, lz = (at >> 8) & 15;
                        node.voxels++;
                        const Int3 p{origin.x + lx, origin.y + ly, origin.z + lz};
                        node.bounds.include(p);
                        if (p.y >= samples.y - 1) node.openToSky = true;
                        const int neighbours[6] = {lx > 0 ? at - 1 : -1, lx < 15 ? at + 1 : -1, ly > 0 ? at - 16 : -1, ly < 15 ? at + 16 : -1, lz > 0 ? at - 256 : -1, lz < 15 ? at + 256 : -1};
                        for (int neighbour : neighbours) {
                            if (neighbour < 0 || sdf[neighbour] < 0 || labels[static_cast<size_t>(neighbour)] != 0xFFFF) continue;
                            labels[static_cast<size_t>(neighbour)] = static_cast<uint16_t>(localNodes);
                            stack.push_back(neighbour);
                        }
                    }
                    nodes.push_back(node);
                    parent.push_back(info.firstNode + localNodes);
                    localNodes++;
                }
                info.nodeCount = localNodes;
                if (localNodes == 0) {
                    info.firstNode = -1;
                } else {
                    info.labels = static_cast<int>(labelStore.size());
                    labelStore.push_back(std::move(labels));
                }
            }
        }
    }
    const auto nodeAt = [&](const BlockInfo& info, int lx, int ly, int lz) {
        if (info.firstNode < 0) return -1;
        if (info.labels < 0) return info.firstNode;
        const uint16_t label = labelStore[static_cast<size_t>(info.labels)][static_cast<size_t>(sampleIndex(lx, ly, lz))];
        return label == 0xFFFF ? -1 : info.firstNode + label;
    };
    for (int z = 0; z < count.z; z++) {
        for (int y = 0; y < count.y; y++) {
            for (int x = 0; x < count.x; x++) {
                const BlockInfo& info = blocks[blockIndex(x, y, z)];
                if (info.firstNode < 0) continue;
                if (x + 1 < count.x) {
                    const BlockInfo& other = blocks[blockIndex(x + 1, y, z)];
                    if (other.firstNode >= 0) {
                        for (int b = 0; b < 16; b++) {
                            for (int a = 0; a < 16; a++) {
                                const int n0 = nodeAt(info, 15, a, b), n1 = nodeAt(other, 0, a, b);
                                if (n0 >= 0 && n1 >= 0) unite(n0, n1);
                            }
                        }
                    }
                }
                if (y + 1 < count.y) {
                    const BlockInfo& other = blocks[blockIndex(x, y + 1, z)];
                    if (other.firstNode >= 0) {
                        for (int b = 0; b < 16; b++) {
                            for (int a = 0; a < 16; a++) {
                                const int n0 = nodeAt(info, a, 15, b), n1 = nodeAt(other, a, 0, b);
                                if (n0 >= 0 && n1 >= 0) unite(n0, n1);
                            }
                        }
                    }
                }
                if (z + 1 < count.z) {
                    const BlockInfo& other = blocks[blockIndex(x, y, z + 1)];
                    if (other.firstNode >= 0) {
                        for (int b = 0; b < 16; b++) {
                            for (int a = 0; a < 16; a++) {
                                const int n0 = nodeAt(info, a, b, 15), n1 = nodeAt(other, a, b, 0);
                                if (n0 >= 0 && n1 >= 0) unite(n0, n1);
                            }
                        }
                    }
                }
            }
        }
    }
    ConnectivityResult result;
    std::unordered_map<int, int> rootToComponent;
    for (size_t node = 0; node < nodes.size(); node++) {
        const int root = findRoot(static_cast<int>(node));
        const auto found = rootToComponent.try_emplace(root, static_cast<int>(result.components.size()));
        if (found.second) result.components.push_back(AirComponent{});
        AirComponent& component = result.components[static_cast<size_t>(found.first->second)];
        component.voxels += nodes[node].voxels;
        component.bounds.include(nodes[node].bounds);
        component.openToSky = component.openToSky || nodes[node].openToSky;
    }
    const auto componentOf = [&](const Int3& p) {
        if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= samples.x || p.y >= samples.y || p.z >= samples.z) return -1;
        const BlockInfo& info = blocks[blockIndex(p.x >> BLOCK_SHIFT, p.y >> BLOCK_SHIFT, p.z >> BLOCK_SHIFT)];
        const int node = nodeAt(info, p.x & 15, p.y & 15, p.z & 15);
        if (node < 0) return -1;
        return rootToComponent[findRoot(node)];
    };
    if (from) {
        result.fromComponent = componentOf(*from);
        result.fromInAir = result.fromComponent >= 0;
    }
    if (to) {
        result.toComponent = componentOf(*to);
        result.toInAir = result.toComponent >= 0;
    }
    result.connected = from && to && result.fromInAir && result.toInAir && result.fromComponent == result.toComponent;
    return result;
}

BoxStats jasperengine::voxel::boxStats(Store& store, const IntBox& box) {
    BoxStats stats;
    const Int3 samples = store.levelSamples(0);
    IntBox clipped;
    clipped.min = Int3{std::max(box.min.x, 0), std::max(box.min.y, 0), std::max(box.min.z, 0)};
    clipped.max = Int3{std::min(box.max.x, samples.x - 1), std::min(box.max.y, samples.y - 1), std::min(box.max.z, samples.z - 1)};
    if (clipped.empty()) return stats;
    Cursor cursor(store);
    int64_t solid = 0, air = 0;
    bool first = true;
    for (int z = clipped.min.z; z <= clipped.max.z; z++) {
        for (int y = clipped.min.y; y <= clipped.max.y; y++) {
            for (int x = clipped.min.x; x <= clipped.max.x; x++) {
                const int16_t value = cursor.sdf(x, y, z);
                const double distance = decodeSdf(value);
                if (first) {
                    stats.minDistance = stats.maxDistance = distance;
                    first = false;
                } else {
                    stats.minDistance = std::min(stats.minDistance, distance);
                    stats.maxDistance = std::max(stats.maxDistance, distance);
                }
                if (value < 0) solid++; else air++;
                const bool here = value < 0;
                if ((x < samples.x - 1 && (cursor.sdf(x + 1, y, z) < 0) != here) || (y < samples.y - 1 && (cursor.sdf(x, y + 1, z) < 0) != here)
                    || (z < samples.z - 1 && (cursor.sdf(x, y, z + 1) < 0) != here) || (x > 0 && (cursor.sdf(x - 1, y, z) < 0) != here)
                    || (y > 0 && (cursor.sdf(x, y - 1, z) < 0) != here) || (z > 0 && (cursor.sdf(x, y, z - 1) < 0) != here)) {
                    stats.surfaceSamples++;
                }
                const uint16_t material = cursor.material(x, y, z);
                stats.slotsSeen = static_cast<uint16_t>(stats.slotsSeen | (1u << materialSlotA(material)));
                if (materialWeightB(material)) stats.slotsSeen = static_cast<uint16_t>(stats.slotsSeen | (1u << materialSlotB(material)));
            }
        }
    }
    stats.samples = solid + air;
    stats.solidFraction = static_cast<double>(solid) / static_cast<double>(stats.samples);
    stats.airFraction = static_cast<double>(air) / static_cast<double>(stats.samples);
    return stats;
}

VolumeBounds jasperengine::voxel::solidBounds(Store& store) {
    VolumeBounds bounds;
    const Int3 count = store.blockCount(0);
    const Int3 samples = store.levelSamples(0);
    Cursor cursor(store);
    for (int bz = 0; bz < count.z; bz++) {
        for (int by = 0; by < count.y; by++) {
            for (int bx = 0; bx < count.x; bx++) {
                const BlockContent& content = cursor.blockAt(bx, by, bz);
                const Int3 origin{bx << BLOCK_SHIFT, by << BLOCK_SHIFT, bz << BLOCK_SHIFT};
                if (content.sdfIsUniform() && content.sdfValueIfUniform() >= 0) {
                    continue;
                }
                if (content.sdfIsUniform()) {
                    bounds.solid.include(origin);
                    bounds.solid.include(Int3{origin.x + 15, origin.y + 15, origin.z + 15});
                    continue;
                }
                const BlockContent keep = content;
                for (int z = 0; z < BLOCK_SIZE; z++) {
                    for (int y = 0; y < BLOCK_SIZE; y++) {
                        for (int x = 0; x < BLOCK_SIZE; x++) {
                            const bool here = keep.sdfAt(sampleIndex(x, y, z)) < 0;
                            const Int3 p{origin.x + x, origin.y + y, origin.z + z};
                            if (here) bounds.solid.include(p);
                            // Inside the block's faces this reads the same block, so the cursor stays put
                            if ((p.x + 1 < samples.x && (cursor.sdf(p.x + 1, p.y, p.z) < 0) != here)
                                || (p.y + 1 < samples.y && (cursor.sdf(p.x, p.y + 1, p.z) < 0) != here)
                                || (p.z + 1 < samples.z && (cursor.sdf(p.x, p.y, p.z + 1) < 0) != here)) {
                                bounds.surface.include(p);
                            }
                        }
                    }
                }
            }
        }
    }
    return bounds;
}
