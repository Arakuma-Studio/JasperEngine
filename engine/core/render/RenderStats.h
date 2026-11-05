// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RenderStats_h
#define RenderStats_h

// What the renderer did in a frame, per view (spec S15 §6.3): the counters, the ordered pass
// list with each pass's targets, actions, draw counts and CPU/GPU time, and the GPU timer's
// state. SystemRender is the only way in (the editor and MCP never read sokol); the backend's
// pass recorder (renders/sokol/SokolPassRecorder) fills it from sokol's trace hooks.

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    // the most colour attachments a pass has (sokol's SG_MAX_COLOR_ATTACHMENTS)
    constexpr int RENDER_STATS_MAX_COLORS = 4;
    constexpr int RENDER_STATS_LABEL_LEN = 48;

    enum class PassLoadAction : uint8_t { Clear, Load, DontCare };
    enum class PassStoreAction : uint8_t { Store, DontCare };

    struct PassAttachment {
        bool used = false;
        uint32_t image = 0;         // the backend's image id (0: none)
        char label[RENDER_STATS_LABEL_LEN] = {};
        int width = 0;              // of the mip level the pass renders
        int height = 0;
        int samples = 1;
        int mip = 0;
        int slice = 0;              // cube face, array layer or 3D depth slice
        const char* format = "";    // pixel format name ("RGBA8", "DEPTH_STENCIL")
        bool cube = false;
        bool array = false;
    };

    struct PassRecord {
        uint32_t index = 0;         // in its frame (all the commits of the view), from 0
        uint64_t commit = 0;        // the commit it was in
        uint32_t viewTag = 0;
        char label[RENDER_STATS_LABEL_LEN] = {};
        bool swapchain = false;     // drawn into the window (no attachment to preview)
        bool compute = false;
        bool executed = true;       // false: sokol skipped it (validation, attachments gone)
        bool listed = false;        // begun by SokolCamera (the JASPERENGINE_FRAME_STATS pass list)
        bool hidden = false;        // the Frame Debugger's own preview blit (not in pass lists)
        bool detailed = false;      // its attachments were recorded (while a consumer wanted them)
        int width = 0;              // render area
        int height = 0;
        int samples = 1;
        PassAttachment colors[RENDER_STATS_MAX_COLORS];
        PassAttachment resolves[RENDER_STATS_MAX_COLORS];
        PassAttachment depth;
        int numColors = 0;
        PassLoadAction colorLoad[RENDER_STATS_MAX_COLORS] = {};
        PassStoreAction colorStore[RENDER_STATS_MAX_COLORS] = {};
        PassLoadAction depthLoad = PassLoadAction::Clear;
        PassStoreAction depthStore = PassStoreAction::DontCare;
        PassLoadAction stencilLoad = PassLoadAction::Clear;
        PassStoreAction stencilStore = PassStoreAction::DontCare;
        uint32_t draws = 0;
        uint32_t instances = 0;
        uint64_t elements = 0;      // indices or vertices drawn
        uint64_t triangles = 0;     // of the draws whose pipeline draws triangles
        uint32_t pipelines = 0;     // apply_pipeline calls
        uint32_t bindings = 0;      // apply_bindings calls
        uint32_t uniformBytes = 0;
        double cpuMs = 0.0;         // begin to end on the CPU (recording the commands)
        double gpuMs = 0.0;
        bool gpuValid = false;      // gpuMs is a measurement
        bool gpuPending = false;    // timed, the result has not arrived yet
        uint32_t firstDraw = 0;     // in the frame's draw list (capture only)
        uint32_t numDraws = 0;
    };

    // one draw of a pass (the draw list: only while a capture or the Frame Debugger asks for it)
    struct DrawRecord {
        char pipeline[RENDER_STATS_LABEL_LEN] = {};
        char shader[RENDER_STATS_LABEL_LEN] = {};
        uint32_t elements = 0;
        uint32_t instances = 0;
        uint32_t baseElement = 0;
    };

    // the GPU timer of the device (sokol_gpu_timer.h)
    struct GpuTimingState {
        bool available = false;
        std::string reason;         // when not available: backend, extension_missing, device_unsupported, disabled
        std::string passReason;     // passes without time though frames have it (Metal without counters)
        std::string semantics;      // pipeline_point, elapsed or encoder_span
        bool frameIsSum = false;    // frame time = the sum of the pass times (the GL family)
        bool enabled = false;       // queries are being issued now
        uint64_t resolvedCommits = 0;
        uint64_t droppedDisjoint = 0;
        uint64_t droppedOverflow = 0;
        uint64_t droppedDisabled = 0;
    };

    // the passes of one view in one frame: every commit with that view tag between two frame marks
    struct PassList {
        uint32_t viewTag = 0;
        uint64_t frame = 0;         // FrameTimes frame serial
        std::vector<uint64_t> commits;
        std::vector<PassRecord> passes;
        std::vector<DrawRecord> draws;   // when recorded
        double gpuFrameMs = 0.0;
        bool gpuFrameValid = false;
        bool gpuPending = false;    // some pass still waits for its GPU time
        uint32_t droppedPasses = 0; // passes past the GPU timer's capacity
    };

    // sokol's counters of the last commit (zero unless its statistics are on)
    struct BackendFrameCounters {
        bool valid = false;
        uint32_t passes = 0;
        uint32_t draws = 0;
        uint32_t pipelines = 0;
        uint32_t bindings = 0;
        uint32_t uniformBytes = 0;
        uint32_t imageUploadBytes = 0;
        uint32_t bufferUploadBytes = 0;
    };

    // a Frame Debugger pass output preview (SystemRender::requestPassPreview, SokolPassPreview)
    struct PassPreviewRequest {
        uint32_t viewTag = 0;
        std::string label;          // the pass's label
        uint32_t occurrence = 0;    // which pass of that label in the view's frame, from 0
        int attachment = 0;         // 0..3 colour, 4 depth
        int mode = 0;               // 0 colour, 1 alpha, 2 depth, 3 red, 4 green, 5 blue
        int maxSize = 1024;         // the preview's longest side
        float nearPlane = 0.0f;     // depth mode: linearised when far > near > 0
        float farPlane = 0.0f;
        float depthMin = 0.0f;      // else this depth range maps to black..white
        float depthMax = 1.0f;
    };

    struct PassPreviewResult {
        uint64_t serial = 0;        // counts the previews made (and refusals)
        bool ready = false;         // the target holds the request's pass
        bool unavailable = false;   // the pass matched but cannot be previewed (see reason)
        std::string reason;
        uint64_t frame = 0;         // FrameTimes frame of the pass
        PassPreviewRequest request;
        int width = 0;              // the preview's size
        int height = 0;
        int sourceWidth = 0;        // the attachment's (at its mip level)
        int sourceHeight = 0;
        std::string format;
        bool resolved = false;      // read from the resolve image of an MSAA attachment
    };

    // the counters of one view's frame
    struct RenderStats {
        uint32_t viewTag = 0;
        uint64_t frame = 0;
        bool valid = false;         // the view drew in a recorded frame
        // Engine::FrameStats of the game frame (views add nothing to them)
        uint32_t drawCalls = 0;
        uint32_t instances = 0;
        uint64_t triangles = 0;
        uint64_t vertices = 0;      // indices or vertices the draws ran (the frame's: times their instances)
        // from the pass list: this view's frame
        uint32_t passes = 0;
        uint32_t draws = 0;
        uint32_t pipelinesApplied = 0;
        uint32_t bindingsApplied = 0;
        uint32_t uniformBytes = 0;
        // sokol's statistics of the last commit (zero unless they are on)
        uint32_t imageUploadBytes = 0;
        uint32_t bufferUploadBytes = 0;
        // alive resources
        uint32_t aliveImages = 0;
        uint32_t aliveBuffers = 0;
        uint32_t alivePipelines = 0;
        uint32_t aliveViews = 0;
        uint32_t aliveShaders = 0;
        uint32_t aliveSamplers = 0;
        // the dynamic pipeline cache
        uint64_t pipelineCacheMisses = 0;
        uint64_t pipelineCacheApplies = 0;
        size_t pipelineCacheSize = 0;
        double gpuFrameMs = 0.0;
        bool gpuFrameValid = false;
        // by kind of pass, from the pass list (spec S55 R7): the colour passes of the view's
        // cameras ("camera", "camera layer"), the shadow maps ("shadow", "point shadow" and their
        // static layers), the depth passes ("depth pre-pass", "gbuffer") and every other one
        uint32_t mainDraws = 0;
        uint32_t shadowDraws = 0;
        uint32_t depthDraws = 0;
        uint32_t otherDraws = 0;
        uint64_t mainTriangles = 0;
        uint64_t shadowTriangles = 0;
        // automatic GPU instancing (DrawBatchStats) of the last committed frame
        uint32_t instancedDraws = 0;
        uint32_t batchedInstances = 0;
        uint64_t uniformBytesKept = 0;
        // the frame's visible lists (RenderSystem): mesh draws kept and culled, every pass
        uint32_t visible = 0;
        uint32_t culled = 0;
    };

    // automatic GPU instancing and the state not applied again in a frame (spec S55)
    struct DrawBatchStats {
        uint32_t instancedDraws = 0;    // instanced draws made of batches
        uint32_t batchedInstances = 0;  // the draws they replaced
        uint64_t uniformBytesKept = 0;  // uniform bytes already in their slot, not applied again
        uint32_t pipelinesKept = 0;     // pipelines already applied, not applied again
        uint64_t instanceBytes = 0;     // the batches' instance data
        // draws that could batch, and why one did not join the batch before it (another pipeline,
        // elements, bindings or uniform bytes)
        uint32_t offeredDraws = 0;
        uint32_t splitPipeline = 0;
        uint32_t splitElements = 0;
        uint32_t splitBindings = 0;
        uint32_t splitUniforms = 0;
        uint32_t splitUniformSlots = 0;  // the uniform slots whose bytes differed (bit 31: other blocks)
        uint32_t splitBindingParts = 0;  // 1 vertex buffers, 2 their offsets, 4 index buffer, 8 views, 16 samplers
    };

}

#endif /* RenderStats_h */
