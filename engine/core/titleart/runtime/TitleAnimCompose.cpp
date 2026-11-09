// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimCompose.h"

#include "render/TextureRender.h"
#include "render/SystemRender.h"
#include "titleart/render/TitleArtCaps.h"
#include "util/TitleArtBakeInfo.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "titleart/geometry/Path.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleFrame.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/render/TitlePrograms.h"
#include "titleart/render/TitleRenderJob.h"
#include "titleart/runtime/TitleGlyphAtlas.h"
#include "titleart/text/TitleTextLayout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    // quads per pass of GLYPH_COMPOSE_ANIM (6 vec4s each after p[2])
    constexpr int QUADS_PER_PASS = 7;
    constexpr uint32_t COMPOSE_US = 200;

    void setMap(TitleGpuPass& pass, int index, float sx, float sy, float ox, float oy){
        pass.params.set(index, sx, sy, ox, oy);
    }

    bool chainTo(const Json& layers, const std::string& id, std::vector<const Json*>& chain){
        for (const Json& layer : layers){
            if (!layer.is_object()) continue;
            chain.push_back(&layer);
            if (layer.value("id", std::string()) == id) return true;
            if (chainTo(layerChildren(layer), id, chain)) return true;
            chain.pop_back();
        }
        return false;
    }

    geometry::Affine localOf(const Json& layer){
        const TitleTransform t = decodeTransform(layer.value("transform", Json::object()));
        geometry::Affine local = geometry::Affine::scale((t.flipX ? -1.0 : 1.0) * t.scale * t.stretch.x, (t.flipY ? -1.0 : 1.0) * t.scale * t.stretch.y);
        local = geometry::Affine::rotateDegrees(-t.rotation).after(local);
        return geometry::Affine::translate(t.position.x, t.position.y).after(local);
    }

    geometry::Affine chainAffine(const std::vector<const Json*>& chain, const Json* lastOverride){
        geometry::Affine m;
        for (size_t i = 0; i < chain.size(); i++) m = m.after(localOf(i + 1 == chain.size() && lastOverride ? *lastOverride : *chain[i]));
        return m;
    }

    geometry::Affine inverseOf(const geometry::Affine& m){
        const double det = m.a * m.d - m.b * m.c;
        if (std::abs(det) < 1e-12) return geometry::Affine();
        geometry::Affine out;
        out.a = m.d / det;
        out.b = -m.b / det;
        out.c = -m.c / det;
        out.d = m.a / det;
        out.e = (m.c * m.f - m.d * m.e) / det;
        out.f = (m.b * m.e - m.a * m.f) / det;
        return out;
    }

    // a layer moved by its pose, as a move of the frame's pixels
    void layerMove(const TitleFrame& frame, const std::vector<const Json*>& chain, const Json& posed, double move[6]){
        const geometry::Affine rest = chainAffine(chain, nullptr), now = chainAffine(chain, &posed);
        const geometry::Affine toDocument = inverseOf(rest);
        auto map = [&](double x, double y){
            const TitleVec2 doc = frame.toDocument(TitleVec2{x, y});
            return frame.toFrame(now.apply(toDocument.apply(doc)));
        };
        const TitleVec2 o = map(0.0, 0.0), ax = map(1.0, 0.0), ay = map(0.0, 1.0);
        move[0] = ax.x - o.x;
        move[1] = ay.x - o.x;
        move[2] = o.x;
        move[3] = ax.y - o.y;
        move[4] = ay.y - o.y;
        move[5] = o.y;
    }

    // a colour field of a pose: "#rrggbb", [r, g, b] or a component ("tint.r")
    void applyColor(const std::string& path, const Json& value, float* color){
        float v[3] = {1.0f, 1.0f, 1.0f};
        size_t width = 0;
        if (!titleanim::numericFromJson(value, v, width)) return;
        if (width == 1){
            const char c = path.back();
            color[c == 'r' ? 0 : c == 'g' ? 1 : 2] = v[0];
        }else{
            for (size_t i = 0; i < 3 && i < width; i++) color[i] = v[i];
        }
    }

    void toLinear(float* color){
        for (int i = 0; i < 3; i++) color[i] = srgbToLinear(std::max(0.0f, color[i]));
    }

}

