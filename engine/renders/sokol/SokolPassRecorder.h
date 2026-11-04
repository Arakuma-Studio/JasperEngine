// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolPassRecorder_h
#define SokolPassRecorder_h

// The frame pass recorder (spec S15 R10, R11): from sokol's trace hooks (SokolTraceHub) it
// records every pass of every commit in order (label, view tag, attachments, actions, draws,
// instances, elements, pipelines and bindings applied, uniform bytes, CPU time), keeps the
// commits of the last frames, and joins the GPU timer's results into them when they arrive.
// It also keeps the JASPERENGINE_FRAME_STATS pass list (SokolSystem::getFrameStatsLine), which
// lists the passes SokolCamera begins, as before the recorder existed. Main thread only (the
// thread that calls sokol).

#include "render/RenderStats.h"

#include <cstdint>
#include <string>

struct sg_pass;
struct sg_pipeline_desc;
struct sg_shader_desc;
struct sgj_commit_times;

namespace jasperengine{

    class SokolPassRecorder{
    public:
        static void setEnabled(bool enabled);
        static bool isEnabled();

        // the view tag of the commits from now (SystemRender::setViewTag)
        static void setViewTag(uint32_t tag);
        static uint32_t getViewTag();

        // SokolCamera begins `pass` next: it goes in the frame stats' pass list (recorded only
        // with JASPERENGINE_FRAME_STATS, setFrameStatsListing)
        static void listPass(const sg_pass& pass);
        static void setFrameStatsListing(bool listing);
        // the next pass is the Frame Debugger's preview blit: timed (the GPU timer sees it) but
        // left out of the pass lists
        static void hideNextPass();
        // the pass that ended last (SokolPassPreview, right after sg_end_pass); null when none
        static const PassRecord* lastEndedPass();
        // the draw list (R11) of the passes from now (a capture, the Frame Debugger)
        static void setDrawListEnabled(bool enabled);
        static bool isDrawListEnabled();
        // the next draw is a batch of `items` objects whose first did not join the draw before
        // it for `reason` (DrawRecord::batchItems, batchBreak); only kept with the draw list
        static void annotateNextDraw(uint16_t items, uint8_t reason);

        // trace hooks (SokolTraceHub)
        static void onMakePipeline(uint32_t pipeline, const sg_pipeline_desc* desc);
        static void onDestroyPipeline(uint32_t pipeline);
        static void onMakeShader(uint32_t shader, const sg_shader_desc* desc);
        static void onDestroyShader(uint32_t shader);
        static void onBeginPass(const sg_pass* pass);
        static void onApplyPipeline(uint32_t pipeline);
        static void onApplyBindings();
        static void onApplyUniforms(size_t bytes);
        static void onDraw(int baseElement, int numElements, int numInstances);
        static void onEndPass();
        static void onCommit();

        // a commit's GPU times (SokolSystem, after each commit)
        static void onGpuTimes(const sgj_commit_times& times);

        // the passes of `viewTag` in the newest recorded frame that drew it; with
        // `resolvedOnly`, in the newest one whose GPU times have all arrived when one has
        static PassList getPassList(uint32_t viewTag, bool resolvedOnly);
        // the view tags of the commits recorded in the last frames
        static std::vector<uint32_t> getRecordedViewTags();
        // the GPU time of `viewTag`'s commits in `frame`, once they all resolved
        static bool getFrameGpuMs(uint32_t viewTag, uint64_t frame, double& gpuMs);

        // JASPERENGINE_FRAME_STATS: the last commit's SokolCamera passes in order, repeats
        // folded ("shadow 2048x2048 x3"), "msaa N" for a multisampled target, and with
        // JASPERENGINE_FRAME_STATS_ACTIONS the colour load and depth store actions
        static std::string getFrameStatsPassList();

        static void clear();
    };

}

#endif //SokolPassRecorder_h
