// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolPipelineCache_h
#define SokolPipelineCache_h

#include "render/PipelineState.h"

#include "sokol_gfx.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine{

    // Pipelines of the dynamic path (ObjectRender::endLoadDynamic / beginDraw(PipelineState)),
    // keyed by shader, vertex layout, index and primitive type and the PipelineState. Sokol bakes
    // the stencil reference, blend and formats into the pipeline on every backend, so each
    // state actually drawn gets its own pipeline, created on first use and kept until its
    // shader is destroyed (releaseShader) or the render system shuts down (clear).
    class SokolPipelineCache{
    public:
        // creates on a miss; SG_INVALID_ID when sokol could not create it
        static sg_pipeline get(const sg_pipeline_desc& base, const PipelineState& state);
        // destroys every pipeline made with it (SokolShader::destroyShader)
        static void releaseShader(sg_shader shader);
        // every pipeline (SokolSystem shutdown)
        static void clear();
        static size_t size();

        // the description get() creates the pipeline from (tests)
        static sg_pipeline_desc makeDesc(const sg_pipeline_desc& base, const PipelineState& state);

        // sg_apply_pipeline of the dynamic path, skipped when the pipeline is the one applied
        // last in this pass. True when it was applied
        static bool apply(sg_pipeline pipeline);
        // a pass began, or another path applied a pipeline: the next apply() applies
        static void invalidateApplied();

        // counters since start: misses (pipelines created) and sg_apply_pipeline calls of apply()
        static uint64_t getMissCount();
        static uint64_t getApplyCount();
    };
}

#endif //SokolPipelineCache_h
