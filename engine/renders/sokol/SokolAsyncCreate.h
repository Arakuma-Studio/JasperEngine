// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolAsyncCreate_h
#define SokolAsyncCreate_h

#include "render/GpuCreation.h"

#include "sokol_gfx.h"

#include <cstdint>
#include <string>

namespace jasperengine {

    // Shaders and pipelines whose driver work runs off the render thread (spec S32): a creation
    // returns a handle at once, in sokol's ALLOC state (not VALID, not FAILED: "pending"); what
    // sokol's backend allows runs on one worker thread ("GPU Create Worker"), the rest finishes
    // in update(), once a frame, within the frame's budget. Every engine shader and pipeline is
    // made and destroyed here (SokolShader, SokolObject, SokolPipelineCache, SokolCmdQueue).
    //
    //   GL family  a worker context from the platform (GpuWorkerContextFactory) compiles and links
    //              and runs the warm draws; without one, KHR_parallel_shader_compile on the render
    //              context, else a status read two frames after the compile was issued
    //   Vulkan     pipelines on the worker, through a pipeline cache saved per device
    //   D3D11      shaders on the worker (pipelines are state objects: created at once)
    //   Metal      the completion-handler APIs
    //
    // Render thread only. A pending resource is never destroyed under a creation: its destroy waits
    // for the creation to end. JASPER_ASYNC_SHADERS=sync|worker|parallel|deferred forces a mode
    // (tests); JASPER_ASYNC_SHADERS_DELAY_MS=<n> holds every creation n ms on the worker (tests of
    // the waiting paths).
    class SokolAsyncCreate {
    public:
        // after sg_setup / before sg_shutdown
        static void setup();
        static void shutdown();
        // the warm draws done are forgotten (the render system goes: a new context reuses names)
        static void forgetWarmDraws();
        static void setSettings(const ShaderCreationSettings& settings);
        static const ShaderCreationSettings& getSettings();

        static sg_shader makeShader(const sg_shader_desc& desc);
        // a handle sg_alloc_shader made (SokolCmdQueue's MAKE_SHADER)
        static void initShader(sg_shader shader, const sg_shader_desc& desc);
        static void destroyShader(sg_shader shader);
        static bool isPending(sg_shader shader);

        // a pipeline of a pending shader waits for it in the queue
        static sg_pipeline makePipeline(const sg_pipeline_desc& desc);
        static void initPipeline(sg_pipeline pipeline, const sg_pipeline_desc& desc);
        static void destroyPipeline(sg_pipeline pipeline);
        static bool isPending(sg_pipeline pipeline);
        // created now on the render thread (the passes of the frame: ObjectRender::setImmediatePipelines),
        // its shader waited for when it is pending
        static sg_pipeline makePipelineNow(const sg_pipeline_desc& desc);
        // until the creation ended and is published (an immediate caller's interned pipeline)
        static void waitFor(sg_pipeline pipeline);
        static void waitFor(sg_shader shader);

        // GL with a worker context and warm draws on: a warm draw of the pipeline with these
        // bindings (one per driver variant: program, state, texture formats and samplers), and its
        // key; 0 when none is needed (another backend, nothing to warm, warm already)
        static uint64_t requestWarmDraw(sg_pipeline pipeline, const sg_bindings& bindings);
        static bool isWarm(uint64_t warmKey);
        // the render thread's first draw of a shader's pipeline with a warmed state: false while the
        // worker warms another state of its program (the driver's variants of a program are made
        // at the draw, never on both contexts at once); true reserves the program for this frame
        static bool beginFirstDraw(sg_shader shader);
        // warm draws can be asked for (GL worker mode with them on): false costs a draw nothing
        static bool warmDrawsActive();

        // once a frame, before the scenes draw: finishes and publishes within the budget
        static void update();
        static bool isIdle();
        // update() until idle or the timeout (tests, the benchmark's start); true when idle
        static bool waitIdle(double timeoutMs);

        static GpuCreationMode getMode();
        static GpuCreationStats getStats();
        static void resetMaxMainMs();

        // the Vulkan pipeline cache is saved now (and at shutdown, and at most once a minute
        // after new pipelines)
        static void savePipelineCache();
        // where this device's pipeline cache is kept (empty without Vulkan)
        static std::string getPipelineCachePath();
    };

}

#endif // SokolAsyncCreate_h
