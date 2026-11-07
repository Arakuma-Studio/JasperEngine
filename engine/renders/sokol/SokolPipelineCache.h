// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolPipelineCache_h
#define SokolPipelineCache_h

#include "render/PipelineState.h"

#include "sokol_gfx.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

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
        // the same with the base's hash computed once by the caller (baseHash()): hashing the
        // vertex layout costs more than the lookup
        // immediate: created at once (a pending one is waited for), for the passes of the frame
        static sg_pipeline get(const sg_pipeline_desc& base, uint64_t baseHash, const PipelineState& state, bool immediate = false);
        static uint64_t baseHash(const sg_pipeline_desc& base);
        // bumped whenever pipelines are destroyed (releaseShader, clear): a pipeline a caller kept
        // from get() is valid while it is the same
        static uint32_t generation();
        // destroys every pipeline made with it (SokolShader::destroyShader)
        static void releaseShader(sg_shader shader);
        // every pipeline (SokolSystem shutdown)
        static void clear();
        static size_t size();

        // the description get() creates the pipeline from (tests)
        static sg_pipeline_desc makeDesc(const sg_pipeline_desc& base, const PipelineState& state);

        // ---- interning (spec S32 R3): the pipelines of SokolObject::endLoad. Objects whose
        // descriptions are equal (every field but the label) share one pipeline, counted: a
        // second object with the same state gets the first one's, ready if it is. Its last
        // release() leaves it to the next collect() round but one (an object loaded again in
        // the same frame keeps it); releaseShader() destroys it with its shader.
        static sg_pipeline acquire(const sg_pipeline_desc& desc, bool immediate = false);
        static void release(sg_pipeline pipeline);
        // once a frame (SokolAsyncCreate::update): destroys what was released a round before and
        // not acquired since
        static void collect();
        // one more reference to an interned pipeline (the shader warm-up keeps what it made)
        static bool retain(sg_pipeline pipeline);
        static size_t internedSize();
        // every pipeline created from now (interned and dynamic) is reported with the description
        // it was asked with (the shader warm-up's recording, spec S32 R10); any thread, under the
        // cache's lock: the recorder only queues
        static void setRecorder(std::function<void(const sg_pipeline_desc& desc)> recorder);
        // The base description of a loaded SokolObject (shader, vertex layout, index and
        // primitive type), shared by every object loaded with an equal one (descEqual) and read
        // only: an object keeps a reference instead of a copy of its own. The last reference
        // takes it out (any thread). Counted in the memory record Render.PipelineDescs
        static std::shared_ptr<const sg_pipeline_desc> internBase(const sg_pipeline_desc& desc);
        // the base descriptions alive (tests, diagnostics)
        static size_t internedBaseCount();
        // the hash and equality interning uses, field by field (never the struct's bytes: padding)
        static uint64_t descHash(const sg_pipeline_desc& desc);
        static bool descEqual(const sg_pipeline_desc& a, const sg_pipeline_desc& b);

        // sg_apply_pipeline of the dynamic path, skipped when the pipeline is the one applied
        // last in this pass. True when it was applied
        static bool apply(sg_pipeline pipeline);
        // a pass began, or another path applied a pipeline: the next apply() applies
        static void invalidateApplied();
        // changes with every pipeline applied and every invalidateApplied: the uniform blocks
        // applied in an earlier epoch are not applied any more (sokol asks for them again after
        // sg_apply_pipeline), those of this one stay until the next (SokolDrawBatch, spec S55)
        static uint32_t appliedEpoch();
        // the pipeline applied last in this pass (SG_INVALID_ID when unknown)
        static uint32_t appliedPipeline();

        // counters since start: misses (pipelines created) and sg_apply_pipeline calls of apply()
        static uint64_t getMissCount();
        static uint64_t getApplyCount();

        // Variants of the fixed pipelines (SokolObject::beginDraw: the reversed face winding of a
        // mirrored draw, the editor's no-cull view), shared by every object asking for the same
        // description and reference counted: acquire makes the pipeline on a miss (main thread
        // only; SG_INVALID_ID when sokol could not), release lets go of one reference and
        // destroys the pipeline with the last (an unknown handle is ignored). releaseShader does
        // not touch them (their objects release them, as their own pipelines); clear destroys all
        static sg_pipeline acquireVariant(const sg_pipeline_desc& desc);
        static void releaseVariant(sg_pipeline pipeline);
        // a reference more on a variant acquired before (a copy of an object holding it)
        static void retainVariant(sg_pipeline pipeline);
        // the shared variant pipelines alive (tests)
        static size_t variantCount();
    };
}

#endif //SokolPipelineCache_h
