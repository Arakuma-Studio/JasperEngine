// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R12, R13, R19, R37, §6.3)
// - Purpose: the GPU layer of the Title Art renderer (Pachislop's PassKit): the sokol render
//   targets of TitleTargetPool, the uploaded textures (coverage masks, LUTs, images), and one
//   pass = one fullscreen triangle of a TitleRenderProgram into a target, inside a scissor, with a
//   fixed-function blend or none. Every pass is its own render pass (CameraRender), drawn with
//   the dynamic pipeline path (ObjectRender::endLoadDynamic, beginDraw(PipelineState)), the
//   FluidSimulator pattern; pipelines are cached by PipelineState::hash() per program.
// - Programs build through ShaderPool (ShaderType::TITLE_ART, the program number in the
//   property bits): ensureProgram() is false while one compiles in the background (the job
//   waits, TitleRenderStep::ready) and false with an error when it cannot be built.
// - Addressing: a scissor names memory rows of the target (CameraRender::applyScissorTopLeft:
//   row 0 the first memory row on every backend); a pass whose scissor is not the whole target
//   loads it, so a split pass (TitleRenderJob::splitOversizedSteps) keeps the other tiles.
// - Device loss: release() drops every program and the placeholder; uploads and targets belong
//   to the renderer's caches and pool, which it clears at the same time.
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/TextureRender.h"
#include "shader/ShaderBuildTypes.h"
#include "texture/Framebuffer.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitlePrograms.h"
#include "titleart/render/TitleTargetPool.h"
#include "titleart/TitleArtTypes.h"

#include <array>
#include <map>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class ShaderRender;
}

namespace jasperengine::titleart {

    // A pool target on the GPU: a framebuffer whose colour texture passes sample
    class JASPERENGINE_API SokolTitleTarget : public TitleTarget {
    public:
        explicit SokolTitleTarget(const TargetKey& key);
        ~SokolTitleTarget() override;
        bool create(std::string* error);

        const TargetKey& key() const { return targetKey; }
        TextureRender& texture();
        TextureRender* colorTexture() const override;
        FramebufferRender& framebuffer();
        // the engine framebuffer itself: a Texture shows it (S81's display images, Texture::setFramebuffer;
        // S82's canvas shows a finished image on a sprite)
        Framebuffer& framebufferObject() { return target; }

    private:
        TargetKey targetKey;
        Framebuffer target;
    };

    class JASPERENGINE_API SokolTitleTargetAllocator : public TitleTargetAllocator {
    public:
        std::unique_ptr<TitleTarget> create(const TargetKey& key, std::string* error) override;
    };

    // An immutable texture made from CPU data (rows first to last, tightly packed)
    class JASPERENGINE_API TitleUpload {
    public:
        TitleUpload() = default;
        TitleUpload(const TitleUpload&) = delete;
        TitleUpload& operator=(const TitleUpload&) = delete;
        ~TitleUpload();

        bool create(const char* label, int width, int height, ColorFormat format, const void* data, size_t size,
                    TextureFilter filter, TextureWrap wrapU = TextureWrap::CLAMP_TO_EDGE, TextureWrap wrapV = TextureWrap::CLAMP_TO_EDGE);
        TextureRender& texture() { return render; }
        bool created() const { return made; }
        int width() const { return w; }
        int height() const { return h; }
        size_t bytes() const { return size; }

    private:
        TextureRender render;
        int w = 0, h = 0;
        size_t size = 0;
        bool made = false;
    };

    // What a scalar buffer holds, for its encoding descriptor (titleart/render/common.glsl)
    enum class TitleValueKind : uint8_t {
        Distance,       // signed distance in px (Fixed16: 1/64 px, +-512 px)
        Unit,           // 0..1 (height; Fixed16: 1/65535)
        Pixels,         // a length in px >= 0 (Fit cover radius, shape-burst depth; Fixed16: 1/16 px up to 4096)
        Index,          // an integer (the nearest glyph; Fixed16: exact up to 65535)
        Seeds           // jump-flood seeds (native RGBA32F or Fixed12_4)
    };

    // vec4(encoding, scale, offset, 0) of a buffer of `format` holding `kind`
    JASPERENGINE_API std::array<float, 4> titleEncoding(const TitleBufferFormat& format, TitleValueKind kind);

    struct TitleGpuPass {
        TitleRenderProgram program = TitleRenderProgram::Copy;
        TitleTarget* target = nullptr;
        TitleRectI scissor;                 // memory rows of the target; empty = all of it
        BlendMode blend = BlendMode::NONE;
        uint32_t variant = 0;               // property bits above the program number (TitlePasses.h's encodings)
        // p[0] as given (the effect programs' (size, px_scale) of TitlePassRunner) rather than
        // the renderer's (size, 1 / size)
        bool keepParam0 = false;
        bool clear = false;                 // clear the target to clearColor first (whole target)
        float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        TitleParams params;
        TextureRender* tex[TITLE_TEXTURE_SLOTS] = {};
        TextureRender* raw[TITLE_RAW_SLOTS] = {};
        // S83's vertex-pulling generator programs: vertices drawn (3 = the fullscreen triangle)
        // and the data texture the vertex stage reads (u_taVertexData)
        int vertices = 3;
        TextureRender* vertexData = nullptr;
    };

