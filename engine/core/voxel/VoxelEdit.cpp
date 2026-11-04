// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelEdit.h"

#include "VoxelSample.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

using namespace jasperengine::voxel;

namespace {

    Vec3d add(const Vec3d& a, const Vec3d& b) { return Vec3d{a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3d sub(const Vec3d& a, const Vec3d& b) { return Vec3d{a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec3d mul(const Vec3d& a, double s) { return Vec3d{a.x * s, a.y * s, a.z * s}; }
    double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    double length(const Vec3d& a) { return std::sqrt(dot(a, a)); }

    Vec3d rotate(const Quatd& q, const Vec3d& v) {
        const Vec3d u{q.x, q.y, q.z};
        const double s = q.w;
        const Vec3d cross1{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        const Vec3d cross2{u.y * cross1.z - u.z * cross1.y, u.z * cross1.x - u.x * cross1.z, u.x * cross1.y - u.y * cross1.x};
        return Vec3d{v.x + 2.0 * (s * cross1.x + cross2.x), v.y + 2.0 * (s * cross1.y + cross2.y), v.z + 2.0 * (s * cross1.z + cross2.z)};
    }

    Vec3d rotateInverse(const Quatd& q, const Vec3d& v) {
        return rotate(Quatd{q.w, -q.x, -q.y, -q.z}, v);
    }

    bool finite(double v) { return v - v == 0.0; }
    bool finite(const Vec3d& v) { return finite(v.x) && finite(v.y) && finite(v.z); }
    bool finite(const Quatd& q) { return finite(q.w) && finite(q.x) && finite(q.y) && finite(q.z); }

    // The shape's distance in voxels at a position in voxels
    class ShapeField {
    public:
        ShapeField(const Shape& shape, double voxelSize): shape(shape) {
            const double inverse = 1.0 / voxelSize;
            center = mul(shape.center, inverse);
            radius = shape.radius * inverse;
            half = mul(shape.halfExtents, inverse);
            from = mul(shape.from, inverse);
            to = mul(shape.to, inverse);
            axis = sub(to, from);
            axisLengthSq = dot(axis, axis);
            if (shape.field) {
                scale = shape.scale;
                sampleScale = voxelSize / (shape.field->voxelSize * scale);
                fieldToVoxels = shape.field->voxelSize * scale * inverse / SDF_UNITS_PER_VOXEL;
                outsideDistance = SDF_BAND_VOXELS + 1.0;
            }
        }

        double distance(double x, double y, double z) const {
            const Vec3d p{x, y, z};
            switch (shape.kind) {
                case ShapeKind::Sphere:
                    return length(sub(p, center)) - radius;
                case ShapeKind::Box: {
                    const Vec3d q = rotateInverse(shape.rotation, sub(p, center));
                    const double dx = std::fabs(q.x) - half.x, dy = std::fabs(q.y) - half.y, dz = std::fabs(q.z) - half.z;
                    const Vec3d outside{dx > 0.0 ? dx : 0.0, dy > 0.0 ? dy : 0.0, dz > 0.0 ? dz : 0.0};
                    const double largest = dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
                    return length(outside) + (largest < 0.0 ? largest : 0.0);
                }
                case ShapeKind::Capsule: {
                    const Vec3d pa = sub(p, from);
                    double h = axisLengthSq > 0.0 ? dot(pa, axis) / axisLengthSq : 0.0;
                    h = h < 0.0 ? 0.0 : (h > 1.0 ? 1.0 : h);
                    return length(sub(pa, mul(axis, h))) - radius;
                }
                case ShapeKind::Mesh:
                case ShapeKind::Stamp:
                    return fieldDistance(p);
            }
            return outsideDistance;
        }

        // The width a falloff of 1 reaches inward (R26)
        double characteristicRadius() const {
            switch (shape.kind) {
                case ShapeKind::Sphere:
                case ShapeKind::Capsule:
                    return radius;
                case ShapeKind::Box: {
                    double smallest = half.x;
                    if (half.y < smallest) smallest = half.y;
                    if (half.z < smallest) smallest = half.z;
                    return smallest;
                }
                case ShapeKind::Mesh:
                case ShapeKind::Stamp:
                    return SDF_BAND_VOXELS * 0.5;
            }
            return 1.0;
        }

    private:
        double fieldDistance(const Vec3d& p) const {
            const StampField& field = *shape.field;
            const Vec3d local = rotateInverse(shape.rotation, sub(p, center));
            const double gx = local.x * sampleScale + field.originOffset[0] / field.voxelSize;
            const double gy = local.y * sampleScale + field.originOffset[1] / field.voxelSize;
            const double gz = local.z * sampleScale + field.originOffset[2] / field.voxelSize;
            const int ix = static_cast<int>(std::floor(gx)), iy = static_cast<int>(std::floor(gy)), iz = static_cast<int>(std::floor(gz));
            if (ix < 0 || iy < 0 || iz < 0 || ix + 1 >= field.size.x || iy + 1 >= field.size.y || iz + 1 >= field.size.z) return outsideDistance;
            const double fx = gx - ix, fy = gy - iy, fz = gz - iz;
            const auto at = [&](int x, int y, int z) -> double {
                return field.sdf[static_cast<size_t>(x) + static_cast<size_t>(field.size.x) * (static_cast<size_t>(y) + static_cast<size_t>(field.size.y) * static_cast<size_t>(z))];
            };
            const double c00 = at(ix, iy, iz) * (1.0 - fx) + at(ix + 1, iy, iz) * fx;
            const double c10 = at(ix, iy + 1, iz) * (1.0 - fx) + at(ix + 1, iy + 1, iz) * fx;
            const double c01 = at(ix, iy, iz + 1) * (1.0 - fx) + at(ix + 1, iy, iz + 1) * fx;
            const double c11 = at(ix, iy + 1, iz + 1) * (1.0 - fx) + at(ix + 1, iy + 1, iz + 1) * fx;
            const double c0 = c00 * (1.0 - fy) + c10 * fy;
            const double c1 = c01 * (1.0 - fy) + c11 * fy;
            return (c0 * (1.0 - fz) + c1 * fz) * fieldToVoxels;
        }

        const Shape& shape;
        Vec3d center, half, from, to, axis;
        double radius = 0.0;
        double axisLengthSq = 0.0;
        double scale = 1.0;
        double sampleScale = 1.0;
        double fieldToVoxels = 1.0;
        double outsideDistance = SDF_BAND_VOXELS + 1.0;
    };

    // Polynomial smooth minimum (R26)
    double smoothMin(double a, double b, double k) {
        const double lower = a < b ? a : b;
        if (k <= 0.0) return lower;
        const double difference = a > b ? a - b : b - a;
        const double h = k > difference ? (k - difference) / k : 0.0;
        return lower - h * h * k * 0.25;
    }

    int floorToInt(double v) { return static_cast<int>(std::floor(v)); }
    int ceilToInt(double v) { return static_cast<int>(std::ceil(v)); }

    // The box of metres a shape reaches, unclipped
    void shapeExtent(const Shape& shape, Vec3d& low, Vec3d& high) {
        switch (shape.kind) {
            case ShapeKind::Sphere:
                low = sub(shape.center, Vec3d{shape.radius, shape.radius, shape.radius});
                high = add(shape.center, Vec3d{shape.radius, shape.radius, shape.radius});
                break;
            case ShapeKind::Capsule:
                low = Vec3d{std::min(shape.from.x, shape.to.x) - shape.radius, std::min(shape.from.y, shape.to.y) - shape.radius, std::min(shape.from.z, shape.to.z) - shape.radius};
                high = Vec3d{std::max(shape.from.x, shape.to.x) + shape.radius, std::max(shape.from.y, shape.to.y) + shape.radius, std::max(shape.from.z, shape.to.z) + shape.radius};
                break;
            case ShapeKind::Box:
            case ShapeKind::Mesh:
            case ShapeKind::Stamp: {
                Vec3d cornerLow, cornerHigh;
                if (shape.kind == ShapeKind::Box) {
                    cornerLow = mul(shape.halfExtents, -1.0);
                    cornerHigh = shape.halfExtents;
                } else if (shape.field) {
                    const StampField& field = *shape.field;
                    cornerLow = Vec3d{-field.originOffset[0] * shape.scale, -field.originOffset[1] * shape.scale, -field.originOffset[2] * shape.scale};
                    cornerHigh = Vec3d{((field.size.x - 1) * field.voxelSize - field.originOffset[0]) * shape.scale,
                                       ((field.size.y - 1) * field.voxelSize - field.originOffset[1]) * shape.scale,
                                       ((field.size.z - 1) * field.voxelSize - field.originOffset[2]) * shape.scale};
                }
                bool first = true;
                for (int corner = 0; corner < 8; corner++) {
                    const Vec3d local{(corner & 1) ? cornerHigh.x : cornerLow.x, (corner & 2) ? cornerHigh.y : cornerLow.y, (corner & 4) ? cornerHigh.z : cornerLow.z};
                    const Vec3d world = add(rotate(shape.rotation, local), shape.center);
                    if (first) {
                        low = high = world;
                        first = false;
                    } else {
                        low = Vec3d{std::min(low.x, world.x), std::min(low.y, world.y), std::min(low.z, world.z)};
                        high = Vec3d{std::max(high.x, world.x), std::max(high.y, world.y), std::max(high.z, world.z)};
                    }
                }
                break;
            }
        }
    }

    IntBox boxInVoxels(const Shape& shape, double voxelSize, double expandVoxels) {
        Vec3d low, high;
        shapeExtent(shape, low, high);
        IntBox box;
        box.min = Int3{floorToInt(low.x / voxelSize - expandVoxels), floorToInt(low.y / voxelSize - expandVoxels), floorToInt(low.z / voxelSize - expandVoxels)};
        box.max = Int3{ceilToInt(high.x / voxelSize + expandVoxels), ceilToInt(high.y / voxelSize + expandVoxels), ceilToInt(high.z / voxelSize + expandVoxels)};
        return box;
    }

    IntBox clipBox(const IntBox& box, const IntBox& limit) {
        IntBox out;
        out.min = Int3{std::max(box.min.x, limit.min.x), std::max(box.min.y, limit.min.y), std::max(box.min.z, limit.min.z)};
        out.max = Int3{std::min(box.max.x, limit.max.x), std::min(box.max.y, limit.max.y), std::min(box.max.z, limit.max.z)};
        return out;
    }

    IntBox volumeBox(const VolumeDesc& desc, int margin) {
        IntBox box;
        box.min = Int3{margin, margin, margin};
        box.max = Int3{desc.dimensions.x - 1 - margin, desc.dimensions.y - 1 - margin, desc.dimensions.z - 1 - margin};
        return box;
    }

    int64_t boxSamples(const IntBox& box) {
        if (box.empty()) return 0;
        return static_cast<int64_t>(box.max.x - box.min.x + 1) * (box.max.y - box.min.y + 1) * (box.max.z - box.min.z + 1);
    }

    bool insideBox(const IntBox& box, int x, int y, int z) {
        return x >= box.min.x && x <= box.max.x && y >= box.min.y && y <= box.max.y && z >= box.min.z && z <= box.max.z;
    }

    // Absolute: at factor 1 the sample ends with exactly `weight` of the slot (R26)
    uint16_t paintedSample(uint16_t sample, int slot, double weight, double factor) {
        const int a = materialSlotA(sample), b = materialSlotB(sample), wb = materialWeightB(sample);
        int current = 0;
        if (a == slot) current += 255 - wb;
        if (b == slot && wb) current += wb;
        const double target = current / 255.0 + (weight - current / 255.0) * factor;
        int other = slot;
        if (a != slot) {
            other = a;
            if (wb > 127 && b != slot) other = b;
        } else if (wb && b != slot) {
            other = b;
        }
        const int amount = static_cast<int>(std::floor(target * 255.0 + 0.5));
        if (amount >= 255 || other == slot) return packMaterial(slot, slot, 0);
        if (amount <= 0) return packMaterial(other, other, 0);
        return packMaterial(other, slot, amount);
    }

    double falloffFactor(double inside, double width) {
        if (width <= 0.0) return 1.0;
        if (inside >= width) return 1.0;
        const double t = inside / width;
        return t * t * (3.0 - 2.0 * t);
    }

    // Deterministic per-position hash for debris selection
    uint32_t mixPosition(uint64_t seed, int x, int y, int z) {
        uint64_t h = seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(x)) * 0xFF51AFD7ED558CCDull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(y)) * 0xC4CEB9FE1A85EC53ull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(z)) * 0x9FB21C651E98DF25ull;
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        return static_cast<uint32_t>(h);
    }

    // R31: a stratified lattice over the shape's box, one best sample per cell
    class DebrisLattice {
    public:
        void begin(int count, uint64_t seed, const IntBox& box) {
            wanted = count;
            hashSeed = seed;
            bounds = box;
            if (wanted <= 0) return;
            side = 1;
            while (side * side * side < wanted) side++;
        }
        void offer(int x, int y, int z) {
            if (wanted <= 0) return;
            if (cells.empty()) cells.resize(static_cast<size_t>(side * side * side));
            const int cx = cellOf(x, bounds.min.x, bounds.max.x);
            const int cy = cellOf(y, bounds.min.y, bounds.max.y);
            const int cz = cellOf(z, bounds.min.z, bounds.max.z);
            Cell& cell = cells[static_cast<size_t>(cx + side * (cy + side * cz))];
            const uint32_t score = mixPosition(hashSeed, x, y, z);
            if (!cell.used || score < cell.score || (score == cell.score && (z < cell.position.z || y < cell.position.y || x < cell.position.x))) {
                cell.used = true;
                cell.score = score;
                cell.position = Int3{x, y, z};
            }
        }
        void collect(int count, double voxelSize, std::vector<Vec3d>& out) const {
            for (const Cell& cell : cells) {
                if (static_cast<int>(out.size()) >= count) break;
                if (cell.used) out.push_back(Vec3d{cell.position.x * voxelSize, cell.position.y * voxelSize, cell.position.z * voxelSize});
            }
        }

    private:
        struct Cell {
            bool used = false;
            uint32_t score = 0;
            Int3 position;
        };
        int cellOf(int value, int low, int high) const {
            const int64_t span = static_cast<int64_t>(high) - low + 1;
            int cell = static_cast<int>((static_cast<int64_t>(value - low) * side) / span);
            return cell < 0 ? 0 : (cell >= side ? side - 1 : cell);
        }
        int wanted = 0;
        int side = 1;
        uint64_t hashSeed = 0;
        IntBox bounds;
        std::vector<Cell> cells;
    };

    // The call's writes, committed together or not at all. std::map allocates nothing until written.
    class Transaction {
    public:
        explicit Transaction(const Store& store): store(store) {}

        BlockContent read(const BlockKey& key) const {
            const auto found = pending.find(key);
            if (found != pending.end()) return found->second;
            return store.readBlock(key);
        }
        void write(const BlockKey& key, BlockContent content) {
            pending[key] = std::move(content);
        }
        int16_t sdfAtLevel0(int x, int y, int z) {
            const BlockKey key{0, x >> BLOCK_SHIFT, y >> BLOCK_SHIFT, z >> BLOCK_SHIFT};
            if (key != cursorKey) {
                cursorKey = key;
                cursor = read(key);
            }
            return cursor.sdfAt(sampleIndex(x & 15, y & 15, z & 15));
        }
        uint16_t materialAtLevel0(int x, int y, int z) {
            const BlockKey key{0, x >> BLOCK_SHIFT, y >> BLOCK_SHIFT, z >> BLOCK_SHIFT};
            if (key != cursorKey) {
                cursorKey = key;
                cursor = read(key);
            }
            return cursor.materialAt(sampleIndex(x & 15, y & 15, z & 15));
        }
        void forgetCursor() {
            cursorKey = BlockKey{-1, 0, 0, 0};
            cursor = BlockContent();
        }

        const Store& store;
        std::map<BlockKey, BlockContent> pending;

    private:
        BlockKey cursorKey{-1, 0, 0, 0};
        BlockContent cursor;
    };

    void noteRemovedOrAdded(const Store& store, EditResult& result, DebrisLattice& debris, int x, int y, int z, int16_t before, int16_t after, uint16_t material) {
        if ((before < 0) == (after < 0)) return;
        const double voxelSize = store.desc().voxelSize;
        const double cubic = voxelSize * voxelSize * voxelSize;
        result.surfaceVoxelsChanged++;
        if (before < 0) {
            result.removedVolume += cubic;
            const int dominant = materialWeightB(material) > 127 ? materialSlotB(material) : materialSlotA(material);
            result.removedByMaterial[static_cast<size_t>(dominant)] += cubic;
            debris.offer(x, y, z);
        } else {
            result.addedVolume += cubic;
        }
    }

    // dig, add and paint: each block's new samples depend only on its own and the shape
    void applyPointwise(Transaction& transaction, const EditOp& op, const ShapeField& field, const IntBox& box, const IntBox& editable, EditResult& result, DebrisLattice& debris) {
        const Store& store = transaction.store;
        const double voxelSize = store.desc().voxelSize;
        const double k = op.blend / voxelSize;
        const bool writesMaterial = op.material >= 0;
        const double width = op.falloff * field.characteristicRadius();
        for (int bz = box.min.z >> BLOCK_SHIFT; bz <= (box.max.z >> BLOCK_SHIFT); bz++) {
            for (int by = box.min.y >> BLOCK_SHIFT; by <= (box.max.y >> BLOCK_SHIFT); by++) {
                for (int bx = box.min.x >> BLOCK_SHIFT; bx <= (box.max.x >> BLOCK_SHIFT); bx++) {
                    const BlockKey key{0, bx, by, bz};
                    const BlockContent old = transaction.read(key);
                    // R30: nothing to do in a block already all of what the op makes
                    if (old.sdfIsUniform()) {
                        const int16_t value = old.sdfValueIfUniform();
                        if (op.kind == EditKind::Dig && value >= SDF_MAX) continue;
                        if (op.kind == EditKind::Add && value <= -SDF_MAX && !writesMaterial) continue;
                    }
                    int16_t sdf[BLOCK_SAMPLES];
                    uint16_t material[BLOCK_SAMPLES];
                    bool loaded = false;
                    bool changed = false;
                    const int ox = bx << BLOCK_SHIFT, oy = by << BLOCK_SHIFT, oz = bz << BLOCK_SHIFT;
                    const int z0 = std::max(box.min.z, oz) - oz, z1 = std::min(box.max.z, oz + BLOCK_SIZE - 1) - oz;
                    const int y0 = std::max(box.min.y, oy) - oy, y1 = std::min(box.max.y, oy + BLOCK_SIZE - 1) - oy;
                    const int x0 = std::max(box.min.x, ox) - ox, x1 = std::min(box.max.x, ox + BLOCK_SIZE - 1) - ox;
                    for (int z = z0; z <= z1; z++) {
                        for (int y = y0; y <= y1; y++) {
                            for (int x = x0; x <= x1; x++) {
                                const int index = sampleIndex(x, y, z);
                                const int px = ox + x, py = oy + y, pz = oz + z;
                                const double s = field.distance(px, py, pz);
                                const int16_t beforeSdf = old.sdfAt(index);
                                const uint16_t beforeMaterial = old.materialAt(index);
                                int16_t afterSdf = beforeSdf;
                                uint16_t afterMaterial = beforeMaterial;
                                switch (op.kind) {
                                    case EditKind::Dig:
                                        afterSdf = encodeSdf(-smoothMin(-decodeSdf(beforeSdf), s, k));
                                        break;
                                    case EditKind::Add:
                                        afterSdf = encodeSdf(smoothMin(decodeSdf(beforeSdf), s, k));
                                        if (writesMaterial && afterSdf != beforeSdf) afterMaterial = paintedSample(beforeMaterial, op.material, op.weight, 1.0);
                                        break;
                                    case EditKind::Paint:
                                        if (s <= 0.0) afterMaterial = paintedSample(beforeMaterial, op.material, op.weight, falloffFactor(-s, width));
                                        break;
                                    case EditKind::Smooth:
                                        break;
                                }
                                if (afterSdf == beforeSdf && afterMaterial == beforeMaterial) continue;
                                if (!insideBox(editable, px, py, pz)) {
                                    result.clampedVoxels++;
                                    continue;
                                }
                                if (!loaded) {
                                    for (int i = 0; i < BLOCK_SAMPLES; i++) {
                                        sdf[i] = old.sdfAt(i);
                                        material[i] = old.materialAt(i);
                                    }
                                    loaded = true;
                                }
                                sdf[index] = afterSdf;
                                material[index] = afterMaterial;
                                changed = true;
                                result.voxelsChanged++;
                                result.changedVoxels.include(Int3{px, py, pz});
                                noteRemovedOrAdded(store, result, debris, px, py, pz, beforeSdf, afterSdf, beforeMaterial);
                            }
                        }
                    }
                    if (changed) transaction.write(key, makeContent(sdf, material));
                }
            }
        }
    }

    // smooth: a Jacobi blur over a scratch of the region. Every iteration reads the previous one
    // only, so the output does not depend on any order.
    void applySmooth(Transaction& transaction, const EditOp& op, const ShapeField& field, const IntBox& box, const IntBox& editable, EditResult& result, DebrisLattice& debris) {
        const Store& store = transaction.store;
        const VolumeDesc& desc = store.desc();
        const IntBox region = clipBox(IntBox{Int3{box.min.x - 1, box.min.y - 1, box.min.z - 1}, Int3{box.max.x + 1, box.max.y + 1, box.max.z + 1}}, volumeBox(desc, 0));
        const int sizeX = region.max.x - region.min.x + 1, sizeY = region.max.y - region.min.y + 1;
        const size_t count = static_cast<size_t>(boxSamples(region));
        std::vector<double> current(count), next(count);
        std::vector<float> factors(count, 0.0f);
        const auto at = [&](int x, int y, int z) {
            return static_cast<size_t>(x - region.min.x) + static_cast<size_t>(sizeX) * (static_cast<size_t>(y - region.min.y) + static_cast<size_t>(sizeY) * static_cast<size_t>(z - region.min.z));
        };
        const double width = op.falloff * field.characteristicRadius();
        for (int z = region.min.z; z <= region.max.z; z++) {
            for (int y = region.min.y; y <= region.max.y; y++) {
                for (int x = region.min.x; x <= region.max.x; x++) {
                    current[at(x, y, z)] = decodeSdf(transaction.sdfAtLevel0(x, y, z));
                    if (insideBox(box, x, y, z)) {
                        const double s = field.distance(x, y, z);
                        if (s <= 0.0) factors[at(x, y, z)] = static_cast<float>(falloffFactor(-s, width));
                    }
                }
            }
        }
        const double amount = op.amount;
        for (int iteration = 0; iteration < op.iterations; iteration++) {
            next = current;
            for (int z = box.min.z; z <= box.max.z; z++) {
                for (int y = box.min.y; y <= box.max.y; y++) {
                    for (int x = box.min.x; x <= box.max.x; x++) {
                        const size_t index = at(x, y, z);
                        const double factor = factors[index];
                        if (factor <= 0.0) continue;
                        const double xl = x > region.min.x ? current[at(x - 1, y, z)] : current[index];
                        const double xh = x < region.max.x ? current[at(x + 1, y, z)] : current[index];
                        const double yl = y > region.min.y ? current[at(x, y - 1, z)] : current[index];
                        const double yh = y < region.max.y ? current[at(x, y + 1, z)] : current[index];
                        const double zl = z > region.min.z ? current[at(x, y, z - 1)] : current[index];
                        const double zh = z < region.max.z ? current[at(x, y, z + 1)] : current[index];
                        const double average = (xl + xh + yl + yh + zl + zh) / 6.0;
                        next[index] = current[index] + (average - current[index]) * amount * factor;
                    }
                }
            }
            current.swap(next);
        }
        transaction.forgetCursor();
        for (int bz = box.min.z >> BLOCK_SHIFT; bz <= (box.max.z >> BLOCK_SHIFT); bz++) {
            for (int by = box.min.y >> BLOCK_SHIFT; by <= (box.max.y >> BLOCK_SHIFT); by++) {
                for (int bx = box.min.x >> BLOCK_SHIFT; bx <= (box.max.x >> BLOCK_SHIFT); bx++) {
                    const BlockKey key{0, bx, by, bz};
                    const BlockContent old = transaction.read(key);
                    int16_t sdf[BLOCK_SAMPLES];
                    uint16_t material[BLOCK_SAMPLES];
                    bool loaded = false;
                    bool changed = false;
                    const int ox = bx << BLOCK_SHIFT, oy = by << BLOCK_SHIFT, oz = bz << BLOCK_SHIFT;
                    for (int z = std::max(box.min.z, oz); z <= std::min(box.max.z, oz + BLOCK_SIZE - 1); z++) {
                        for (int y = std::max(box.min.y, oy); y <= std::min(box.max.y, oy + BLOCK_SIZE - 1); y++) {
                            for (int x = std::max(box.min.x, ox); x <= std::min(box.max.x, ox + BLOCK_SIZE - 1); x++) {
                                const int index = sampleIndex(x - ox, y - oy, z - oz);
                                const int16_t before = old.sdfAt(index);
                                const int16_t after = encodeSdf(current[at(x, y, z)]);
                                if (after == before) continue;
                                if (!insideBox(editable, x, y, z)) {
                                    result.clampedVoxels++;
                                    continue;
                                }
                                if (!loaded) {
                                    for (int i = 0; i < BLOCK_SAMPLES; i++) {
                                        sdf[i] = old.sdfAt(i);
                                        material[i] = old.materialAt(i);
                                    }
                                    loaded = true;
                                }
                                sdf[index] = after;
                                changed = true;
                                result.voxelsChanged++;
                                result.changedVoxels.include(Int3{x, y, z});
                                noteRemovedOrAdded(store, result, debris, x, y, z, before, after, material[index]);
                            }
                        }
                    }
                    if (changed) transaction.write(key, makeContent(sdf, material));
                }
            }
        }
    }

    // R5: every coarser level gets the touched positions by copying, never by averaging
    void propagateLevels(Transaction& transaction, const IntBox& changed) {
        const Store& store = transaction.store;
        transaction.forgetCursor();
        for (int level = 1; level < store.levels(); level++) {
            const int spacing = 1 << level;
            const int shift = BLOCK_SHIFT + level;
            const int step = spacing;
            // The first position of this level's grid inside the box
            const Int3 first{((changed.min.x + spacing - 1) >> level) << level, ((changed.min.y + spacing - 1) >> level) << level, ((changed.min.z + spacing - 1) >> level) << level};
            if (first.x > changed.max.x || first.y > changed.max.y || first.z > changed.max.z) continue;
            for (int bz = first.z >> shift; bz <= (changed.max.z >> shift); bz++) {
                for (int by = first.y >> shift; by <= (changed.max.y >> shift); by++) {
                    for (int bx = first.x >> shift; bx <= (changed.max.x >> shift); bx++) {
                        const BlockKey key{level, bx, by, bz};
                        const BlockContent old = transaction.read(key);
                        int16_t sdf[BLOCK_SAMPLES];
                        uint16_t material[BLOCK_SAMPLES];
                        bool loaded = false;
                        bool changedBlock = false;
                        const int ox = bx << shift, oy = by << shift, oz = bz << shift;
                        const int span = BLOCK_SIZE << level;
                        for (int z = std::max(first.z, oz); z <= changed.max.z && z < oz + span; z += step) {
                            for (int y = std::max(first.y, oy); y <= changed.max.y && y < oy + span; y += step) {
                                for (int x = std::max(first.x, ox); x <= changed.max.x && x < ox + span; x += step) {
                                    const int index = sampleIndex((x - ox) >> level, (y - oy) >> level, (z - oz) >> level);
                                    const int16_t valueSdf = transaction.sdfAtLevel0(x, y, z);
                                    const uint16_t valueMaterial = transaction.materialAtLevel0(x, y, z);
                                    if (old.sdfAt(index) == valueSdf && old.materialAt(index) == valueMaterial) continue;
                                    if (!loaded) {
                                        for (int i = 0; i < BLOCK_SAMPLES; i++) {
                                            sdf[i] = old.sdfAt(i);
                                            material[i] = old.materialAt(i);
                                        }
                                        loaded = true;
                                    }
                                    sdf[index] = valueSdf;
                                    material[index] = valueMaterial;
                                    changedBlock = true;
                                }
                            }
                        }
                        if (changedBlock) transaction.write(key, makeContent(sdf, material));
                    }
                }
            }
        }
    }

    EditError validateOp(const Store& store, const EditOp& op, const EditOptions& options, int64_t& budgetUsed) {
        const Shape& shape = op.shape;
        if (!finite(op.blend) || op.blend < 0.0 || !finite(op.weight) || op.weight < 0.0 || op.weight > 1.0 || !finite(op.falloff) || op.falloff < 0.0
            || op.falloff > 1.0 || !finite(op.amount) || op.amount < 0.0 || op.amount > 1.0) {
            return EditError::InvalidShape;
        }
        if (op.kind == EditKind::Smooth && (op.iterations < 1 || op.iterations > 8)) return EditError::InvalidShape;
        if (op.material < -1 || op.material >= options.slotCount) return EditError::InvalidMaterial;
        if (op.kind == EditKind::Paint && op.material < 0) return EditError::InvalidMaterial;
        if (!finite(shape.center) || !finite(shape.rotation) || !finite(shape.from) || !finite(shape.to) || !finite(shape.halfExtents) || !finite(shape.radius) || !finite(shape.scale)) {
            return EditError::InvalidShape;
        }
        switch (shape.kind) {
            case ShapeKind::Sphere:
            case ShapeKind::Capsule:
                if (shape.radius <= 0.0) return EditError::InvalidShape;
                break;
            case ShapeKind::Box:
                if (shape.halfExtents.x <= 0.0 || shape.halfExtents.y <= 0.0 || shape.halfExtents.z <= 0.0) return EditError::InvalidShape;
                break;
            case ShapeKind::Mesh:
            case ShapeKind::Stamp:
                if (!shape.field) return EditError::MissingAsset;
                if (shape.scale <= 0.0) return EditError::InvalidShape;
                break;
        }
        const VolumeDesc& desc = store.desc();
        const IntBox unclipped = boxInVoxels(shape, desc.voxelSize, 0.0);
        const int margin = desc.bounds == BoundsMode::Closed ? 1 : 2;
        if (clipBox(unclipped, volumeBox(desc, margin)).empty()) return EditError::OutOfBounds;
        if (options.maxVoxels > 0) {
            const int64_t samples = boxSamples(clipBox(unclipped, volumeBox(desc, 0)));
            const int64_t cost = (shape.kind == ShapeKind::Mesh && shape.field) ? samples * std::max(shape.field->triangleCount, 1) : samples;
            budgetUsed += cost;
            if (budgetUsed > options.maxVoxels) return EditError::BudgetExceeded;
        }
        return EditError::None;
    }

    void accumulate(EditResult& into, const EditResult& op) {
        into.changedVoxels.include(op.changedVoxels);
        into.voxelsChanged += op.voxelsChanged;
        into.surfaceVoxelsChanged += op.surfaceVoxelsChanged;
        into.clampedVoxels += op.clampedVoxels;
        into.removedVolume += op.removedVolume;
        into.addedVolume += op.addedVolume;
        for (size_t i = 0; i < op.removedByMaterial.size(); i++) into.removedByMaterial[i] += op.removedByMaterial[i];
    }

}

const char* jasperengine::voxel::editErrorCode(EditError error) {
    switch (error) {
        case EditError::None: return "";
        case EditError::OutOfBounds: return "out_of_bounds";
        case EditError::InvalidShape: return "invalid_shape";
        case EditError::InvalidMaterial: return "invalid_material";
        case EditError::MissingAsset: return "missing_asset";
        case EditError::BudgetExceeded: return "budget_exceeded";
        case EditError::NotLoaded: return "not_loaded";
    }
    return "";
}

IntBox jasperengine::voxel::shapeSampleBox(const VolumeDesc& desc, const Shape& shape) {
    return boxInVoxels(shape, desc.voxelSize, 0.0);
}

EditResult jasperengine::voxel::applyEdit(Store& store, std::span<const EditOp> ops, const EditOptions& options) {
    EditResult result;
    // R27: all or nothing, before any write
    int64_t budgetUsed = 0;
    for (const EditOp& op : ops) {
        const EditError error = validateOp(store, op, options, budgetUsed);
        if (error != EditError::None) {
            result.ok = false;
            result.error = error;
            return result;
        }
    }

    const VolumeDesc& desc = store.desc();
    const IntBox editable = volumeBox(desc, desc.bounds == BoundsMode::Closed ? 1 : 2);
    const IntBox whole = volumeBox(desc, 0);
    Transaction transaction(store);
    DebrisLattice debris;
    if (options.debrisCount > 0 && ops.size() == 1) {
        debris.begin(options.debrisCount, options.debrisSeed, clipBox(boxInVoxels(ops[0].shape, desc.voxelSize, 0.0), whole));
    } else if (options.debrisCount > 0) {
        IntBox all;
        for (const EditOp& op : ops) all.include(clipBox(boxInVoxels(op.shape, desc.voxelSize, 0.0), whole));
        debris.begin(options.debrisCount, options.debrisSeed, all);
    }

    for (const EditOp& op : ops) {
        const ShapeField field(op.shape, desc.voxelSize);
        double expand = 0.0;
        if (op.kind == EditKind::Dig || op.kind == EditKind::Add) expand = SDF_BAND_VOXELS + op.blend / desc.voxelSize + 1.0;
        const IntBox box = clipBox(boxInVoxels(op.shape, desc.voxelSize, expand), whole);
        EditResult opResult;
        if (!box.empty()) {
            if (op.kind == EditKind::Smooth) {
                applySmooth(transaction, op, field, box, editable, opResult, debris);
            } else {
                applyPointwise(transaction, op, field, box, editable, opResult, debris);
            }
        }
        if (!opResult.changedVoxels.empty()) propagateLevels(transaction, opResult.changedVoxels);
        accumulate(result, opResult);
        if (ops.size() > 1) result.perOp.push_back(opResult);
    }

    int64_t memoryDelta = 0;
    for (const auto& item : transaction.pending) {
        const BlockContent before = store.readBlock(item.first);
        const Slot& slot = store.slot(item.first);
        const int64_t beforeBytes = slot.kind() == Slot::Hot ? static_cast<int64_t>(before.payloadBytes()) : 0;
        const int64_t afterBytes = static_cast<int64_t>(item.second.payloadBytes());
        memoryDelta += afterBytes - beforeBytes;
        result.blocksTouched++;
        if (before.isFullyUniform() && !item.second.isFullyUniform()) result.blocksAllocated++;
        if (!before.isFullyUniform() && item.second.isFullyUniform()) result.blocksCollapsed++;
    }
    result.memoryDelta = memoryDelta;

    // R7: refused before any write, after whatever can be evicted has been. A dry run answers the
    // same question without evicting.
    if (memoryDelta > 0 && store.budget()) {
        const bool fits = options.dryRun ? store.wouldFit(memoryDelta) : store.makeRoom(memoryDelta);
        if (!fits) {
            EditResult refused;
            refused.ok = false;
            refused.error = EditError::BudgetExceeded;
            refused.memoryDelta = memoryDelta;
            return refused;
        }
    }

    debris.collect(options.debrisCount, desc.voxelSize, result.debrisPoints);

    if (options.hash) {
        result.hasHash = true;
        result.contentHashAfter = contentHash(store, [&transaction](const BlockKey& key) { return transaction.read(key); });
    }
    if (options.dryRun) return result;

    for (auto& item : transaction.pending) {
        if (options.captureDelta) {
            SlotChange change;
            change.key = item.first;
            change.before = store.slot(item.first);
            store.writeBlock(item.first, item.second);
            change.after = store.slot(item.first);
            result.delta.push_back(std::move(change));
        } else {
            store.writeBlock(item.first, item.second);
        }
    }
    if (!transaction.pending.empty()) store.bumpVersion();
    return result;
}

void jasperengine::voxel::applyDelta(Store& store, const std::vector<SlotChange>& delta, bool undo) {
    for (const SlotChange& change : delta) {
        store.applySlot(change.key, undo ? change.before : change.after);
    }
    if (!delta.empty()) store.bumpVersion();
}
