// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISEGRAPHRUNTIME_H
#define NOISE_NOISEGRAPHRUNTIME_H

// The compiled form of a Noise Graph and what evaluates it (T2 R17-R23, R69, N15): the seam T4, T5
// and T6 use. compile() makes an immutable plan, never stored (N5); evaluate it from any number of
// threads, each with its own EvalScratch. A sample's bits depend only on its position, the root seed
// and the node's parameters, never on the chunk, region or thread count that computed it (R7, R23).
// Everything here is in the JASPERENGINE_NOISE module.

#include "Export.h"
#include "graph/Graph.h"
#include "noise/NoiseTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine { namespace jobs { class CancelToken; } }

namespace jasperengine { namespace noise {

    // A 2D region: the position of texel (i, j) is float((double(start + i) + 0.5) * unitsPerTexel) per
    // axis (R23), at height y
    struct GridDomain {
        int64_t startX = 0, startZ = 0;
        int width = 0, depth = 0;
        double unitsPerTexel = 1.0;
        double y = 0.0;
    };

    // A 3D region: samples on the lattice, float(double(start + i) * unitsPerVoxel) with no half step,
    // so a coarse sample is the fine sample at its position (R21). Layout: x fastest, then y, then z.
    struct Grid3Domain {
        int64_t startX = 0, startY = 0, startZ = 0;
        int width = 0, height = 0, depth = 0;
        double unitsPerVoxel = 1.0;
    };

    // Per source node (R20). Only collected when EvalContext::profile is set: off, no clock is read.
    struct NodeProfileEntry {
        std::string nodeId;
        double ms = 0.0;
        double nsPerPoint = 0.0;
        double share = 0.0;
    };

    struct NodeProfile {
        std::vector<NodeProfileEntry> nodes;
        double totalMs = 0.0;
        uint64_t points = 0;
    };

    // What one evaluation did, for reports (R22, R26, R52)
    struct EvalReport {
        size_t peakBytes = 0;
        double erosionMs = 0.0;
        uint64_t dropletSteps = 0;
        bool cancelled = false;
    };

    // Per thread (R21): sized from CompiledGraph::scratchBytes(), it grows once and then never allocates
    class JASPERENGINE_API EvalScratch {
    public:
        EvalScratch() = default;
        explicit EvalScratch(size_t bytes) { reserve(bytes); }
        void reserve(size_t bytes) { if (storage.size() * sizeof(float) < bytes) storage.resize((bytes + sizeof(float) - 1) / sizeof(float)); }
        float* data() { return storage.data(); }
        size_t bytes() const { return storage.size() * sizeof(float); }
        // The evaluator's table of buffer pointers, kept here so a warm scratch never allocates
        float** table(size_t count) { if (pointers.size() < count) pointers.resize(count); return pointers.data(); }
    private:
        std::vector<float> storage;
        std::vector<float*> pointers;
    };

    struct EvalContext {
        uint32_t seed = 1;
        float maxHeight = 100.0f;           // world units: what output_height in metres, slope and erosion scale by
        double metresPerTexel = 1.0;        // the world width of a texel, for slope and erosion
        // Exposed variables not fixed at compile time; a missing one takes its default
        std::span<const std::pair<std::string, float>> variables;
        // The terrain_height snapshot, when the graph reads it: normalized, width * depth, whose first
        // texel is the global texel (startX, startZ). A padded region reads the nearest edge past it.
        const float* terrainHeight = nullptr;
        int terrainHeightWidth = 0, terrainHeightDepth = 0;
        int64_t terrainHeightStartX = 0, terrainHeightStartZ = 0;
        // terrain_uv: u = x * uvPerUnit, clamped to 0..1, for a whole terrain's UVs in any of its
        // regions. 0: 0..1 over the evaluated domain, and the position itself for points.
        double uvPerUnit = 0.0;
        jobs::CancelToken* cancel = nullptr;
        bool parallel = true;
        NodeProfile* profile = nullptr;
        uint64_t (*clock)() = nullptr;      // nanoseconds; null is the steady clock. A seam for the profile's test.
        EvalScratch* scratch = nullptr;     // null: the evaluator's own, per worker. Set: one thread, this scratch.
        Backend backend = Backend::Native;  // Scalar exists under JASPERENGINE_NOISE_ALL_BACKENDS (tests, editor)
        EvalReport* report = nullptr;
    };

