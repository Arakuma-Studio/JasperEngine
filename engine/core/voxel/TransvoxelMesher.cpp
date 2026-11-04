// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TransvoxelMesher.h"

#include "TransvoxelTables.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

using namespace jasperengine::voxel;

namespace {

    // The fine window: samples -1..17 of the block, gradients included
    constexpr int WINDOW = BLOCK_SIZE + 3;
    constexpr int WINDOW_SAMPLES = WINDOW * WINDOW * WINDOW;
    // The coarse window of masked sides: level-(k+1) samples -1..9 relative to the block's corner
    constexpr int COARSE_WINDOW = BLOCK_SIZE / 2 + 3;
    constexpr int COARSE_SAMPLES = COARSE_WINDOW * COARSE_WINDOW * COARSE_WINDOW;
    constexpr int MATERIAL_WINDOW = BLOCK_SIZE + 1;
    constexpr int MATERIAL_SAMPLES = MATERIAL_WINDOW * MATERIAL_WINDOW * MATERIAL_WINDOW;
    // Grid edges of the 17^3 lattice, three per sample
    constexpr int EDGE_COUNT = MATERIAL_SAMPLES * 3;
    constexpr int LOW_RES_EDGES_PER_SIDE = 2 * (BLOCK_SIZE / 2) * (BLOCK_SIZE / 2 + 1);

    inline int windowIndex(int x, int y, int z) { return (x + 1) + WINDOW * ((y + 1) + WINDOW * (z + 1)); }
    inline int coarseIndex(int x, int y, int z) { return (x + 1) + COARSE_WINDOW * ((y + 1) + COARSE_WINDOW * (z + 1)); }
    inline int materialIndex(int x, int y, int z) { return x + MATERIAL_WINDOW * (y + MATERIAL_WINDOW * z); }
    inline int edgeIndex(int x, int y, int z, int axis) { return (x + MATERIAL_WINDOW * (y + MATERIAL_WINDOW * z)) * 3 + axis; }

    // A cell's material set (R13): up to 4 slots, the code is the attribute's value
    struct MaterialSet {
        uint16_t code = 0;
        uint8_t slots[4] = {0, 0, 0, 0};
        int count = 1;
    };

    // Thread-local: mesh jobs allocate nothing per block once warm
    struct Scratch {
        int16_t sdf[WINDOW_SAMPLES];
        int16_t coarse[COARSE_SAMPLES];
        uint16_t material[MATERIAL_SAMPLES];
        uint16_t coarseMaterial[COARSE_SAMPLES];
        uint32_t edgeStamp[EDGE_COUNT];
        int32_t edgeFirst[EDGE_COUNT];
        uint32_t lowStamp[SIDE_COUNT * LOW_RES_EDGES_PER_SIDE];
        int32_t lowFirst[SIDE_COUNT * LOW_RES_EDGES_PER_SIDE];
        std::vector<int32_t> nextOnEdge;
        uint32_t stamp = 0;
    };

    Scratch& scratch() {
        thread_local std::unique_ptr<Scratch> threadScratch;
        if (!threadScratch) threadScratch.reset(new Scratch());
        return *threadScratch;
    }

    // Which face a transition cell's (u, v) lies on. u x v points into the block on all six sides, so one
    // winding convention holds for all.
    struct SideAxes {
        int normalAxis;
        int uAxis;
        int vAxis;
        bool high;
    };
    constexpr SideAxes SIDE_AXES[SIDE_COUNT] = {
        {0, 1, 2, false},   // -x: y x z = +x
        {0, 2, 1, true},    // +x: z x y = -x
        {1, 2, 0, false},   // -y: z x x = +y
        {1, 0, 2, true},    // +y: x x z = -y
        {2, 0, 1, false},   // -z: x x y = +z
        {2, 1, 0, true},    // +z: y x x = -z
    };

    // Dissertation's figure 4.17: the 9-bit case runs around the 3x3 face and ends at its centre
    constexpr int TRANSITION_CASE_BIT[9] = {0, 1, 2, 7, 8, 3, 6, 5, 4};

