// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R10-R18a, §6.3, §6.4, §6.7)
// - Purpose: TitleArtRenderer::begin's planning. A planner walks the draw tree of one request
//   (Pachislop's Compositor: content -> part effects -> layer effects -> clip group -> masks ->
//   blend and opacity onto the parent; adjustment layers on what is below them) and records the
//   flat step list of a TitleRenderJob: CPU stages (coverage rasterization) and GPU passes, each
//   a closure over the job's shared state. Decisions that need no GPU are made here, once:
//   regions of interest, cache hits, which passes run, their order; the steps then only draw.
// - Targets: a slot is one target of the job, acquired from the pool by the first step that
//   uses it and returned by the release step the planner records after its last use, so a job
//   holds only what its next steps need (R44). A slot may instead read a cached result.
// - Coordinates: a TitlePx is a slot with its rectangle in output pixels of the scaled canvas
//   (image space, rows down); the 2D title's element frames are y-up (Title2DPass.cpp).
// - Determinism (R17): nothing here reads ids, id order, timing or cache state into pixels;
//   a cached result is the bytes a fresh one would be.
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/render/DrawTree.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitleFrame.h"
#include "titleart/render/TitleJobRunner.h"
#include "titleart/render/TitleRenderJob.h"
#include "titleart/render/TitleRenderServices.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    // the state a job's steps share (TitleRenderJob::keepAlive holds it)
    class JASPERENGINE_API TitleJobState {
    public:
        TitleJobState(TitleRenderServices& services, std::shared_ptr<const TitleArtDoc> doc);

        struct Slot {
            TargetKey key;
            TargetRef ref;
            std::shared_ptr<TitleCacheEntry> entry;     // a cached result read instead of a target
            bool released = false;
        };

        TitleRenderServices& services;
        std::shared_ptr<const TitleArtDoc> doc;
        uint64_t generation;
        TitleRenderJob* job = nullptr;
        std::vector<Slot> slots;
        std::vector<std::shared_ptr<TitleCacheEntry>> held;     // what the job keeps alive
        std::vector<std::shared_ptr<void>> data;                 // CPU stage outputs

        // the target of a slot (acquired by its first use); null with `error`
        TitleTarget* target(int slot, std::string& error);
        TextureRender* texture(int slot);
        void release(int slot);
        // false with `error` after a device loss (the renderer dropped every target)
        bool alive(std::string& error) const;
    };

    // a slot and the output-pixel rectangle its target covers
    struct TitlePx {
        int slot = -1;
        TitleRectI rect;
        bool valid() const { return slot >= 0; }
    };

    // the uv map of a target (vec4(scale, offset): uv = frag * scale + offset) when a pass
    // drawing `into` (output rect) reads `from`
    std::array<float, 4> uvMap(const TitleRectI& into, const TitleRectI& from);

    // a layer's placement in the document: the composition of its ancestors' transforms and its
    // own (a group's transform is concatenated into its children's, §6.1)
    struct TitlePlacement {
        geometry::Affine toDocument;        // layer space -> document space
        double scale = 1.0;                 // the uniform scale of the composition
        double rotation = 0.0;              // deg clockwise on screen, flips folded (bevel lights stay on screen)
        bool flipX = false;
    };

    class JASPERENGINE_API TitlePlanner {
    public:
        TitlePlanner(TitleRenderServices& services, const RenderRequest& request, std::shared_ptr<const TitleArtDoc> doc);

        // the whole request as a step list (the job wires itself into state() before running)
        TitleStepList plan();
        const std::shared_ptr<TitleJobState>& state() const { return st; }
        std::vector<TitleProblem>& problems() { return found; }

        // ---- recording (used by the pass files) ----
        int newSlot(TitleBuffer buffer, int width, int height);
        int newSlot(const TargetKey& key);
        int entrySlot(const std::shared_ptr<TitleCacheEntry>& entry);
        // a GPU pass writing `slot` inside `scissor` (its rows); `fill` sets the pass's program
        // inputs from the state (textures of other slots) and returns false with an error
        using PassFill = std::function<bool(TitleJobState& state, TitleGpuPass& pass, std::string& error)>;
        void pass(const std::string& label, const std::string& layerId, int slot, const TitleRectI& scissor, TitleRenderProgram program,
                  PassFill fill, BlendMode blend = BlendMode::NONE, bool clear = false, float costWeight = 1.0f, bool splittable = true);
        void clearSlot(const std::string& label, const std::string& layerId, int slot, const float color[4]);
        void cpu(const std::string& label, const std::string& layerId, uint32_t us, std::function<bool(TitleJobState&, std::string&)> run);
        // an effect's or generator's `apply` / `render` as one step of `passes` estimated passes
        // (its passes draw through the step's TitleJobRunner)
        void kernel(const std::string& label, const std::string& layerId, const TitleRectI& rect, uint32_t passes, float costWeight,
                    std::function<bool(TitleJobState&, TitleJobRunner&, std::string&)> run);
        void release(int slot);
        // inserts a finished result into the cache when the step it follows ran
        void store(const std::shared_ptr<TitleCacheEntry>& entry, int slot);
        void hold(const std::shared_ptr<TitleCacheEntry>& entry);
        void problem(TitleProblem problem);
        TitleRenderServices& services() { return svc; }
        const TitleArtDoc& doc() const { return *document; }
        const RenderRequest& request() const { return req; }
        double pxScale() const { return scale; }
        const TitleRectI& region() const { return outRegion; }   // output px
        const TitleArtHashes& hashes() const { return docHashes; }
        uint64_t layerHash(const std::string& id, uint64_t LayerHashes::*member) const;
        // what a drawn node holds: its node hash with its drawn children and clipped layers, so a
        // group drawn for an isolated layer (or a content filter) keys apart from the whole group
        uint64_t drawnHash(const DrawNode& node) const;
        TitleFrame frameOf(const TitleRectI& rect) const;
        TitlePlacement placementOf(const Json& layer) const;

        // ---- compositing (Compositor.cpp) ----
        struct Unit {
            TitlePx px;
            TitlePx ink;            // valid while the pixels hold light and the caller wants ink
        };
        struct Content {
            TitlePx px;
            TitlePx mask;           // the content alpha a clip group reads (px itself unless part effects ran)
            TitlePx ink;
        };
        TitlePx compose(const std::vector<DrawNode>& nodes, TitlePx acc, const TitlePx* clipMask);
        std::pair<TitlePx, TitlePx> composeInk(const std::vector<DrawNode>& nodes, TitlePx acc, TitlePx ink, const TitlePx* clipMask,
                                               bool wantInk);
        Unit unit(const DrawNode& node, bool wantInk);
        Content unitContent(const DrawNode& node, bool wantMask, bool wantInk);
        TitlePx adjustOnto(TitlePx acc, const DrawNode& node, const TitlePx* clipMask);
        // src x opacity (x the clip mask's alpha) blended into dst by `mode` (TitleBlend values, 18 = disjoint)
        void blendOnto(TitlePx dst, TitlePx src, float opacity, int mode, const TitlePx* clipMask, const std::string& layerId);
        TitlePx copyOf(TitlePx src, const std::string& layerId);
        TitlePx clearedLike(const TitleRectI& rect, const std::string& layerId);
        struct Effects {
            TitlePx px;
            TitlePx ink;
        };
        // inputAlpha: what `input`'s alpha is a function of (0: not known), the key the effects'
        // alpha-only results are kept under (EffectContext::inputAlphaKey)
        Effects applyEffects(const std::string& stackId, const Json& effects, TitlePx input, bool keepInput, TitlePx inputInk,
                             bool wantInk, const Json* layer, uint64_t inputAlpha = 0);
        bool holdsLight(const DrawNode& node);
        bool contentLight(const DrawNode& node);

        // ---- regions of interest (R13) ----
        // the output rect a node's pixels (content, effects, clip group, masks) may cover
        TitleRectI nodeRect(const DrawNode& node);
        TitleRectI contentRect(const DrawNode& node);
        TitleRectI alignToAnchor(const TitleRectI& bounds, int anchorX, int anchorY) const;
        float effectsReach(const Json& effects) const;
        bool effectsFillCanvas(const Json& effects) const;

        // ---- masks (Masks.cpp) ----
        // px x the node's combined enabled masks (px itself when none draws)
        TitlePx applyMasks(const DrawNode& node, TitlePx px);
        // the combined mask target of a layer over `rect` (value in red), invalid when none
        TitlePx combinedMask(const Json& layer, const TitleRectI& rect);

        // ---- layer sources (LayerSources.cpp) ----
        // a draw tree kept alive for the planning (its nodes key the rect cache)
        const std::vector<DrawNode>& keepTree(std::vector<DrawNode> nodes);
        // the layer drawn alone (its ancestors' transforms, guides drawn), shared by every reader
        // of the job; invalid for a missing layer or a cycle (problem `cycle`)
        TitlePx layerSource(const std::string& layerId, const char* cycleProblem, const std::string& reader);
        // the same without that layer's own effects (its parts' effects kept): S83's `content`
        // input of a following generator (Pachislop's layerContent)
        TitlePx layerContentSource(const std::string& layerId, const char* cycleProblem, const std::string& reader);

        // ---- 2D titles (Title2DPass.cpp) ----
        // a 2D title's elements and their mask frame (computed once per layer and render)
        struct TitleContentPlan {
            bool valid = false;
            geometry::LayerElements elements;
            TitlePlacement place;
            double size = 0.0;          // the layer's source size (the `%` unit x 100)
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;     // the frame, layer units (y up)
            double m = 1.0;             // mask px per layer unit
            int w = 0, h = 0;           // px
            uint64_t key = 0;           // the frame and the geometry it shows
            uint64_t geometry = 0;      // the layer's geometry hash (and the element filter)
        };
        const TitleContentPlan& contentPlan(const DrawNode& node);
        // the content of a Text, Shape, Vector or Image layer (parts: the parts drawn, empty = all)
        TitlePx title2D(const DrawNode& node, const std::vector<std::string>& parts, const TitleRectI* fixedRect = nullptr);
        TitleRectI title2DRect(const DrawNode& node);
        // the same without the 64-pixel frame grid and margin: the elements' bounds and pads (S81's glyph cells)
        TitleRectI title2DInkRect(const DrawNode& node);
        // the parts of a 2D title in back-to-front order (rim:<id> outermost first, then face)
        std::vector<std::string> titleParts(const Json& layer);

#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
        // ---- 3D titles (S86, three_d/Title3DPass.cpp) ----
        // a Text, Shape, Vector or Image layer with Dimension 3D
        bool is3D(const DrawNode& node) const;
        // its content: the region's 3D render, tone mapped (parts: the part keys coloured, empty = all)
        TitlePx title3D(const DrawNode& node, const std::vector<std::string>& parts, const TitleRectI* fixedRect = nullptr);
        // its region: the projected bounds of its mesh
        TitleRectI title3DRect(const DrawNode& node);
        // its parts back to front: bands outermost first, side, bevel, face
        std::vector<std::string> title3DParts(const Json& layer);
        // an image asset of a 3D material or environment (base texture, matcap), uploaded at planning
        std::shared_ptr<TitleCacheEntry> title3DAsset(const Json& ref, const std::string& layerId, const std::string& path);
#endif

        // ---- generators ----
        TitlePx generator(const DrawNode& node);

        // ---- the effect context's helpers (FieldPasses.cpp) ----
        // the signed distance of `src`'s alpha / red coverage (frames: y-up element masks)
        TitlePx distanceField(TitlePx src, bool alphaSource, float threshold, float reachPx, const std::string& layerId,
                              std::array<float, 4>* encoding);
        TitlePx blur(TitlePx src, float radiusPx, const std::string& layerId);

        // ink of the whole render (OutputContent::Ink and Glow)
        bool inkOnly = false;
        // how many flood passes the job records (R43)
        uint32_t floodPasses = 0;
        uint32_t unboundedFloodPasses = 0;

    private:
        TitleRenderServices& svc;
        RenderRequest req;
        std::shared_ptr<const TitleArtDoc> document;
        std::shared_ptr<TitleJobState> st;
        TitleStepList steps;
        std::vector<TitleProblem> found;
        TitleArtHashes docHashes;
        double scale = 1.0;
        TitleRectI outRegion;
        std::map<std::string, const Json*> parents;     // layer id -> its parent group (null at the top)
        std::map<std::string, TitlePx> sources;         // layer sources of this job
        std::set<std::string> activeSources;
        std::string effectsOffLayer;                    // drawn without its own effects (layerContentSource)
        std::map<const DrawNode*, TitleRectI> rects;    // node rects (the trees below keep the nodes alive)
        std::map<const DrawNode*, TitleRectI> reachRects;   // the same before the ROI grid: content and reach
        TitleRectI glyphReach;                          // a glyph cell's content and reach (RenderRequest::glyphCell)
        std::map<std::string, TitleContentPlan> contentPlans;
        std::map<std::string, std::shared_ptr<void>> title3DPlans;     // S86: per layer, its Title3DPlan
        TitleElementFilter elementFilter;               // the glyph cell's, for its layer only
        std::string filteredLayer;
        std::vector<std::unique_ptr<std::vector<DrawNode>>> trees;
        // the node being planned: set when one of its effects or its generator says it read canvas
        // positions (TitleEffectContext.h), so its cached pixels are not moved with the layer
        std::shared_ptr<bool> canvasReads;

        // where an effect's or generator's target lies (the fields of TitleEffectContext.h's contexts)
        struct PassPlace {
            TitleRectI frame;
            TitleAffine documentToTarget;
            TitleAffine layerToTarget;
            TitleVec2 layerOrigin;
        };
        PassPlace passPlace(const TitleRectI& rect, const Json* layer) const;
        // the images the asset params of a definition name, uploaded at plan time (a missing one
        // is a problem, and the definition draws without it)
        using PassImages = std::vector<std::pair<TitleAssetRef, std::shared_ptr<TitleCacheEntry>>>;
        PassImages passImages(const std::vector<ParamDef>& params, const Json& values, const std::string& layerId);
        static std::function<const TextureRender*(const TitleAssetRef&, int&, int&)> imageLookup(PassImages images);

        void finishOutput(TitlePx acc);
        TitlePx planGlyphCell();
        std::shared_ptr<TitleCacheEntry> assetTexture(const Json& ref, TitleAssetKind kind, TextureWrap wrap, int blur,
                                                      const std::string& layerId, const std::string& path);
    };

}
