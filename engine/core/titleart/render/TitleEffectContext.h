// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.4 "Ink vs light", §6.5, R30; Pachislop's
// EffectContext and the helpers of src/effects/lib.ts)
// - Purpose: what an effect definition's `apply` and a generator definition's `render`
//   (TitleEffectRegistry.h) run with inside a render job: the node's target geometry (its
//   region of interest in output pixels, px_scale, the canvas), the layer's content and ink
//   (Pachislop's content, ink and inputInk), the document and the layer, and the GPU helpers
//   every kernel shares: targets from the pool, passes of any TitleRenderProgram (the definitions'
//   own programs above TITLE_RENDER_PROGRAM_LAST), copies, the pyramid Gaussian blur, the
//   alpha distance field bounded by the reach its consumer needs, gradient LUTs and the
//   fourteen adjustment operations (one implementation for the `adjust` effect and adjustment
//   layers, R30).
// - Pixel contract: every target is linear premultiplied colour of the accumulator format
//   (TitleArtCaps: RGBA16F, or SRGB8A8 on the fallback), rows down from the node's top-left; the
//   output target is lent sized like the input and must be written whole. Effect px params are
//   canvas px: multiply by pxScale (decision 5).
// - Position: a node whose pixels do not depend on where it sits is re-placed from the cache
//   when it moves by whole output pixels (R42); a kernel that reads canvas positions (an
//   `align: canvas` pattern) sets usesCanvasPosition, and the node is then re-rendered on moves.
// - Failure: a helper returns false with error() set; `apply` returning leaves the output as it
//   drew it, and a set error fails the job.
// - Thread-safety: main thread, during one step of a job.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleFrame.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/render/TitleTargetPool.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleRenderServices;
    struct TitleCacheEntry;

    class JASPERENGINE_API TitlePassContext {
    public:
        TitlePassContext(TitleRenderServices& services, const TitleArtDoc& doc, const Json* layer, std::string layerId,
                         const TitleRectI& rect, double pxScale);
        virtual ~TitlePassContext() = default;

        // ---- where the pass draws ----
        const TitleArtDoc& doc() const { return document; }
        const Json* layer() const { return layerObject; }     // null for document-level passes
        const std::string& layerId() const { return id; }     // "<id>#<part>" for a part's stack
        // the target's rectangle in output pixels of the scaled canvas (image space)
        const TitleRectI& rect() const { return area; }
        int width() const { return area.width; }
        int height() const { return area.height; }
        double pxScale() const { return scale; }
        // the document <-> output-pixel map (TitleFrame), with this target's rect as its region
        TitleFrame frame() const;
        // the document's `seed` (decision 8: add a definition's own `seed` param to it)
        int64_t seed() const;
        const TitleArtCaps& caps() const;

        // ---- GPU helpers ----
        TitleGpu& gpu();
        // a target of this pass's size (or the given one) for a buffer kind; empty with error() set
        TargetRef acquire(TitleBuffer buffer = TitleBuffer::Accumulator, int width = 0, int height = 0);
        static TextureRender* texture(const TargetRef& target);
        // one pass of `pass.program` (TitleGpu::run), counted as a pass of the job
        bool run(const TitleGpuPass& pass);
        bool clear(TargetRef& target);
        // src copied into dst (both this pass's size, or resampled when they differ)
        bool copy(const TargetRef& src, TargetRef& dst, float opacity = 1.0f);
        // Gaussian of radius ~ 3 sigma = radiusPx output px: a separable pass of at most 24 taps a
        // side on the pyramid level where sigma / 2^level <= 6 (Pachislop's lib.ts blur)
        bool blur(const TargetRef& src, float radiusPx, TargetRef& out);
        // signed distance (px, + outside) to src's alpha edge at `threshold`, jump-flooded only as
        // far as reachPx (R27: steps from 2^ceil(log2(reach + 2)) / 2 down to 1, plus one); beyond
        // it the field reads reachPx. `out` takes the Distance buffer (texelFetch, TitleBuffer::Distance)
        bool distanceField(const TargetRef& src, float threshold, float reachPx, TargetRef& out);
        // the encoding descriptor of a Distance target this context made (titleart/common.glsl)
        std::array<float, 4> distanceEncoding() const;
        // a 256 x 1 RGBA8 LUT of a gradient (sRGB stops, straight alpha; cached for the render)
        TextureRender* gradientLut(const TitleGradient& gradient);
        // one of the fourteen adjustment operations of an adjustment object (render/Adjustments.h)
        bool adjust(const TargetRef& src, const Json& adjustment, TargetRef& out);

        // ---- position, failure ----
        bool usesCanvasPosition = false;
        bool fail(const std::string& message);
        const std::string& error() const { return failure; }

    protected:
        TitleRenderServices& services;
        const TitleArtDoc& document;
        const Json* layerObject;
        std::string id;
        TitleRectI area;
        double scale;
        std::string failure;
        std::vector<std::shared_ptr<TitleCacheEntry>> keep;     // LUTs the pass's draws read
    };

    // apply(EffectContext&, input, output, params)
    struct JASPERENGINE_API EffectContext : public TitlePassContext {
        using TitlePassContext::TitlePassContext;
        // the layer's (group's, part's) pixels before any of its effects: shapes an effect traces
        // from the silhouette come from here, so an earlier glow does not move them
        const TargetRef* content = nullptr;
        // `content` without the light its renderer added (the same target when it has none)
        const TargetRef* ink = nullptr;
        // the input without the light of earlier light effects: outline effects read it (§6.4)
        const TargetRef* inputInk = nullptr;
    };

    // render(GeneratorContext&, target, params): the target is this context's rect (the canvas
    // region for fills, the followed layer's region otherwise)
    struct JASPERENGINE_API GeneratorContext : public TitlePassContext {
        using TitlePassContext::TitlePassContext;
        // the followed layer drawn alone (Generator `follow`), null when none or a cycle
        const TargetRef* follow = nullptr;
    };

}