    class Mesher {
    public:
        Mesher(const ReadView& view, const MeshRequest& request, MeshOut& out, Scratch& work)
            : view(view), request(request), out(out), work(work), volume(view.desc()), level(request.key.level) {
            cellFixed = FIXED_ONE << level;
            blockFixed = cellFixed * BLOCK_SIZE;
            originSamples = Int3{request.key.x * BLOCK_SIZE, request.key.y * BLOCK_SIZE, request.key.z * BLOCK_SIZE};
            origin[0] = originSamples.x * cellFixed;
            origin[1] = originSamples.y * cellFixed;
            origin[2] = originSamples.z * cellFixed;
            full = request.mode == MeshMode::Full;
            // A side with no coarser level to read cannot be masked
            mask = (level + 1 < volume.levels) ? request.sideMask : 0;
        }

        void run(const MeshOptions& options) {
            out.clear();
            view.fetchSdf(level, Int3{originSamples.x - 1, originSamples.y - 1, originSamples.z - 1}, Int3{WINDOW, WINDOW, WINDOW}, work.sdf);
            if (!anyCrossing()) return;
            if (mask) {
                coarseOrigin = Int3{(originSamples.x >> 1), (originSamples.y >> 1), (originSamples.z >> 1)};
                view.fetchSdf(level + 1, Int3{coarseOrigin.x - 1, coarseOrigin.y - 1, coarseOrigin.z - 1}, Int3{COARSE_WINDOW, COARSE_WINDOW, COARSE_WINDOW}, work.coarse);
            }
            if (full) prepareMaterials();
            if (!build(options.maxVerticesPerCellSets)) {
                // R14: one set for the whole block duplicates nothing, so it always fits 16-bit indices
                blockSet = topSlotsOfBlock();
                useBlockSet = true;
                out.usedBlockMaterialSet = true;
                build(std::numeric_limits<int>::max());
            }
            finish();
        }

    private:
        bool anyCrossing() const {
            // The block's cells read samples 0..16
            const bool firstSolid = work.sdf[windowIndex(0, 0, 0)] < 0;
            for (int z = 0; z <= BLOCK_SIZE; z++) {
                for (int y = 0; y <= BLOCK_SIZE; y++) {
                    const int16_t* row = work.sdf + windowIndex(0, y, z);
                    for (int x = 0; x <= BLOCK_SIZE; x++) {
                        if ((row[x] < 0) != firstSolid) return true;
                    }
                }
            }
            return false;
        }

        void prepareMaterials() {
            uint16_t single = 0;
            singleMaterial = view.windowOneMaterial(single);
            if (singleMaterial) {
                singleSet = setFromSample(single);
                return;
            }
            view.fetchMaterial(level, originSamples, Int3{MATERIAL_WINDOW, MATERIAL_WINDOW, MATERIAL_WINDOW}, work.material);
            if (mask) {
                view.fetchMaterial(level + 1, Int3{coarseOrigin.x - 1, coarseOrigin.y - 1, coarseOrigin.z - 1},
                    Int3{COARSE_WINDOW, COARSE_WINDOW, COARSE_WINDOW}, work.coarseMaterial);
            }
        }

        static MaterialSet setFromSample(uint16_t sample) {
            int weights[MAX_SLOTS] = {};
            addWeights(weights, sample);
            return setFromWeights(weights);
        }

        static void addWeights(int* weights, uint16_t sample) {
            const int b = materialWeightB(sample);
            weights[materialSlotA(sample)] += 255 - b;
            if (b) weights[materialSlotB(sample)] += b;
        }

        // R13: the highest summed weights, ties by lower index, sorted by weight
        static MaterialSet setFromWeights(const int* weights) {
            MaterialSet set;
            int order[4];
            int orderWeight[4];
            int count = 0;
            for (int slot = 0; slot < MAX_SLOTS; slot++) {
                const int weight = weights[slot];
                if (weight <= 0) continue;
                int at = count;
                while (at > 0 && orderWeight[at - 1] < weight) at--;
                if (at >= 4) continue;
                const int last = std::min(count, 3);
                for (int i = last; i > at; i--) {
                    order[i] = order[i - 1];
                    orderWeight[i] = orderWeight[i - 1];
                }
                order[at] = slot;
                orderWeight[at] = weight;
                if (count < 4) count++;
            }
            if (count == 0) {
                order[0] = 0;
                count = 1;
            }
            set.count = count;
            uint16_t code = 0;
            for (int i = 0; i < count; i++) {
                set.slots[i] = static_cast<uint8_t>(order[i]);
                code = static_cast<uint16_t>(code | (order[i] << (4 * i)));
            }
            set.code = code;
            return set;
        }

