// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GpuCreation_h
#define GpuCreation_h

#include "Export.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine {

    // How GPU programs and pipelines are created (spec S32): Project Settings > Graphics > Shader
    // Creation (the editor reflects it, S08), set by an exported game's main
    // (Engine::setShaderCreationSettings) and applied when the render system is set up
    struct ShaderCreationSettings {
        // off: every shader and pipeline is created at once, on the render thread, as before S32
        bool asyncCreation = true;
        // the render thread's share of finishing creations, per frame (one always finishes)
        float mainThreadBudgetMs = 2.0f;
        // GL with a worker context: draw a new program and state once there before the frame uses
        // them, so the driver's first-draw compile happens on the worker
        bool glWarmDraws = true;
        // a game replaying its shader warm-up list (ShaderWarmup) reports its scenes loaded only
        // once the list is ready, so the loading screen covers it
        bool holdFirstSceneForWarmup = true;

        bool operator==(const ShaderCreationSettings& other) const {
            return asyncCreation == other.asyncCreation && mainThreadBudgetMs == other.mainThreadBudgetMs &&
                   glWarmDraws == other.glWarmDraws && holdFirstSceneForWarmup == other.holdFirstSceneForWarmup;
        }
        bool operator!=(const ShaderCreationSettings& other) const { return !(*this == other); }
    };

    enum class GpuCreationMode : uint8_t {
        Sync,           // asynchronous creation off (or forced)
        GlWorker,       // GL: a shared worker context compiles, links and warm-draws
        GlParallel,     // GL: KHR_parallel_shader_compile on the render context
        GlDeferred,     // GL: compile and link issued, the status read two frames later
        Thread,         // D3D11 shaders / Vulkan pipelines on a worker thread
        Metal           // Metal completion handlers
    };

    // "sync", "worker", "parallel", "deferred", "thread", "metal"
    JASPERENGINE_API const char* gpuCreationModeName(GpuCreationMode mode);

    struct GpuCreationStats {
        GpuCreationMode mode = GpuCreationMode::Sync;
        size_t pendingShaders = 0;
        size_t pendingPipelines = 0;
        size_t pendingWarmDraws = 0;
        uint64_t createdShaders = 0;        // published since setup (asynchronously or not)
        uint64_t createdPipelines = 0;
        uint64_t warmDraws = 0;             // warm draws the worker ran
        size_t internedPipelines = 0;       // SokolPipelineCache's shared pipelines
        double lastFrameMainMs = 0.0;       // the render thread's creation work in the last frame
        double maxMainMs = 0.0;             // the most of any frame since setup or resetMax
        double maxCreateMs = 0.0;           // the longest single creation (any thread)
    };

}

#endif /* GpuCreation_h */
