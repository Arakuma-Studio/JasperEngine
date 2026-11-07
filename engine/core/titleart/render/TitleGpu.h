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
        // the engine framebuffer itself: a Texture shows it (S81's display images, Texture::setFramebuffer)
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
    };

    struct TitleGpuStats {
        uint64_t passes = 0;            // draw passes run
        uint64_t clears = 0;            // clear-only passes
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
        // one pass; false with `error` when it could not draw
        bool run(const TitleGpuPass& pass, std::string& error);
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

        bool ensurePlaceholder();
    };

    // the program's ShaderPool key (TitleRenderJob::shaderKeys)
    JASPERENGINE_API ShaderKey titleRenderProgramKey(uint32_t program);

}