        MaterialSet topSlotsOfBlock() const {
            if (singleMaterial) return singleSet;
            int weights[MAX_SLOTS] = {};
            for (int i = 0; i < MATERIAL_SAMPLES; i++) addWeights(weights, work.material[i]);
            return setFromWeights(weights);
        }

        MaterialSet regularCellSet(int x, int y, int z) const {
            if (useBlockSet) return blockSet;
            if (singleMaterial) return singleSet;
            int weights[MAX_SLOTS] = {};
            for (int dz = 0; dz <= 1; dz++) {
                for (int dy = 0; dy <= 1; dy++) {
                    for (int dx = 0; dx <= 1; dx++) addWeights(weights, work.material[materialIndex(x + dx, y + dy, z + dz)]);
                }
            }
            return setFromWeights(weights);
        }

        // R13: a sample's weights over the set
        static void sampleWeights(const MaterialSet& set, uint16_t sample, int* weights) {
            const int a = materialSlotA(sample), b = materialSlotB(sample), wb = materialWeightB(sample);
            for (int i = 0; i < set.count; i++) {
                const int slot = set.slots[i];
                int w = 0;
                if (slot == a) w += 255 - wb;
                if (slot == b && wb) w += wb;
                weights[i] = w;
            }
            for (int i = set.count; i < 4; i++) weights[i] = 0;
        }

        // Interpolated by t, normalized to sum 255, the rounding remainder on the first weight
        static void setVertexWeights(const MaterialSet& set, uint16_t sample0, uint16_t sample1, int t, MeshVertex& vertex) {
            int w0[4], w1[4];
            sampleWeights(set, sample0, w0);
            sampleWeights(set, sample1, w1);
            int sum = 0;
            int raw[4];
            for (int i = 0; i < 4; i++) {
                raw[i] = w0[i] * t + w1[i] * (FIXED_ONE - t);
                sum += raw[i];
            }
            if (sum <= 0) {
                vertex.weights[0] = 255;
                vertex.weights[1] = vertex.weights[2] = vertex.weights[3] = 0;
                return;
            }
            int total = 0;
            for (int i = 0; i < 4; i++) {
                const int w = static_cast<int>((static_cast<int64_t>(raw[i]) * 255) / sum);
                vertex.weights[i] = static_cast<uint8_t>(w);
                total += w;
            }
            vertex.weights[0] = static_cast<uint8_t>(vertex.weights[0] + (255 - total));
        }

        static void normalize(int32_t gx, int32_t gy, int32_t gz, float* normal) {
            if (gx == 0 && gy == 0 && gz == 0) {
                normal[0] = 0.0f;
                normal[1] = 1.0f;
                normal[2] = 0.0f;
                return;
            }
            const double x = gx, y = gy, z = gz;
            const double length = std::sqrt(x * x + y * y + z * z);
            normal[0] = static_cast<float>(x / length);
            normal[1] = static_cast<float>(y / length);
            normal[2] = static_cast<float>(z / length);
        }

        // V5: moves a vertex of a masked block's boundary layer. A function of position, normal, level and
        // mask alone. A vertex on an unmasked side stays, so the neighbour across it agrees.
        void moveAside(int32_t* position, const float* normal) const {
            if (!mask) return;
            int32_t relative[3];
            double delta[3] = {0.0, 0.0, 0.0};
            bool inLayer = false;
            for (int axis = 0; axis < 3; axis++) {
                relative[axis] = position[axis] - origin[axis];
                const bool lowMasked = (mask & (1u << (axis * 2))) != 0;
                const bool highMasked = (mask & (2u << (axis * 2))) != 0;
                if ((relative[axis] == 0 && !lowMasked) || (relative[axis] == blockFixed && !highMasked)) return;
                if (lowMasked && relative[axis] < cellFixed) {
                    delta[axis] += static_cast<double>(cellFixed - relative[axis]) * 0.5;
                    inLayer = true;
                }
                if (highMasked && relative[axis] > blockFixed - cellFixed) {
                    delta[axis] -= static_cast<double>(cellFixed - (blockFixed - relative[axis])) * 0.5;
                    inLayer = true;
                }
            }
            if (!inLayer) return;
            const double nx = normal[0], ny = normal[1], nz = normal[2];
            const double along = delta[0] * nx + delta[1] * ny + delta[2] * nz;
            const double projected[3] = {delta[0] - along * nx, delta[1] - along * ny, delta[2] - along * nz};
            const double limit = cellFixed;
            for (int axis = 0; axis < 3; axis++) {
                const double clamped = std::clamp(projected[axis], -limit, limit);
                position[axis] += static_cast<int32_t>(std::floor(clamped + 0.5));
            }
        }