bool jasperengine::titleart::drawTitlePosedQuads(TitleGpu& gpu, TitleTarget* target, const std::vector<TitlePosedQuad>& quads, int width, int height,
                                                 uint32_t& passes, std::string& error){
    for (size_t i = 0; i < quads.size();){
        TextureRender* texture = quads[i].texture;
        size_t end = i;
        while (end < quads.size() && quads[end].texture == texture && end - i < (size_t)QUADS_PER_PASS) end++;
        TitleGpuPass pass;
        pass.program = TitleRenderProgram::GlyphComposeAnim;
        pass.target = target;
        pass.scissor = {0, 0, width, height};
        pass.blend = BlendMode::PREMULTIPLIED;
        pass.tex[0] = texture;
        pass.params.set(2, (float)(end - i));
        for (size_t q = i; q < end; q++){
            const TitlePosedQuad& quad = quads[q];
            const int slot = 3 + 6 * (int)(q - i);
            const double* m = quad.move;
            const double det = m[0] * m[4] - m[1] * m[3];
            if (std::abs(det) < 1e-9 || quad.opacity <= 0.0f || quad.width <= 0 || quad.height <= 0){
                pass.params.set(slot, 0.0f, 0.0f, 0.0f, 0.0f);     // a flat or hidden quad draws nothing
                continue;
            }
            double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
            for (int corner = 0; corner < 4; corner++){
                const double cx = quad.x0 + ((corner & 1) ? quad.width : 0), cy = quad.y0 + ((corner & 2) ? quad.height : 0);
                const double px = m[0] * cx + m[1] * cy + m[2], py = m[3] * cx + m[4] * cy + m[5];
                x0 = std::min(x0, px);
                x1 = std::max(x1, px);
                y0 = std::min(y0, py);
                y1 = std::max(y1, py);
            }
            pass.params.set(slot, (float)std::floor(x0), (float)std::floor(y0), (float)std::ceil(x1) + 1.0f, (float)std::ceil(y1) + 1.0f);
            pass.params.set(slot + 1, quad.uv[0], quad.uv[1], quad.uv[2], quad.uv[3]);
            const double w = 1.0 / quad.width, h = 1.0 / quad.height;
            const double ia = m[4] / det, ib = -m[1] / det, ic = -m[3] / det, id = m[0] / det;
            const double ix = -(ia * m[2] + ib * m[5]) - quad.x0, iy = -(ic * m[2] + id * m[5]) - quad.y0;
            pass.params.set(slot + 2, (float)(ia * w), (float)(ib * w), (float)(ix * w), quad.opacity);
            pass.params.set(slot + 3, (float)(ic * h), (float)(id * h), (float)(iy * h), 0.0f);
            pass.params.set(slot + 4, quad.tint[0], quad.tint[1], quad.tint[2], 0.0f);
            pass.params.set(slot + 5, quad.flash[0], quad.flash[1], quad.flash[2], 0.0f);
        }
        passes++;
        if (!pass.tex[0]){
            error = "a composed title's texture is gone";
            return false;
        }
        if (!gpu.run(pass, error)) return false;
        i = end;
    }
    return true;
}