    struct TitleGpuStats {
        uint64_t passes = 0;            // draw passes run
        uint64_t clears = 0;            // clear-only passes
        // the longest pass (its draw holds the driver's compile of a new variant) and its program
        double slowestPassMs = 0.0;
        uint32_t slowestProgram = 0;
        bool slowestWasFirstDraw = false;   // its (program, state) key drew for the first time
    };

    // The first draw of a program in a pipeline state makes the driver compile that variant on the
    // render thread (llvmpipe's LLVM JIT: up to about 1 s for a title art program, 4-5 s for the
    // whole ELEMENT program before its variants, TitleElementVariant.h; a GL worker context,
    // S32, takes it off the render thread on hardware, not on a software rasteriser). Title Art lets
    // one new (program, state) draw per frame: the others wait for a later frame (their steps run
    // again; a 3D layer's render stops at the draw and runs again), so a render that meets many new
    // programs spreads their compiles over frames instead of holding one frame for all of them.
    // Main thread.
    struct TitleFirstDrawStats {
        uint64_t granted = 0;       // new (program, state) keys let through
        uint64_t deferred = 0;      // draws sent to a later frame
        uint64_t maxPerFrame = 0;   // the most new keys one frame let through
    };
    class JASPERENGINE_API TitleFirstDraws {
    public:
        // true when `key` (titleDrawKey) drew before, or no new key went this frame (then this one
        // goes now: the caller draws it at once)
        static bool allow(uint64_t key);
        // `key` drew before
        static bool drew(uint64_t key);
        // no new key went this frame
        static bool available();
        static const TitleFirstDrawStats& stats();
        // tests: forget every key and the counters; `unbounded` lets every new key through (the
        // red control: the frames' compiles are no longer spread)
        static void resetForTesting(bool unbounded = false);

        // While one lives, every new key goes at once: a synchronous render (one call that steps
        // its job to the end, as the editor's view_asset and bake do) has no later frame to send a
        // draw to, so spreading would wait for a frame that never comes.
        class JASPERENGINE_API SynchronousScope {
        public:
            SynchronousScope();
            ~SynchronousScope();
            SynchronousScope(const SynchronousScope&) = delete;
            SynchronousScope& operator=(const SynchronousScope&) = delete;

        private:
            bool previous;
        };
    };

    // The first draws of one render that draws everything in one call (a 3D layer's): the render
    // stops at the frame's second new program and runs again on a later frame. While it is only
    // meeting programs (`warming`, after a stop), a draw of a program that drew before is skipped,
    // so a run costs its one compile and not the passes before it (llvmpipe rasterizes a 3D main
    // pass for seconds); once a warming run reaches the end, one complete run draws the image.
    class JASPERENGINE_API TitleDrawGate {
    public:
        enum class Result { Draw, Skip, Stop };
        // what a draw of the program in `state` does in this run
        Result check(ShaderType type, uint64_t properties, const PipelineState& state);
        // a run starts / has ended: the end's answer is true when the run did not draw the image
        // (it stopped, or it was warming), so the caller runs again on a later frame
        void begin();
        bool end();
        bool stopped() const { return stop; }

    private:
        bool warming = false, stop = false, skipped = false;
    };

    class JASPERENGINE_API TitleGpu {
    public:
        TitleGpu();
        ~TitleGpu();
        TitleGpu(const TitleGpu&) = delete;
        TitleGpu& operator=(const TitleGpu&) = delete;

        // true when the program (its number and variant bits: ShaderType::TITLE_ART's
        // properties) can draw; false while it compiles (error untouched) or when it cannot be
        // built (error set). The renderer's programs and the effects' (TitlePasses.h) alike.
        bool ensureProgram(uint32_t properties, std::string* error = nullptr);
        bool ensureProgram(TitleRenderProgram program, std::string* error = nullptr) { return ensureProgram((uint32_t)program, error); }
        // one pass; false with `error` when it could not draw. A pass whose program draws for the
        // first time in this state while another program did this frame is not drawn:
        // lastRunDeferred() says so, and the step runs again on a later frame (TitleFirstDraws)
        bool run(const TitleGpuPass& pass, std::string& error);
        bool lastRunDeferred() const { return deferredRun; }
        // a target cleared to a colour (one clear pass)
        bool clear(TitleTarget* target, const float color[4], std::string& error);

        const TitleGpuStats& stats() const { return counters; }
        // drops every program (device loss, trim)
        void release();

    private:
        struct Program;
        std::map<uint32_t, std::unique_ptr<Program>> programs;     // by properties
        TextureRender placeholder;
        bool placeholderMade = false;
        CameraRender pass;
        TitleGpuStats counters;
        bool deferredRun = false;

        bool ensurePlaceholder();
    };

    // the program's ShaderPool key (TitleRenderJob::shaderKeys)
    JASPERENGINE_API ShaderKey titleRenderProgramKey(uint32_t program);

    // a draw's TitleFirstDraws key: its program (shader type and properties) in a pipeline state
    JASPERENGINE_API uint64_t titleDrawKey(ShaderType type, uint64_t properties, const PipelineState& state);

}