        // V5's t: corners ordered low to high along an axis-aligned edge, so a shared edge computes the
        // same bits in every block. A Past-the-end Open transition may hand in equal signs; both then
        // give the midpoint, which only the excluded outer shell can see.
        static int edgeT(int d0, int d1) {
            const int denominator = d1 - d0;
            if ((d0 < 0) == (d1 < 0) || denominator == 0) return FIXED_ONE / 2;
            return std::clamp((d1 * FIXED_ONE) / denominator, 0, FIXED_ONE);
        }

        // Slot 0 as a later slot reads like an unused nibble, so the set's identity includes its count
        static bool sameSet(const MeshVertex& vertex, const MaterialSet& set) {
            return vertex.slots == set.code && vertex.spare == set.count;
        }

        // A vertex on the level-k lattice: regular cells and the transition cell's full-resolution face
        int fineVertex(int x, int y, int z, int axis, const MaterialSet& set) {
            const int edge = edgeIndex(x, y, z, axis);
            int first = -1;
            if (work.edgeStamp[edge] == work.stamp) {
                first = work.edgeFirst[edge];
                for (int at = first; at >= 0; at = work.nextOnEdge[static_cast<size_t>(at)]) {
                    if (!full || sameSet(out.vertices[static_cast<size_t>(at)], set)) return at;
                }
            }
            const int dx = axis == 0, dy = axis == 1, dz = axis == 2;
            const int d0 = work.sdf[windowIndex(x, y, z)];
            const int d1 = work.sdf[windowIndex(x + dx, y + dy, z + dz)];
            const int t = edgeT(d0, d1);
            MeshVertex vertex{};
            const int low[3] = {originSamples.x + x, originSamples.y + y, originSamples.z + z};
            const int high[3] = {low[0] + dx, low[1] + dy, low[2] + dz};
            for (int i = 0; i < 3; i++) {
                vertex.position[i] = (low[i] * t + high[i] * (FIXED_ONE - t)) << level;
            }
            if (full || mask) {
                const int g0x = work.sdf[windowIndex(x + 1, y, z)] - work.sdf[windowIndex(x - 1, y, z)];
                const int g0y = work.sdf[windowIndex(x, y + 1, z)] - work.sdf[windowIndex(x, y - 1, z)];
                const int g0z = work.sdf[windowIndex(x, y, z + 1)] - work.sdf[windowIndex(x, y, z - 1)];
                const int x1 = x + dx, y1 = y + dy, z1 = z + dz;
                const int g1x = work.sdf[windowIndex(x1 + 1, y1, z1)] - work.sdf[windowIndex(x1 - 1, y1, z1)];
                const int g1y = work.sdf[windowIndex(x1, y1 + 1, z1)] - work.sdf[windowIndex(x1, y1 - 1, z1)];
                const int g1z = work.sdf[windowIndex(x1, y1, z1 + 1)] - work.sdf[windowIndex(x1, y1, z1 - 1)];
                normalize(g0x * t + g1x * (FIXED_ONE - t), g0y * t + g1y * (FIXED_ONE - t), g0z * t + g1z * (FIXED_ONE - t), vertex.normal);
                moveAside(vertex.position, vertex.normal);
            }
            if (full) {
                vertex.slots = set.code;
                vertex.spare = static_cast<uint16_t>(set.count);
                if (singleMaterial || set.count == 1) {
                    vertex.weights[0] = 255;
                } else {
                    setVertexWeights(set, work.material[materialIndex(x, y, z)], work.material[materialIndex(x + dx, y + dy, z + dz)], t, vertex);
                }
            }
            const int index = static_cast<int>(out.vertices.size());
            out.vertices.push_back(vertex);
            work.nextOnEdge.push_back(first);
            work.edgeStamp[edge] = work.stamp;
            work.edgeFirst[edge] = index;
            return index;
        }