    enum class OutputKind : uint8_t { Height, Layer, Density, Value, NodePreview };

    struct OutputInfo {
        OutputKind kind = OutputKind::Value;
        std::string name;       // "height", "layer:2", "density:0", a value's own name, "node:n3:out"
        std::string nodeId;
        int index = -1;         // the layer or foliage layer
        bool optional = false;  // output_layer, output_density: a missing target warns instead of failing
    };

    struct CompiledNodeInfo {
        std::string id;
        std::string type;
        Interval range;         // the analytic range of its first output
        bool dead = false;
        bool folded = false;
    };

    struct PlanData;

    class JASPERENGINE_API CompiledGraph {          // immutable: share across threads
    public:
        explicit CompiledGraph(std::unique_ptr<const PlanData> plan);
        ~CompiledGraph();

        bool isGridOnly() const;
        bool isBakeOnly() const;
        // By OutputInfo::name; -1 when there is none
        int outputIndex(std::string_view name) const;
        const std::vector<OutputInfo>& outputs() const;
        const std::vector<CompiledNodeInfo>& nodes() const;
        // Graph inputs the plan reads: whether a generation must snapshot heights
        bool readsTerrainHeight() const;

        Interval rangeOf(int output) const;
        // The output's range for positions inside the box (R19): T6's block clipping
        Interval rangeOver(int output, const float boxMin[3], const float boxMax[3]) const;
        // Per scratch: buffers * 4096 * 4 bytes (R52)
        size_t scratchBytes() const;
        size_t bufferCount() const;     // what a chunk holds at most, for the report: BuffersAreReused
        size_t opCount() const;
        // Texels of padding the output's region is evaluated with (R22)
        int paddingOf(int output) const;

        // False when cancelled. width * depth values, row by row.
        bool evaluateGrid(const GridDomain& domain, const EvalContext& context, int output, float* out) const;
        // Several outputs of one evaluation, sharing its work (a stage runs once)
        bool evaluateGridOutputs(const GridDomain& domain, const EvalContext& context, std::span<const int> outputs, std::span<float* const> out) const;
        bool evaluateGrid3D(const Grid3Domain& domain, const EvalContext& context, int output, float* out) const;
        // One thread, no allocation: T6's blocks are already on a job. A grid-only graph is refused at
        // compile time by CompileOptions::forbidGridOnly; reaching here with one writes zeros.
        void evaluatePoints(const float* x, const float* y, const float* z, size_t count, const EvalContext& context,
                            int output, float* out, EvalScratch& scratch) const;

    private:
        std::unique_ptr<const PlanData> plan;
    };

    struct CompileOptions {
        bool forbidGridOnly = false;    // grid-only-node is an error: scripts' sample(), T6
        bool forbidBakeOnly = false;    // bake-only-node is an error: a game's load (N7)
        int resolution = 0;             // texels per side for erosion-budget; 0: metadata.noise.resolution, else 1024
        // Fixed here, so what depends only on them folds (R18)
        std::span<const std::pair<std::string, float>> variables;
        float maxHeight = 100.0f;       // what output_height in metres divides by, for its range
        // Every node's outputs become outputs named "node:<id>:<port>" (a coord: :x and :z), so a
        // preview can show them; deadNodes still says what reaches no real output
        bool previewNodes = false;
    };

    struct CompileResult {
        std::shared_ptr<const CompiledGraph> graph; // null when an issue is an error
        std::vector<graph::Issue> issues;
        std::vector<std::string> deadNodes, foldedNodes;
        size_t opCount = 0, bufferCount = 0;
    };

    JASPERENGINE_API CompileResult compile(const graph::Graph& graph, const CompileOptions& options = {});
    // Idempotent; GraphRegistry owns the descriptors (R15, R50)
    JASPERENGINE_API void registerNoiseKind();
    // FNV-1a 64 of the raw bytes (R41)
    JASPERENGINE_API uint64_t hashFloats(const float* values, size_t count);

}}

#endif // NOISE_NOISEGRAPHRUNTIME_H
