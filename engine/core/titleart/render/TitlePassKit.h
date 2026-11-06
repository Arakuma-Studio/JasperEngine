// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.3, R37)
// - Purpose: the TitlePassRunner that draws at once on the engine's render layer (sokol-gfx,
//   every backend): pooled targets (TitleSokolTargetAllocator: colour only, one sample, clamped),
//   the TITLE_ART programs through ShaderPool with dynamic pipelines (ObjectRender::
//   endLoadDynamic, beginDraw(PipelineState)), lookups uploaded once per content, one render
//   pass per draw with its viewport and scissor. Tests and tools run effects and generators with
//   it; the renderer's job wraps the same calls as its GPU steps.
// - Programs compile in the background (the runtime compiler, or a build's precompiled
//   variants): prepare() requests one and says when it is ready; run() fails with "being
//   compiled" until it is, so callers warm up first (TitleRenderJob::shaderKeys).
// - Device loss: reset() drops every pipeline object, lookup and target (their handles read
//   null); the shaders stay in ShaderPool, which owns them.
// - Thread-safety: main thread (it records draw calls).

#pragma once

#include "Export.h"
#include "titleart/render/TitlePasses.h"

#include <memory>
#include <string>

namespace jasperengine {
    class TextureRender;
}

namespace jasperengine::titleart {

    // the GPU objects of a pooled target: colour attachment only
    class JASPERENGINE_API TitleSokolTargetAllocator : public TitleTargetAllocator {
    public:
        std::unique_ptr<TitleTarget> create(const TargetKey& key, std::string* error) override;
    };

    // the colour texture of a target the sokol allocator made (null for any other)
    JASPERENGINE_API TextureRender* titleTargetTexture(const TargetRef& target);

    class JASPERENGINE_API TitlePassKit : public TitlePassRunner {
    public:
        // a kit with its own pool of `poolBytes`
        TitlePassKit(const TitleArtCaps& caps, size_t poolBytes = 512u << 20);
        ~TitlePassKit() override;
        TitlePassKit(const TitlePassKit&) = delete;
        TitlePassKit& operator=(const TitlePassKit&) = delete;

        const TitleArtCaps& caps() const override;
        TargetRef* acquire(const TargetKey& key) override;
        void release(TargetRef* target) override;
        const TitleLut* lut(const TitleLutData& data) override;
        bool run(const TitlePass& pass) override;
        bool clear(TargetRef& target) override;
        const std::string& error() const override;
        void clearError() override;

        // requests the program variant; true once it can draw
        bool prepare(TitleProgram program, uint64_t variant);
        // a target of the accumulator format (TitleArtCaps) at this size
        TargetKey accumulatorKey(int width, int height) const;
        // passes drawn since construction
        uint64_t passes() const;
        TitleTargetPool& pool();
        // drops every program, lookup and target (device loss, shutdown)
        void reset();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}