        // A vertex on the low-resolution face of a side: the same edge function at the next level's
        // spacing and gradients, so it is bit-equal to the coarse block's own (V5). c* are coarse-window
        // coordinates; axis the edge's direction.
        int coarseVertex(int side, int cx, int cy, int cz, int axis, const MaterialSet& set) {
            const SideAxes& axes = SIDE_AXES[side];
            const int coord[3] = {cx, cy, cz};
            // Dense index among the face's coarse edges: (u, v) along u or v
            const int u = coord[axes.uAxis], v = coord[axes.vAxis];
            const int local = axis == axes.uAxis ? u + (BLOCK_SIZE / 2) * v : (BLOCK_SIZE / 2) * (BLOCK_SIZE / 2 + 1) + v + (BLOCK_SIZE / 2) * u;
            const int edge = side * LOW_RES_EDGES_PER_SIDE + local;
            int first = -1;
            if (work.lowStamp[edge] == work.stamp) {
                first = work.lowFirst[edge];
                for (int at = first; at >= 0; at = work.nextOnEdge[static_cast<size_t>(at)]) {
                    if (!full || sameSet(out.vertices[static_cast<size_t>(at)], set)) return at;
                }
            }
            const int dx = axis == 0, dy = axis == 1, dz = axis == 2;
            const int d0 = work.coarse[coarseIndex(cx, cy, cz)];
            const int d1 = work.coarse[coarseIndex(cx + dx, cy + dy, cz + dz)];
            const int t = edgeT(d0, d1);
            MeshVertex vertex{};
            const int low[3] = {coarseOrigin.x + cx, coarseOrigin.y + cy, coarseOrigin.z + cz};
            const int high[3] = {low[0] + dx, low[1] + dy, low[2] + dz};
            for (int i = 0; i < 3; i++) {
                vertex.position[i] = (low[i] * t + high[i] * (FIXED_ONE - t)) << (level + 1);
            }
            if (full) {
                const int x1 = cx + dx, y1 = cy + dy, z1 = cz + dz;
                const int g0x = work.coarse[coarseIndex(cx + 1, cy, cz)] - work.coarse[coarseIndex(cx - 1, cy, cz)];
                const int g0y = work.coarse[coarseIndex(cx, cy + 1, cz)] - work.coarse[coarseIndex(cx, cy - 1, cz)];
                const int g0z = work.coarse[coarseIndex(cx, cy, cz + 1)] - work.coarse[coarseIndex(cx, cy, cz - 1)];
                const int g1x = work.coarse[coarseIndex(x1 + 1, y1, z1)] - work.coarse[coarseIndex(x1 - 1, y1, z1)];
                const int g1y = work.coarse[coarseIndex(x1, y1 + 1, z1)] - work.coarse[coarseIndex(x1, y1 - 1, z1)];
                const int g1z = work.coarse[coarseIndex(x1, y1, z1 + 1)] - work.coarse[coarseIndex(x1, y1, z1 - 1)];
                normalize(g0x * t + g1x * (FIXED_ONE - t), g0y * t + g1y * (FIXED_ONE - t), g0z * t + g1z * (FIXED_ONE - t), vertex.normal);
                vertex.slots = set.code;
                vertex.spare = static_cast<uint16_t>(set.count);
                if (singleMaterial || set.count == 1) {
                    vertex.weights[0] = 255;
                } else {
                    setVertexWeights(set, work.coarseMaterial[coarseIndex(cx, cy, cz)], work.coarseMaterial[coarseIndex(cx + dx, cy + dy, cz + dz)], t, vertex);
                }
            }
            const int index = static_cast<int>(out.vertices.size());
            out.vertices.push_back(vertex);
            work.nextOnEdge.push_back(first);
            work.lowStamp[edge] = work.stamp;
            work.lowFirst[edge] = index;
            return index;
        }

        bool emitTriangle(int a, int b, int c) {
            if (request.coverage) {
                const MeshVertex& va = out.vertices[static_cast<size_t>(a)];
                const MeshVertex& vb = out.vertices[static_cast<size_t>(b)];
                const MeshVertex& vc = out.vertices[static_cast<size_t>(c)];
                const auto culled = [this](const MeshVertex& v) {
                    return request.coverage->vertexCulled(v.position[0] / static_cast<double>(FIXED_ONE), v.position[1] / static_cast<double>(FIXED_ONE), v.position[2] / static_cast<double>(FIXED_ONE));
                };
                if (culled(va) && culled(vb) && culled(vc)) return false;
            }
            out.indices.push_back(static_cast<uint16_t>(a));
            out.indices.push_back(static_cast<uint16_t>(b));
            out.indices.push_back(static_cast<uint16_t>(c));
            return true;
        }