void jasperengine::titleart::titleGlyphMove(const GlyphPose& g, const TitleRect& inkBox, const TitleVec2& pen, double layerSize,
                                            const std::function<TitleVec2(const TitleVec2&)>& toOutput, double move[6]){
    double lx = inkBox.x + inkBox.width * 0.5, ly = inkBox.y + inkBox.height * 0.5;
    if (g.pivot == titleanim::TrackPivot::Bottom) ly = inkBox.y;
    else if (g.pivot == titleanim::TrackPivot::Top) ly = inkBox.y + inkBox.height;
    else if (g.pivot == titleanim::TrackPivot::Baseline) ly = pen.y;
    const TitleVec2 pivot = toOutput({lx, ly});
    const TitleVec2 moved = toOutput({lx + g.offset[0] * layerSize / 100.0, ly + g.offset[1] * layerSize / 100.0});
    const double radians = g.rotation * 3.14159265358979323846 / 180.0;
    const double cs = std::cos(radians), sn = std::sin(radians);
    move[0] = cs * g.scale[0];
    move[1] = -sn * g.scale[1];
    move[3] = sn * g.scale[0];
    move[4] = cs * g.scale[1];
    move[2] = moved.x - (move[0] * pivot.x + move[1] * pivot.y);
    move[5] = moved.y - (move[3] * pivot.x + move[4] * pivot.y);
}

namespace {

    class ComposeJob : public TitleJob {
    public:
        ComposeJob(TitleArtRenderer& renderer, const AssetResolver* assets, const TitleJobSpec& spec)
            : renderer(renderer), assets(assets), spec(spec), atlas(spec.atlas){
            renderFlags = spec.testFlags & ~(TITLE_ART_TEST_GLYPH_STRING_ORDER | TITLE_ART_TEST_FAIL_TARGETS | TITLE_ART_TEST_SLOW_STEPS);
            if (titleArtForceRgba8()) renderFlags |= TITLE_ART_TEST_NO_FLOAT_TARGETS;
            caps = TitleArtCaps::select(SystemRender::getFeatures(), renderFlags);
            path = caps.pathName();
            atlas->releaseUploads();
            prepare();
        }

        bool step(const StepBudget&, StepReport* report) override{
            if (!failure.empty() || finished) return finished;
            StepReport local;
            const auto start = std::chrono::steady_clock::now();
            if (!firstDrawsGranted){
                // as the Glyph Atlas job: a skipped pass here would be a hole, so every state at once
                if (!TitleFirstDraws::allowGroup(firstDrawKeys())){
                    blocked = true;
                    if (report) *report = local;
                    return false;
                }
                firstDrawsGranted = true;
                blocked = false;
            }
            if (sub){
                sub->step(StepBudget{1, 0}, &local);
                if (sub->failed()) fail(sub->error());
                else if (sub->done()) finishSub();
                blocked = sub && sub->waiting() && local.steps == 0;
            }else if (!startNext()){
                if (compose(local)) finished = true;
            }
            local.measuredUs = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
            if (local.steps == 0 && !blocked) local.steps = 1;
            if (report) *report = local;
            return finished;
        }

        bool done() const override{ return finished; }
        bool failed() const override{ return !failure.empty(); }
        bool waiting() const override{ return blocked; }
        float progress() const override{
            const size_t total = std::max<size_t>(1, tasks.size() + 1);
            return std::min(1.0f, (float)(nextTask + (finished ? 1 : 0)) / (float)total);
        }
        uint32_t nextStepCostUs() const override{ return finished ? 0 : sub ? sub->nextStepCostUs() : COMPOSE_US; }
        void splitOversizedSteps(uint32_t maxUs) override{ splitLimit = maxUs; }
        uint32_t passesDone() const override{ return passesRun + (sub ? sub->passesDone() : 0); }
        uint32_t passesTotal() const override{ return passesRun + (sub ? sub->passesTotal() : 0) + (finished ? 0 : 4); }
        const std::string& error() const override{ return failure; }
        bool allocationFailure() const override{ return allocation; }
        bool floatTargetFailure() const override{ return false; }
        const char* pathName() const override{ return path; }

        TitleDisplayImage takeImage() override{
            TitleDisplayImage out;
            if (!finished) return out;
            out.owner = atlas;
            out.width = composeRect.width;
            out.height = composeRect.height;
            out.frame = frameRect;
            out.bytes = targetBytes(display.key());
            out.target = std::move(display);
            return out;
        }

