// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelGeneratorNoise.h"

#include "graph/Graph.h"

#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::voxel;

namespace {

    class NoiseBase final : public BaseSource {
    public:
        explicit NoiseBase(const NoiseBaseOptions& opts): options(opts), output(opts.graph->outputIndex("sdf")) {}

        // A coarse sample and the fine sample at its position are the same computation (R5): integer
        // indices and units per voxel, volume-local, so moving a volume does not change its base
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            thread_local noise::EvalScratch scratch;
            scratch.reserve(options.graph->scratchBytes());
            noise::Grid3Domain domain;
            domain.startX = origin.x / spacing;
            domain.startY = origin.y / spacing;
            domain.startZ = origin.z / spacing;
            domain.width = domain.height = domain.depth = BLOCK_SIZE;
            domain.unitsPerVoxel = options.voxelSize * spacing;
            noise::EvalContext context;
            context.seed = options.seed;
            context.parallel = false;
            context.scratch = &scratch;
            float values[BLOCK_SAMPLES];
            options.graph->evaluateGrid3D(domain, context, output, values);
            const uint16_t slot = packMaterial(options.material, options.material, 0);
            const double scale = options.graphScale / options.voxelSize;
            for (int i = 0; i < BLOCK_SAMPLES; i++) {
                sdf[i] = encodeSdf(static_cast<double>(values[i]) * scale);
                material[i] = slot;
            }
        }

        // R34: a block whose range is wholly beyond the band resolves uniform with no evaluation
        bool evaluateUniform(const Int3& origin, int spacing, int16_t& sdf, uint16_t& material) const override {
            const float span = static_cast<float>((BLOCK_SIZE - 1) * spacing * options.voxelSize);
            const float low[3] = {static_cast<float>(origin.x * options.voxelSize), static_cast<float>(origin.y * options.voxelSize), static_cast<float>(origin.z * options.voxelSize)};
            const float high[3] = {low[0] + span, low[1] + span, low[2] + span};
            const noise::Interval range = options.graph->rangeOver(output, low, high);
            const double scale = options.graphScale / options.voxelSize;
            material = packMaterial(options.material, options.material, 0);
            if (static_cast<double>(range.min) * scale >= SDF_BAND_VOXELS) {
                sdf = SDF_AIR;
                return true;
            }
            if (static_cast<double>(range.max) * scale <= -SDF_BAND_VOXELS) {
                sdf = -SDF_MAX;
                return true;
            }
            return false;
        }

    private:
        NoiseBaseOptions options;
        int output;
    };

}

std::shared_ptr<const BaseSource> jasperengine::voxel::makeNoiseBase(const NoiseBaseOptions& options) {
    return std::make_shared<NoiseBase>(options);
}

std::shared_ptr<const noise::CompiledGraph> jasperengine::voxel::compileVoxelGraph(std::string_view json, std::string& error) {
    noise::registerNoiseKind();
    graph::LoadReport report;
    const graph::Graph source = graph::load(json, report);
    noise::CompileOptions options;
    options.forbidGridOnly = true;
    options.forbidBakeOnly = true;
    noise::CompileResult compiled = noise::compile(source, options);
    for (const graph::Issue& issue : compiled.issues) {
        if (issue.severity != graph::Severity::Error) continue;
        error = issue.message + (issue.nodeId.empty() ? std::string() : " (node " + issue.nodeId + ")");
        return nullptr;
    }
    if (!compiled.graph) {
        error = "the graph did not compile";
        return nullptr;
    }
    if (compiled.graph->outputIndex("sdf") < 0) {
        error = "the graph has no Value Output named sdf";
        return nullptr;
    }
    return compiled.graph;
}