        void regularCell(int x, int y, int z, int caseCode, int vertexLimit, bool& overflow) {
            const tables::CellGeometry geometry = tables::regularCell(caseCode);
            const unsigned short* vertexData = tables::regularVertices(caseCode);
            const MaterialSet set = full ? regularCellSet(x, y, z) : MaterialSet{};
            int cellVertices[12];
            for (int i = 0; i < geometry.vertexCount; i++) {
                const int data = vertexData[i] & 0xFF;
                const int c0 = data >> 4, c1 = data & 0xF;
                // The two corners differ in one bit, whose axis is the edge's; the lower is the corner
                const int differ = c0 ^ c1;
                const int axis = differ == 1 ? 0 : (differ == 2 ? 1 : 2);
                const int lowCorner = std::min(c0, c1);
                cellVertices[i] = fineVertex(x + (lowCorner & 1), y + ((lowCorner >> 1) & 1), z + ((lowCorner >> 2) & 1), axis, set);
            }
            if (static_cast<int>(out.vertices.size()) > vertexLimit) {
                overflow = true;
                return;
            }
            for (int i = 0; i < geometry.triangleCount; i++) {
                // The table's winding is counter-clockwise seen from the air (R9), our front face
                emitTriangle(cellVertices[geometry.vertexIndex[i * 3]],
                             cellVertices[geometry.vertexIndex[i * 3 + 1]],
                             cellVertices[geometry.vertexIndex[i * 3 + 2]]);
            }
        }

        // Coordinates of a face sample on the full-resolution lattice, in window terms
        void faceSample(const SideAxes& axes, int u, int v, int* xyz) const {
            xyz[axes.normalAxis] = axes.high ? BLOCK_SIZE : 0;
            xyz[axes.uAxis] = u;
            xyz[axes.vAxis] = v;
        }

        MaterialSet transitionCellSet(const SideAxes& axes, int u, int v) const {
            if (useBlockSet) return blockSet;
            if (singleMaterial) return singleSet;
            int weights[MAX_SLOTS] = {};
            for (int dv = 0; dv < 3; dv++) {
                for (int du = 0; du < 3; du++) {
                    int xyz[3];
                    faceSample(axes, u + du, v + dv, xyz);
                    addWeights(weights, work.material[materialIndex(xyz[0], xyz[1], xyz[2])]);
                }
            }
            return setFromWeights(weights);
        }

        void transitionCells(int side, int vertexLimit, bool& overflow) {
            const SideAxes& axes = SIDE_AXES[side];
            for (int cv = 0; cv < BLOCK_SIZE / 2; cv++) {
                for (int cu = 0; cu < BLOCK_SIZE / 2; cu++) {
                    transitionCell(side, axes, cu, cv, vertexLimit, overflow);
                    if (overflow) return;
                }
            }
        }