        std::vector<ShaderKey> shaderKeys() const override{
            std::vector<ShaderKey> keys = subKeys;
            for (TitleRenderProgram p : {TitleRenderProgram::Copy, TitleRenderProgram::GlyphComposeAnim, TitleRenderProgram::Output}){
                const ShaderKey k = titleRenderProgramKey((uint32_t)p);
                if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
            }
            return keys;
        }

    private:
        enum class ItemKind { Plate, Layer, Glyphs };
        struct Item {
            ItemKind kind = ItemKind::Plate;
            std::vector<std::string> layers;    // Plate: its layers; Layer, Glyphs: the one layer
            uint64_t key = 0;                   // Plate, Layer: the image's
        };
        struct Task {
            enum class Kind { Plate, Cell } kind = Kind::Plate;
            std::vector<std::string> layers;
            std::vector<FieldOverride> overrides;
            uint64_t key = 0;
            std::string cellKey;
            std::string layer;
            uint32_t cluster = 0;
            bool first = false;     // the canvas background is the first plate's
        };
        struct PlacedGlyph {
            std::string key;
            const GlyphPose* pose = nullptr;
            double move[6] = {1, 0, 0, 0, 1, 0};
        };

        TitleArtRenderer& renderer;
        const AssetResolver* assets;
        TitleJobSpec spec;
        std::shared_ptr<TitleGlyphAtlas> atlas;
        TitleArtCaps caps;
        uint32_t renderFlags = 0;
        const char* path = "rgba8";
        std::string failure;
        bool allocation = false;
        bool finished = false;
        bool blocked = false;
        bool firstDrawsGranted = false;
        uint32_t splitLimit = 0;
        uint32_t passesRun = 0;
        std::vector<ShaderKey> subKeys;

        TitleFrame canvasFrame;
        TitleRectI composeRect, frameRect;
        uint64_t styleHash = 0;
        std::vector<Item> items;
        std::map<std::string, std::vector<PlacedGlyph>> glyphs;     // by text layer, in cluster order
        std::vector<Task> tasks;
        size_t nextTask = 0;
        std::unique_ptr<TitleRenderJob> sub;
        const Task* running = nullptr;
        std::set<std::string> inUse;

        TargetRef accumulator, display;

        void fail(const std::string& message){
            failure = message;
            allocation = failure.find("out_of_target_memory") != std::string::npos;
        }

        std::vector<uint64_t> firstDrawKeys() const{
            TargetKey rgba;
            rgba.format = ColorFormat::RGBA;
            TargetKey acc;
            acc.format = caps.format(TitleBuffer::Accumulator).format;
            auto key = [](TitleRenderProgram p, const TargetKey& t, BlendMode b){ return TitleGpu::firstDrawKey((uint32_t)p, t, b); };
            return {key(TitleRenderProgram::Copy, rgba, BlendMode::NONE),
                    key(TitleRenderProgram::Copy, acc, BlendMode::PREMULTIPLIED),
                    key(TitleRenderProgram::GlyphComposeAnim, acc, BlendMode::PREMULTIPLIED),
                    key(TitleRenderProgram::Output, rgba, BlendMode::NONE)};
        }

        uint64_t keyOf(const std::string& what) const{ return hashCombine(styleHash, fnv1a64(what)); }

        bool isMoving(const Json& layer) const{
            const std::string id = layer.value("id", std::string());
            return spec.compose->layers.count(id) > 0 || spec.compose->glyphs.count(id) > 0;
        }

        bool containsMoving(const Json& group) const{
            for (const Json& child : layerChildren(group))
                if (isMoving(child) || containsMoving(child)) return true;
            return false;
        }

