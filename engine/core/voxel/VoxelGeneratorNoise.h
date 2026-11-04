// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_GENERATOR_NOISE_H
#define VOXEL_GENERATOR_NOISE_H

// R34: a base from a Noise Graph, only through T2's runtime (D6, D10). The main thread compiles the graph;
// workers share the immutable plan, each with its own scratch.

#include "Export.h"
#include "VoxelStore.h"
#include "noise/NoiseGraphRuntime.h"

#include <memory>
#include <string>

namespace jasperengine::voxel {

    struct NoiseBaseOptions {
        std::shared_ptr<const noise::CompiledGraph> graph;
        uint32_t seed = 1;
        double voxelSize = 1.0;
        // the named output_value sdf, times this, is the distance in metres (R34)
        double graphScale = 1.0;
        int material = 0;
    };

    JASPERENGINE_API std::shared_ptr<const BaseSource> makeNoiseBase(const NoiseBaseOptions& options);

    // Compiles a graph as R34 asks: 3D world space, no grid-only and no bake-only node, an output named sdf.
    // Null when it cannot; error names the node.
    JASPERENGINE_API std::shared_ptr<const noise::CompiledGraph> compileVoxelGraph(std::string_view json, std::string& error);

}

#endif