        void transitionCell(int side, const SideAxes& axes, int cu, int cv, int vertexLimit, bool& overflow) {
            int xyz[3];
            int caseCode = 0;
            for (int corner = 0; corner < 9; corner++) {
                faceSample(axes, cu * 2 + corner % 3, cv * 2 + corner / 3, xyz);
                if (work.sdf[windowIndex(xyz[0], xyz[1], xyz[2])] < 0) caseCode |= 1 << TRANSITION_CASE_BIT[corner];
            }
            if (caseCode == 0 || caseCode == 511) return;
            bool reversed = false;
            const tables::CellGeometry geometry = tables::transitionCell(caseCode, reversed);
            const unsigned short* vertexData = tables::transitionVertices(caseCode);
            const MaterialSet set = full ? transitionCellSet(axes, cu * 2, cv * 2) : MaterialSet{};
            int cellVertices[12];
            for (int i = 0; i < geometry.vertexCount; i++) {
                const int data = vertexData[i] & 0xFF;
                const int c0 = data >> 4, c1 = data & 0xF;
                if (c0 < 9) {
                    // Both corners on the full-resolution face: the lattice edge between them
                    const int u0 = c0 % 3, v0 = c0 / 3, u1 = c1 % 3, v1 = c1 / 3;
                    const int alongU = u0 != u1;
                    faceSample(axes, cu * 2 + std::min(u0, u1), cv * 2 + std::min(v0, v1), xyz);
                    cellVertices[i] = fineVertex(xyz[0], xyz[1], xyz[2], alongU ? axes.uAxis : axes.vAxis, set);
                } else {
                    // Corners 9-C are the coarse samples at 0, 2, 6, 8
                    const int u0 = (c0 - 9) & 1, v0 = (c0 - 9) >> 1, u1 = (c1 - 9) & 1, v1 = (c1 - 9) >> 1;
                    const int alongU = u0 != u1;
                    int coarse[3];
                    coarse[axes.normalAxis] = axes.high ? BLOCK_SIZE / 2 : 0;
                    coarse[axes.uAxis] = cu + std::min(u0, u1);
                    coarse[axes.vAxis] = cv + std::min(v0, v1);
                    cellVertices[i] = coarseVertex(side, coarse[0], coarse[1], coarse[2], alongU ? axes.uAxis : axes.vAxis, set);
                }
            }
            if (static_cast<int>(out.vertices.size()) > vertexLimit) {
                overflow = true;
                return;
            }
            for (int i = 0; i < geometry.triangleCount; i++) {
                const int a = cellVertices[geometry.vertexIndex[i * 3]];
                const int b = cellVertices[geometry.vertexIndex[i * 3 + 1]];
                const int c = cellVertices[geometry.vertexIndex[i * 3 + 2]];
                // SIDE_AXES has u x v pointing into the block, which is the mirror of the table's frame
                if (reversed) {
                    emitTriangle(a, b, c);
                } else {
                    emitTriangle(a, c, b);
                }
            }
        }

        bool build(int vertexLimit) {
            out.vertices.clear();
            out.indices.clear();
            work.nextOnEdge.clear();
            work.stamp++;
            if (work.stamp == 0) {
                std::memset(work.edgeStamp, 0, sizeof(work.edgeStamp));
                std::memset(work.lowStamp, 0, sizeof(work.lowStamp));
                work.stamp = 1;
            }
            bool overflow = false;
            for (int z = 0; z < BLOCK_SIZE; z++) {
                for (int y = 0; y < BLOCK_SIZE; y++) {
                    const int16_t* row0 = work.sdf + windowIndex(0, y, z);
                    const int16_t* row1 = work.sdf + windowIndex(0, y + 1, z);
                    const int16_t* row2 = work.sdf + windowIndex(0, y, z + 1);
                    const int16_t* row3 = work.sdf + windowIndex(0, y + 1, z + 1);
                    int previous = (row0[0] < 0 ? 1 : 0) | (row1[0] < 0 ? 2 : 0) | (row2[0] < 0 ? 4 : 0) | (row3[0] < 0 ? 8 : 0);
                    for (int x = 0; x < BLOCK_SIZE; x++) {
                        const int current = (row0[x + 1] < 0 ? 1 : 0) | (row1[x + 1] < 0 ? 2 : 0) | (row2[x + 1] < 0 ? 4 : 0) | (row3[x + 1] < 0 ? 8 : 0);
                        // Corner bits: x = 1, y = 2, z = 4
                        const int caseCode = (previous & 1) | ((current & 1) << 1) | ((previous & 2) << 1) | ((current & 2) << 2)
                            | ((previous & 4) << 2) | ((current & 4) << 3) | ((previous & 8) << 3) | ((current & 8) << 4);
                        previous = current;
                        if (caseCode == 0 || caseCode == 255) continue;
                        regularCell(x, y, z, caseCode, vertexLimit, overflow);
                        if (overflow) return false;
                    }
                }
            }
            for (int side = 0; side < SIDE_COUNT; side++) {
                if (!(mask & (1u << side))) continue;
                transitionCells(side, vertexLimit, overflow);
                if (overflow) return false;
            }
            return true;
        }

        void finish() {
            if (request.coverage && !out.indices.empty()) compact();
            out.triangleCount = static_cast<int>(out.indices.size() / 3);
            if (out.indices.empty()) {
                out.vertices.clear();
                return;
            }
            for (int axis = 0; axis < 3; axis++) {
                out.boundsMin[axis] = std::numeric_limits<int32_t>::max();
                out.boundsMax[axis] = std::numeric_limits<int32_t>::min();
            }
            for (const MeshVertex& vertex : out.vertices) {
                for (int axis = 0; axis < 3; axis++) {
                    out.boundsMin[axis] = std::min(out.boundsMin[axis], vertex.position[axis]);
                    out.boundsMax[axis] = std::max(out.boundsMax[axis], vertex.position[axis]);
                }
            }
        }