        // draw order: the runs of still layers around each layer that moves
        void walk(const Json& siblings, std::vector<std::string>& run){
            auto flush = [&](){
                if (run.empty()) return;
                Item plate;
                plate.kind = ItemKind::Plate;
                plate.layers = run;
                plate.key = keyOf("plate|" + joinIds(run));
                items.push_back(plate);
                run.clear();
            };
            for (const Json& layer : siblings){
                if (!layer.is_object()) continue;
                const std::string id = layer.value("id", std::string());
                if (isMoving(layer)){
                    flush();
                    Item item;
                    item.kind = spec.compose->glyphs.count(id) ? ItemKind::Glyphs : ItemKind::Layer;
                    item.layers = {id};
                    item.key = keyOf("layer|" + id);
                    items.push_back(item);
                }else if (containsMoving(layer)){
                    walk(layerChildren(layer), run);
                }else{
                    run.push_back(id);
                }
            }
            if (&siblings == &layersOf(spec.doc->root)) flush();
        }

        static std::string joinIds(const std::vector<std::string>& ids){
            std::string out;
            for (const std::string& id : ids) out += id + ",";
            return out;
        }

        const Json& layersOf(const Json& root) const{
            static const Json empty = Json::array();
            auto it = root.find("layers");
            return it != root.end() ? *it : empty;
        }

        void prepare(){
            const TitleArtDoc& doc = *spec.doc;
            const int cw = doc.canvasWidth(), ch = doc.canvasHeight();
            canvasFrame = TitleFrame::canvas(cw, ch, std::max(1e-3, (double)spec.pxScale));
            const TitleRectI canvasPx{0, 0, cw, ch};
            frameRect = spec.region.empty() ? canvasPx : intersectRect(spec.region, canvasPx);
            composeRect = intersectRect(canvasFrame.canvasToFrame(frameRect), canvasFrame.region);
            if (composeRect.empty()){
                fail("the title's region is empty");
                return;
            }
            uint64_t values = 0;
            for (const FieldOverride& o : spec.overrides) values = hashCombine(values, fnv1a64(o.layerId + "|" + o.path + "|" + o.value.dump()));
            styleHash = hashCombine(hashCombine(spec.docHash, values), (uint64_t)std::llround((double)spec.pxScale * 1000.0));
            styleHash = hashCombine(styleHash, ((uint64_t)(uint32_t)composeRect.x << 32) ^ (uint32_t)composeRect.y);

            std::vector<std::string> run;
            walk(layersOf(doc.root), run);
            for (const Item& item : items){
                if (item.kind == ItemKind::Plate && !atlas->plate(item.key)){
                    Task t;
                    t.kind = Task::Kind::Plate;
                    t.layers = item.layers;
                    t.overrides = spec.overrides;
                    t.key = item.key;
                    t.first = &item == &items.front();
                    tasks.push_back(t);
                }else if (item.kind == ItemKind::Layer && !atlas->plate(item.key)){
                    Task t;
                    t.kind = Task::Kind::Plate;
                    t.layers = item.layers;
                    t.overrides = spec.overrides;
                    t.key = item.key;
                    // the quad applies these: the split holds the layer at rest
                    for (const auto& field : spec.compose->layers.at(item.layers[0])){
                        const std::string& path = field.first;
                        if (path == "opacity") t.overrides.push_back(FieldOverride{item.layers[0], "opacity", Json(1.0)});
                        else if (path == "flash.amount") t.overrides.push_back(FieldOverride{item.layers[0], "flash.amount", Json(0.0)});
                        else if (path.rfind("tint", 0) == 0) t.overrides.push_back(FieldOverride{item.layers[0], "tint", Json("#ffffff")});
                    }
                    tasks.push_back(t);
                }else if (item.kind == ItemKind::Glyphs){
                    planGlyphs(item.layers[0]);
                }
            }
        }