        // R41 dropped triangles: their vertices go too, so a culled block costs nothing
        void compact() {
            std::vector<int32_t> remap(out.vertices.size(), -1);
            std::vector<MeshVertex> kept;
            kept.reserve(out.vertices.size());
            for (uint16_t& index : out.indices) {
                int32_t& mapped = remap[index];
                if (mapped < 0) {
                    mapped = static_cast<int32_t>(kept.size());
                    kept.push_back(out.vertices[index]);
                }
                index = static_cast<uint16_t>(mapped);
            }
            out.vertices.swap(kept);
        }

        const ReadView& view;
        const MeshRequest& request;
        MeshOut& out;
        Scratch& work;
        const VolumeDesc& volume;
        int level;
        int32_t cellFixed = FIXED_ONE;
        int32_t blockFixed = FIXED_ONE * BLOCK_SIZE;
        Int3 originSamples;
        Int3 coarseOrigin;
        int32_t origin[3] = {0, 0, 0};
        bool full = true;
        uint8_t mask = 0;
        bool singleMaterial = true;
        MaterialSet singleSet;
        bool useBlockSet = false;
        MaterialSet blockSet;
    };

}

void MeshOut::clear() {
    vertices.clear();
    indices.clear();
    for (int axis = 0; axis < 3; axis++) {
        boundsMin[axis] = 0;
        boundsMax[axis] = 0;
    }
    triangleCount = 0;
    usedBlockMaterialSet = false;
}

bool jasperengine::voxel::meshBlock(const ReadView& view, const MeshRequest& request, MeshOut& out, const MeshOptions& options) {
    if (request.key != view.center()) return false;
    Mesher mesher(view, request, out, scratch());
    mesher.run(options);
    return true;
}

AuditResult jasperengine::voxel::auditWatertight(std::span<const MeshOut* const> meshes, const IntBox* outerShell) {
    struct PositionKey {
        int32_t x, y, z;
        bool operator==(const PositionKey& other) const { return x == other.x && y == other.y && z == other.z; }
    };
    struct PositionHash {
        size_t operator()(const PositionKey& key) const {
            return fnv1a(&key, sizeof(key));
        }
    };
    std::unordered_map<PositionKey, uint32_t, PositionHash> weld;
    std::unordered_map<uint64_t, int32_t> balance;
    std::vector<PositionKey> welded;
    const auto idOf = [&](const MeshVertex& vertex) {
        const PositionKey key{vertex.position[0], vertex.position[1], vertex.position[2]};
        const auto found = weld.try_emplace(key, static_cast<uint32_t>(welded.size()));
        if (found.second) welded.push_back(key);
        return found.first->second;
    };
    const auto onOuterPlane = [&](const PositionKey& a, const PositionKey& b) {
        if (!outerShell) return false;
        return (a.x <= outerShell->min.x && b.x <= outerShell->min.x) || (a.x >= outerShell->max.x && b.x >= outerShell->max.x)
            || (a.y <= outerShell->min.y && b.y <= outerShell->min.y) || (a.y >= outerShell->max.y && b.y >= outerShell->max.y)
            || (a.z <= outerShell->min.z && b.z <= outerShell->min.z) || (a.z >= outerShell->max.z && b.z >= outerShell->max.z);
    };
    AuditResult result;
    for (const MeshOut* mesh : meshes) {
        if (!mesh) continue;
        for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
            const uint32_t ids[3] = {idOf(mesh->vertices[mesh->indices[i]]), idOf(mesh->vertices[mesh->indices[i + 1]]), idOf(mesh->vertices[mesh->indices[i + 2]])};
            for (int edge = 0; edge < 3; edge++) {
                const uint32_t from = ids[edge], to = ids[(edge + 1) % 3];
                if (from == to) continue;
                if (onOuterPlane(welded[from], welded[to])) continue;
                result.edgesChecked++;
                const uint64_t low = std::min(from, to), high = std::max(from, to);
                balance[(high << 32) | low] += from < to ? 1 : -1;
            }
        }
    }
    for (const auto& entry : balance) {
        if (entry.second != 0) result.unbalancedEdges++;
    }
    result.watertight = result.unbalancedEdges == 0;
    return result;
}