        void planGlyphs(const std::string& layerId){
            const TitleArtDoc& doc = *spec.doc;
            std::vector<const Json*> chain;
            if (!chainTo(layersOf(doc.root), layerId, chain)) return;
            const Json& layer = *chain.back();
            const geometry::Affine toDocument = chainAffine(chain, nullptr);
            auto toOutput = [&](const TitleVec2& p){ return canvasFrame.toFrame(toDocument.apply(p)); };
            std::vector<FieldOverride> layerOverrides;
            for (const FieldOverride& o : spec.overrides) if (o.layerId == layerId) layerOverrides.push_back(o);
            const LaidOutText& laid = atlas->layout(0, assets).layout(layer, layerOverrides);
            const double size = layer.value("size", 200.0);
            const auto& poses = spec.compose->glyphs.at(layerId);
            std::vector<PlacedGlyph>& placed = glyphs[layerId];
            for (size_t c = 0; c < laid.clusters.size(); c++){
                const LaidOutCluster& cluster = laid.clusters[c];
                if (cluster.inkBox.empty()) continue;
                PlacedGlyph glyph;
                glyph.key = "compose|" + std::to_string(keyOf("glyph|" + layerId + "|" + std::to_string(c)));
                auto found = poses.find(cluster.index);
                if (found != poses.end() && !found->second.identity()){
                    glyph.pose = &found->second;
                    titleGlyphMove(found->second, cluster.inkBox, cluster.pen, size, toOutput, glyph.move);
                }
                placed.push_back(glyph);
                inUse.insert(glyph.key);
                if (!atlas->find(glyph.key)){
                    Task t;
                    t.kind = Task::Kind::Cell;
                    t.cellKey = glyph.key;
                    t.layer = layerId;
                    t.cluster = (uint32_t)c;
                    t.overrides = spec.overrides;
                    tasks.push_back(t);
                }
            }
        }

        bool begin(RenderRequest request){
            request.assets = assets;
            request.pxScale = spec.pxScale;
            request.output = TitleOutputFormat::Linear;
            request.tier = spec.tier;
            request.testFlags = renderFlags;
            sub = renderer.begin(request);
            if (!sub){
                fail("the renderer refused the document");
                return false;
            }
            if (splitLimit) sub->splitOversizedSteps(splitLimit);
            for (const ShaderKey& k : sub->shaderKeys())
                if (std::find(subKeys.begin(), subKeys.end(), k) == subKeys.end()) subKeys.push_back(k);
            return true;
        }

        bool startNext(){
            if (nextTask >= tasks.size()) return false;
            running = &tasks[nextTask++];
            RenderRequest r;
            r.doc = spec.doc;
            r.region = frameRect;
            r.overrides = running->overrides;
            if (running->kind == Task::Kind::Plate){
                r.content = OutputContent::Layers;
                r.contentLayers = running->layers;
                if (!running->first) r.overrides.push_back(FieldOverride{"", "canvas.background.transparent", Json(true)});
            }else{
                r.glyphCell = GlyphCellRequest{running->layer, running->cluster, GlyphCellRequest::Back};
            }
            return begin(r);
        }

        void finishSub(){
            passesRun += sub->passesDone();
            if (running->kind == Task::Kind::Plate){
                atlas->storePlate(running->key, std::move(sub->resultImage()));
            }else{
                const TitleSplits& splits = sub->splits();
                bool full = false;
                std::string error;
                if (!atlas->insert(running->cellKey, splits.glyphBack, splits.glyphFront, 0.0, 0.0, inUse, full, error))
                    fail(full ? std::string("the title's glyph cells do not fit its atlas: raise Max Pages or lower Resolution Scale") : error);
            }
            sub.reset();
        }

        TargetRef target(ColorFormat format, std::string& error){
            TargetKey key;
            key.width = composeRect.width;
            key.height = composeRect.height;
            key.format = format;
            key.filter = TextureFilter::LINEAR;
            return atlas->pool().acquire(key, &error);
        }

        bool copyPlate(TitleTarget* into, const TitleImage& image, uint32_t& passes, std::string& error){
            TitleTarget* source = image.target.get();
            if (!source || !source->colorTexture()) return true;
            TitleGpuPass pass;
            pass.program = TitleRenderProgram::Copy;
            pass.target = into;
            pass.scissor = {0, 0, composeRect.width, composeRect.height};
            pass.blend = BlendMode::PREMULTIPLIED;
            pass.tex[0] = source->colorTexture();
            const float w = (float)std::max(1, image.pixels.width), h = (float)std::max(1, image.pixels.height);
            setMap(pass, 2, 1.0f / w, 1.0f / h, (float)(composeRect.x - image.pixels.x) / w, (float)(composeRect.y - image.pixels.y) / h);
            pass.params.set(3, 1.0f, 0.0f, 1.0f, 0.0f);
            passes++;
            return atlas->gpu().run(pass, error);
        }

        // a split as one quad, over the whole image, moved by the layer's pose
        TitlePosedQuad layerQuad(const Item& item, const TitleImage& image){
            TitlePosedQuad quad;
            TitleTarget* source = image.target.get();
            quad.texture = source ? source->colorTexture() : nullptr;
            quad.x0 = image.pixels.x - composeRect.x;
            quad.y0 = image.pixels.y - composeRect.y;
            quad.width = image.pixels.width;
            quad.height = image.pixels.height;
            std::vector<const Json*> chain;
            if (!chainTo(layersOf(spec.doc->root), item.layers[0], chain)) return quad;
            Json posed = *chain.back();
            float tint[3] = {1.0f, 1.0f, 1.0f}, flashColor[3] = {1.0f, 1.0f, 1.0f};
            float flashAmount = 0.0f, opacity = 1.0f;
            for (const auto& field : spec.compose->layers.at(item.layers[0])){
                const std::string& fieldPath = field.first;
                const Json& value = field.second;
                if (fieldPath.rfind("transform.", 0) == 0) setTitleArtField(posed, "layer", fieldPath, value);
                else if (fieldPath == "opacity") opacity = (float)value.get<double>();
                else if (fieldPath == "flash.amount") flashAmount = (float)value.get<double>();
                else if (fieldPath.rfind("flash.color", 0) == 0) applyColor(fieldPath, value, flashColor);
                else if (fieldPath.rfind("tint", 0) == 0) applyColor(fieldPath, value, tint);
            }
            TitleFrame moved = canvasFrame;
            moved.region.x += composeRect.x;
            moved.region.y += composeRect.y;
            layerMove(moved, chain, posed, quad.move);
            quad.opacity = std::max(0.0f, opacity);
            toLinear(tint);
            toLinear(flashColor);
            for (int i = 0; i < 3; i++){
                quad.tint[i] = tint[i];
                quad.flash[i] = flashColor[i] * std::max(0.0f, flashAmount);
            }
            return quad;
        }

        void glyphQuads(const std::string& layer, std::vector<TitlePosedQuad>& out){
            const float side = (float)atlas->pageSize();
            auto quadOf = [&](const PlacedGlyph& glyph, const TitleGlyphAtlas::Cell& cell, const TitleRectI& pixels, const TitleRectI& rect){
                TitlePosedQuad quad;
                TitleTarget* page = atlas->pageTarget(cell.page);
                quad.texture = page ? page->colorTexture() : nullptr;
                quad.uv[0] = rect.x / side;
                quad.uv[1] = rect.y / side;
                quad.uv[2] = (rect.x + rect.width) / side;
                quad.uv[3] = (rect.y + rect.height) / side;
                quad.x0 = pixels.x - composeRect.x;
                quad.y0 = pixels.y - composeRect.y;
                quad.width = rect.width;
                quad.height = rect.height;
                std::copy(glyph.move, glyph.move + 6, quad.move);
                // the move is in the frame's pixels; the quad is in the compose target's
                quad.move[2] -= composeRect.x - (quad.move[0] * composeRect.x + quad.move[1] * composeRect.y);
                quad.move[5] -= composeRect.y - (quad.move[3] * composeRect.x + quad.move[4] * composeRect.y);
                if (glyph.pose){
                    quad.opacity = (float)glyph.pose->opacity;
                    for (int k = 0; k < 3; k++){
                        quad.tint[k] = srgbToLinear((float)glyph.pose->tint[k]);
                        quad.flash[k] = srgbToLinear((float)glyph.pose->flash[k]) * (float)glyph.pose->flashAmount;
                    }
                }
                return quad;
            };
            // back cells first, then front cells: rims stay behind every face of their layer
            const std::vector<PlacedGlyph>& placed = glyphs[layer];
            for (const PlacedGlyph& glyph : placed){
                const TitleGlyphAtlas::Cell* cell = atlas->find(glyph.key);
                if (cell && !cell->blank && !cell->back.empty()) out.push_back(quadOf(glyph, *cell, cell->backPixels, cell->back));
            }
            for (const PlacedGlyph& glyph : placed){
                const TitleGlyphAtlas::Cell* cell = atlas->find(glyph.key);
                if (cell && !cell->blank && !cell->front.empty()) out.push_back(quadOf(glyph, *cell, cell->frontPixels, cell->front));
            }
        }

        bool compose(StepReport& report){
            std::string error;
            auto fails = [&](){
                fail(error.empty() ? std::string("the animated title's compose failed") : error);
                return false;
            };
            for (TitleRenderProgram p : {TitleRenderProgram::Copy, TitleRenderProgram::GlyphComposeAnim, TitleRenderProgram::Output}){
                std::string why;
                if (!atlas->gpu().ensureProgram(p, &why)){
                    if (!why.empty()) fail(why);
                    blocked = why.empty();
                    return false;
                }
            }
            blocked = false;
            accumulator = target(caps.format(TitleBuffer::Accumulator).format, error);
            if (accumulator) display = target(ColorFormat::RGBA, error);
            if (!accumulator || !display) return fails();
            TitleTarget* acc = accumulator.get();
            const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            uint32_t passes = 0;
            if (!atlas->gpu().clear(acc, transparent, error)) return fails();
            for (const Item& item : items){
                if (item.kind == ItemKind::Plate){
                    const TitleImage* image = atlas->plate(item.key);
                    if (image && !copyPlate(acc, *image, passes, error)) return fails();
                }else{
                    std::vector<TitlePosedQuad> quads;
                    if (item.kind == ItemKind::Layer){
                        const TitleImage* image = atlas->plate(item.key);
                        if (image) quads.push_back(layerQuad(item, *image));
                    }else{
                        glyphQuads(item.layers[0], quads);
                    }
                    if (!drawTitlePosedQuads(atlas->gpu(), acc, quads, composeRect.width, composeRect.height, passes, error)) return fails();
                }
            }
            TitleGpuPass out;
            out.program = TitleRenderProgram::Output;
            out.target = display.get();
            out.scissor = {0, 0, composeRect.width, composeRect.height};
            out.tex[0] = acc->colorTexture();
            out.params.set(2, 0.0f, (float)composeRect.x, (float)composeRect.y);
            setMap(out, 3, 1.0f / (float)composeRect.width, 1.0f / (float)composeRect.height, 0.0f, 0.0f);
            passes++;
            if (!atlas->gpu().run(out, error)) return fails();
            accumulator.reset();
            passesRun += passes;
            report.passes += passes;
            report.steps = 1;
            report.estimatedUs = COMPOSE_US;
            return true;
        }
    };

}

std::unique_ptr<TitleJob> jasperengine::titleart::makeTitleAnimComposeJob(TitleArtRenderer& renderer, const AssetResolver* assets, const TitleJobSpec& spec){
    return std::make_unique<ComposeJob>(renderer, assets, spec);
}
