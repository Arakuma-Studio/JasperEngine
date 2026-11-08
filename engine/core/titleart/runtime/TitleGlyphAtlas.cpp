// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleGlyphAtlas.h"

#include "render/SystemRender.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleFrame.h"
#include "titleart/text/TitleTextLayout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    // the pages and the plates live in the atlas's own pool (pages count in its bytes)
    constexpr size_t ATLAS_POOL_BYTES = 256u << 20;
    // a transparent texel between cells, so filtering never reads a neighbour
    constexpr int CELL_GUTTER = 1;
    // quads per compose pass (the program's parameter block: 2 vec4s each after p[2])
    constexpr int QUADS_PER_PASS = 20;
    // the compose and output steps' estimate
    constexpr uint32_t COMPOSE_US = 150;

    void setMap(TitleGpuPass& pass, int index, float sx, float sy, float ox, float oy){
        pass.params.set(index, sx, sy, ox, oy);
    }

    // the UTF-8 of a cluster's code points
    std::string utf8Of(const std::vector<uint32_t>& codepoints){
        std::string out;
        for (uint32_t c : codepoints){
            if (c < 0x80){
                out += (char)c;
            }else if (c < 0x800){
                out += (char)(0xC0 | (c >> 6));
                out += (char)(0x80 | (c & 0x3F));
            }else if (c < 0x10000){
                out += (char)(0xE0 | (c >> 12));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }else{
                out += (char)(0xF0 | (c >> 18));
                out += (char)(0x80 | ((c >> 12) & 0x3F));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }
        }
        return out;
    }

    // sub-pixel phases per pixel: a cell renders at its glyph's phase in the string to 1/32 px
    // (a residual under 1/64 px moves an edge pixel by under 4/255)
    constexpr int PHASES = 32;

    // the sub-pixel phase (0..PHASES-1) of an offset
    int phaseOf(double offset){
        const int q = (int)std::floor((offset - std::floor(offset)) * PHASES + 0.5);
        return q >= PHASES ? 0 : q;
    }

    std::string hex(uint64_t value){
        char buffer[17];
        std::snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)value);
        return buffer;
    }

    // the document's top-level layers, by reference (pointers into it stay valid)
    const Json& layersOf(const Json& root){
        static const Json none = Json::array();
        auto it = root.find("layers");
        return it != root.end() && it->is_array() ? *it : none;
    }

    // the groups above a layer, top first, then the layer
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

    // layer space -> document space, as the renderer places a layer (TitlePlanner::placementOf)
    geometry::Affine toDocumentOf(const std::vector<const Json*>& chain){
        geometry::Affine m;
        for (const Json* layer : chain){
            const TitleTransform t = decodeTransform(layer->value("transform", Json::object()));
            geometry::Affine local = geometry::Affine::scale((t.flipX ? -1.0 : 1.0) * t.scale, (t.flipY ? -1.0 : 1.0) * t.scale);
            local = geometry::Affine::rotateDegrees(-t.rotation).after(local);
            local = geometry::Affine::translate(t.position.x, t.position.y).after(local);
            m = m.after(local);
        }
        return m;
    }

    // the layers drawn below and above the text layer (whole layers, at every level of its chain)
    void platesOf(const Json& root, const std::string& textLayer, std::vector<std::string>& below, std::vector<std::string>& above){
        std::vector<const Json*> chain;
        if (!chainTo(layersOf(root), textLayer, chain)) return;
        const Json* siblings = &layersOf(root);
        for (const Json* step : chain){
            bool passed = false;
            for (const Json& layer : *siblings){
                if (&layer == step){
                    passed = true;
                    continue;
                }
                (passed ? above : below).push_back(layer.value("id", std::string()));
            }
            siblings = &layerChildren(*step);
        }
    }

    // the cell document (R26, R31): the text layer without per-character and per-line
    // adjustments (the quads carry them) and with the unsafe members made safe
    std::shared_ptr<const TitleArtDoc> makeCellDocument(const TitleArtDoc& doc, const TitleGlyphAtlasPlan& plan){
        auto cell = std::make_shared<TitleArtDoc>(doc);
        Json* layer = findLayer(cell->root, plan.textLayer);
        if (!layer) return cell;
        layer->erase("chars");
        layer->erase("lines");
        if (layer->contains("warp") && (*layer)["warp"].is_object()) (*layer)["warp"]["style"] = "none";
        if (layer->contains("distort") && (*layer)["distort"].is_object()) (*layer)["distort"]["mode"] = "none";
        if (layer->contains("masks")) (*layer)["masks"] = Json::array();
        if (layer->contains("style") && (*layer)["style"].contains("rims") && (*layer)["style"]["rims"].is_array()){
            for (Json& rim : (*layer)["style"]["rims"]){
                if (!rim.contains("plate") || !rim["plate"].is_object()) continue;
                const std::string kind = rim["plate"].value("kind", std::string());
                if (kind == "hull" || kind == "box") rim["plate"]["kind"] = "contour";
            }
        }
        for (const TitleGlyphAtlasItem& item : plan.unsafe){
            // "fx:<type>": the effect's entries off
            if (item.path.rfind("fx:", 0) != 0 || !layer->contains("effects")) continue;
            const std::string type = item.path.substr(3);
            for (Json& effect : (*layer)["effects"])
                if (effect.value("type", std::string()) == type) effect["enabled"] = false;
        }
        normalizeTitleArt(*cell);
        return cell;
    }

    // one glyph quad of the compose
    struct Quad {
        int page = 0;
        TitleRectI cell;            // atlas pixels
        double x0 = 0, y0 = 0;      // the compose target's pixels
    };

}

// ==================================================================== TitleGlyphAtlas

TitleGlyphAtlas::TitleGlyphAtlas(): targetPool(allocator, ATLAS_POOL_BYTES){
}

TitleGlyphAtlas::~TitleGlyphAtlas(){
    plates.clear();
    pages.clear();
}

void TitleGlyphAtlas::setLimits(int pageSize, int maxPages){
    const int side = pageSize == 512 || pageSize == 2048 ? pageSize : 1024;
    const int limit = std::max(1, std::min(8, maxPages));
    if (side != pageSide){
        // a new page size: the cells move to new pages
        pages.clear();
        cells.clear();
    }
    pageSide = side;
    pageLimit = limit;
    while ((int)pages.size() > pageLimit) freePage((int)pages.size() - 1);
}

size_t TitleGlyphAtlas::bytes() const{
    return targetPool.stats().bytesLive;
}

const TitleGlyphAtlas::Cell* TitleGlyphAtlas::find(const std::string& key){
    auto it = cells.find(key);
    if (it == cells.end()) return nullptr;
    it->second.lastUse = ++clock;
    if (it->second.page >= 0 && it->second.page < (int)pages.size()) pages[(size_t)it->second.page].lastUse = clock;
    return &it->second;
}

TitleTarget* TitleGlyphAtlas::pageTarget(int page) const{
    return page >= 0 && page < (int)pages.size() ? pages[(size_t)page].target.get() : nullptr;
}

bool TitleGlyphAtlas::place(Page& page, int width, int height, TitleRectI& out){
    // skyline, bottom-left: the column span whose highest column is lowest
    const int w = width + CELL_GUTTER, h = height + CELL_GUTTER;
    if (w > pageSide || h > pageSide) return false;
    int bestX = -1, bestY = pageSide + 1;
    for (int x = 0; x + w <= pageSide; x++){
        int top = 0;
        for (int i = x; i < x + w; i++) top = std::max(top, page.skyline[(size_t)i]);
        if (top + h <= pageSide && top < bestY){
            bestY = top;
            bestX = x;
        }
        // the next candidate starts where this span's column height changes
        while (x + 1 + w <= pageSide && page.skyline[(size_t)x + 1] == page.skyline[(size_t)x]) x++;
    }
    if (bestX < 0) return false;
    for (int i = bestX; i < bestX + w; i++) page.skyline[(size_t)i] = bestY + h;
    out = {bestX, bestY, width, height};
    return true;
}

bool TitleGlyphAtlas::newPage(std::string& error){
    Page page;
    TargetKey key;
    key.width = pageSide;
    key.height = pageSide;
    key.format = ColorFormat::RGBA;
    key.filter = TextureFilter::LINEAR;
    page.target = targetPool.acquire(key, &error);
    if (!page.target) return false;
    page.skyline.assign((size_t)pageSide, 0);
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!gpuLayer.clear(page.target.get(), transparent, error)) return false;
    page.cleared = true;
    pages.push_back(std::move(page));
    return true;
}

void TitleGlyphAtlas::freePage(int index){
    for (auto it = cells.begin(); it != cells.end();){
        if (it->second.page == index) it = cells.erase(it);
        else{
            if (it->second.page > index) it->second.page--;
            ++it;
        }
    }
    pages.erase(pages.begin() + index);
}

bool TitleGlyphAtlas::allocate(int width, int height, const std::set<std::string>& inUse, int& page, TitleRectI& rect, bool& full,
                               std::string& error){
    full = false;
    for (size_t p = 0; p < pages.size(); p++){
        if (place(pages[p], width, height, rect)){
            page = (int)p;
            return true;
        }
    }
    if ((int)pages.size() < pageLimit){
        if (!newPage(error)) return false;
        page = (int)pages.size() - 1;
        if (place(pages.back(), width, height, rect)) return true;
        error = "a glyph cell of " + std::to_string(width) + " x " + std::to_string(height) + " px does not fit a page of " +
                std::to_string(pageSide);
        full = true;
        return false;
    }
    // R29: the least recently used page that no wanted or shown string needs is freed
    int victim = -1;
    for (size_t p = 0; p < pages.size(); p++){
        bool used = false;
        for (const auto& [key, cell] : cells){
            if (cell.page == (int)p && (inUse.count(key) || shownKeys.count(key))){
                used = true;
                break;
            }
        }
        if (!used && (victim < 0 || pages[p].lastUse < pages[(size_t)victim].lastUse)) victim = (int)p;
    }
    if (victim < 0){
        full = true;
        return false;
    }
    for (auto it = cells.begin(); it != cells.end();){
        if (it->second.page == victim) it = cells.erase(it);
        else ++it;
    }
    Page& freed = pages[(size_t)victim];
    freed.skyline.assign((size_t)pageSide, 0);
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!gpuLayer.clear(freed.target.get(), transparent, error)) return false;
    counters.evictions++;
    if (!place(freed, width, height, rect)){
        full = true;
        return false;
    }
    page = victim;
    return true;
}

bool TitleGlyphAtlas::copyCell(int page, const TitleImage& from, const TitleRectI& into, std::string& error){
    TitleTarget* target = pageTarget(page);
    TitleTarget* source = from.target.get();
    if (!target || !source || !source->colorTexture()){
        error = "a glyph cell's target is gone";
        return false;
    }
    TitleGpuPass pass;
    pass.program = TitleRenderProgram::Copy;
    pass.target = target;
    pass.scissor = into;
    pass.blend = BlendMode::NONE;
    pass.tex[0] = source->colorTexture();
    const float w = (float)std::max(1, from.width), h = (float)std::max(1, from.height);
    setMap(pass, 2, 1.0f / w, 1.0f / h, -(float)into.x / w, -(float)into.y / h);
    pass.params.set(3, 1.0f, 0.0f, 1.0f, 0.0f);
    return gpuLayer.run(pass, error);
}

bool TitleGlyphAtlas::insert(const std::string& key, const TitleImage& back, const TitleImage& front, double anchorX, double anchorY,
                             const std::set<std::string>& inUse, bool& full, std::string& error){
    full = false;
    Cell cell;
    cell.anchorX = anchorX;
    cell.anchorY = anchorY;
    cell.backPixels = back.pixels;
    cell.frontPixels = front.pixels;
    const bool hasBack = back.target && back.width > 0 && back.height > 0;
    const bool hasFront = front.target && front.width > 0 && front.height > 0;
    if (!hasBack && !hasFront){
        insertBlank(key);
        return true;
    }
    // both parts on one page (one texture per compose run of the glyph)
    const int width = (hasBack ? back.width + CELL_GUTTER : 0) + (hasFront ? front.width : 0);
    const int height = std::max(hasBack ? back.height : 0, hasFront ? front.height : 0);
    int page = -1;
    TitleRectI rect;
    if (!allocate(width, height, inUse, page, rect, full, error)) return false;
    cell.page = page;
    if (hasBack){
        cell.back = {rect.x, rect.y, back.width, back.height};
        if (!copyCell(page, back, cell.back, error)) return false;
    }
    if (hasFront){
        cell.front = {rect.x + (hasBack ? back.width + CELL_GUTTER : 0), rect.y, front.width, front.height};
        if (!copyCell(page, front, cell.front, error)) return false;
    }
    cell.lastUse = ++clock;
    pages[(size_t)page].lastUse = clock;
    cells[key] = cell;
    counters.cellRenders++;
    return true;
}

void TitleGlyphAtlas::insertBlank(const std::string& key){
    Cell cell;
    cell.blank = true;
    cell.lastUse = ++clock;
    cells[key] = cell;
}

TitleImage* TitleGlyphAtlas::plate(uint64_t hash){
    auto it = plates.find(hash);
    return it != plates.end() && it->second.target ? &it->second : nullptr;
}

void TitleGlyphAtlas::storePlate(uint64_t hash, TitleImage image){
    // one state's plates at a time: a new state's replace the old (the shown image holds its own pixels)
    if (plates.size() >= 4) plates.erase(plates.begin());
    plates[hash] = std::move(image);
}

bool TitleGlyphAtlas::anchor(const std::string& key, double& x, double& y) const{
    auto it = anchors.find(key);
    if (it == anchors.end()) return false;
    x = it->second.first;
    y = it->second.second;
    return true;
}

void TitleGlyphAtlas::storeAnchor(const std::string& key, double x, double y){
    if (anchors.size() >= 4096) anchors.clear();
    anchors[key] = {x, y};
}

std::shared_ptr<const TitleArtDoc> TitleGlyphAtlas::cellDocument(uint64_t styleHash) const{
    auto it = cellDocs.find(styleHash);
    return it != cellDocs.end() ? it->second : nullptr;
}

void TitleGlyphAtlas::storeCellDocument(uint64_t styleHash, std::shared_ptr<const TitleArtDoc> doc){
    if (cellDocs.size() >= 4) cellDocs.clear();
    cellDocs[styleHash] = std::move(doc);
}

TitleTextLayout& TitleGlyphAtlas::layout(int which, const AssetResolver* assets){
    if (assets != layoutAssets){
        layouts[0].reset();
        layouts[1].reset();
        layoutAssets = assets;
    }
    std::unique_ptr<TitleTextLayout>& slot = layouts[which ? 1 : 0];
    if (!slot) slot = std::make_unique<TitleTextLayout>(assets);
    return *slot;
}

void TitleGlyphAtlas::clear(){
    cells.clear();
    anchors.clear();
    plates.clear();
    pages.clear();
    shownKeys.clear();
    targetPool.clear();
    gpuLayer.release();
}

// ==================================================================== the job

namespace {

    class GlyphAtlasJob : public TitleJob {
    public:
        GlyphAtlasJob(TitleArtRenderer& renderer, const AssetResolver* assets, const TitleJobSpec& spec)
            : renderer(renderer), assets(assets), spec(spec), atlas(spec.atlas){
            renderFlags = spec.testFlags & ~(TITLE_ART_TEST_GLYPH_STRING_ORDER | TITLE_ART_TEST_FAIL_TARGETS | TITLE_ART_TEST_SLOW_STEPS);
            if (titleArtForceRgba8()) renderFlags |= TITLE_ART_TEST_NO_FLOAT_TARGETS;
            caps = TitleArtCaps::select(SystemRender::getFeatures(), renderFlags);
            path = caps.pathName();
            if (spec.testFlags & TITLE_ART_TEST_FAIL_TARGETS){
                // R21: the request's allocations fail (a device out of memory), as the pool would say
                fail("out_of_target_memory: no title art target could be allocated (TITLE_ART_TEST_FAIL_TARGETS)");
                return;
            }
            prepare();
        }

        bool step(const StepBudget&, StepReport* report) override{
            if (!failure.empty() || finished) return finished;
            StepReport local;
            const auto start = std::chrono::steady_clock::now();
            if (sub){
                sub->step(StepBudget{1, 0}, &local);
                if (sub->failed()){
                    fail(sub->error());
                }else if (sub->done()){
                    finishSub();
                }
                blocked = sub && sub->waiting() && local.steps == 0;
            }else if (!startNext()){
                // every plate and cell is in: compose (one step)
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
            const size_t total = std::max<size_t>(1, work + 1);
            return std::min(1.0f, (float)(workDone + (finished ? 1 : 0)) / (float)total);
        }
        uint32_t nextStepCostUs() const override{
            if (finished) return 0;
            if (sub) return sub->nextStepCostUs();
            return COMPOSE_US;
        }
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
            for (TitleRenderProgram p : {TitleRenderProgram::Copy, TitleRenderProgram::GlyphCompose, TitleRenderProgram::Output}){
                const ShaderKey k = titleRenderProgramKey((uint32_t)p);
                if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
            }
            return keys;
        }

    private:
        struct Needed {
            std::string key;
            std::string text;           // the cluster
            double anchorX = 0, anchorY = 0;    // the glyph's position in the cell's render, output pixels
            double phaseX = 0, phaseY = 0;      // the sub-pixel offset the cell renders at (1/32 px)
        };
        enum class SubKind { None, BackPlate, FrontPlate, Cell };

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
        uint32_t splitLimit = 0;
        uint32_t passesRun = 0;
        size_t work = 0, workDone = 0;
        std::vector<ShaderKey> subKeys;

        // the state
        std::shared_ptr<const TitleArtDoc> cellDoc;
        std::vector<FieldOverride> styleOverrides;      // the instance's values other than the text
        std::vector<FieldOverride> layerOverrides;      // those of the text layer (its layout)
        uint64_t styleHash = 0;
        TitleFrame canvasFrame;
        TitleRectI composeRect;     // output pixels of the scaled canvas
        TitleRectI frameRect;       // canvas pixels
        geometry::Affine toDocument;
        double layerSize = 0.0;             // the text layer's size (its character offsets' 100 %)
        std::vector<std::string> belowLayers, aboveLayers;
        uint64_t backPlateHash = 0, frontPlateHash = 0;
        bool needBackPlate = false, needFrontPlate = false;
        std::vector<Needed> needed;     // cells to render, in order
        std::set<std::string> stringKeys;
        struct Placed {
            std::string key;
            double x = 0, y = 0;        // the string glyph's position, output pixels
        };
        std::vector<Placed> placed;     // the string's glyphs in order

        // the running sub-render
        SubKind subKind = SubKind::None;
        std::unique_ptr<TitleRenderJob> sub;
        size_t subCell = 0;
        size_t nextCell = 0;

        // the outputs
        TargetRef accumulator;
        TargetRef display;

        void fail(const std::string& message){
            failure = message;
            allocation = failure.find("out_of_target_memory") != std::string::npos;
        }

        TitleVec2 toOutput(const TitleVec2& layerPoint) const{
            return canvasFrame.toFrame(toDocument.apply(layerPoint));
        }

        // the state's layout, plates and cells
        void prepare(){
            const TitleArtDoc& doc = *spec.doc;
            const std::string& textLayer = spec.glyphPlan.textLayer;
            for (const FieldOverride& o : spec.overrides){
                const bool text = o.layerId == textLayer && o.path == "text";
                if (!text) styleOverrides.push_back(o);
                if (o.layerId == textLayer) layerOverrides.push_back(o);
            }
            uint64_t hash = hashCombine((uint64_t)(uintptr_t)spec.doc.get(), (uint64_t)std::llround(spec.pxScale * 64.0f));
            for (const FieldOverride& o : styleOverrides) hash = hashCombine(hash, fnv1a64(o.layerId + "\x1f" + o.path + "\x1f" + o.value.dump()));
            styleHash = hash;
            cellDoc = atlas->cellDocument(styleHash);
            if (!cellDoc){
                cellDoc = makeCellDocument(doc, spec.glyphPlan);
                atlas->storeCellDocument(styleHash, cellDoc);
            }

            const int cw = doc.canvasWidth(), ch = doc.canvasHeight();
            canvasFrame = TitleFrame::canvas(cw, ch, std::max(1e-3, (double)spec.pxScale));
            const TitleRectI canvasPx{0, 0, cw, ch};
            const TitleRectI region = spec.region.empty() ? canvasPx : intersectRect(spec.region, canvasPx);
            composeRect = intersectRect(canvasFrame.canvasToFrame(region), canvasFrame.region);
            frameRect = region;
            if (composeRect.empty()){
                fail("the title's region is empty");
                return;
            }

            std::vector<const Json*> chain;
            chainTo(layersOf(doc.root), textLayer, chain);
            toDocument = toDocumentOf(chain);
            if (const Json* cellText = findLayer(cellDoc->root, textLayer)) layerSize = cellText->value("size", 0.0);
            for (const FieldOverride& o : styleOverrides)
                if (o.layerId == textLayer && o.path == "size" && o.value.is_number()) layerSize = o.value.get<double>();
            platesOf(doc.root, textLayer, belowLayers, aboveLayers);
            const uint64_t plateBase = hashCombine(styleHash, ((uint64_t)(uint32_t)composeRect.x << 32) ^ (uint32_t)composeRect.y);
            backPlateHash = hashCombine(plateBase, 1);
            frontPlateHash = hashCombine(plateBase, 2);
            needBackPlate = !belowLayers.empty() && !atlas->plate(backPlateHash);
            needFrontPlate = !aboveLayers.empty() && !atlas->plate(frontPlateHash);

            // R27: the string's layout, the renderer's own
            const Json* layer = findLayer(doc.root, textLayer);
            const Json* cellLayer = findLayer(cellDoc->root, textLayer);
            if (!layer || !cellLayer){
                fail("the Glyph Atlas text layer is gone");
                return;
            }
            // the atlas keeps both layouts (their fonts and shaped runs are cached between strings)
            TitleTextLayout& full = atlas->layout(0, assets);
            const LaidOutText& laid = full.layout(*layer, layerOverrides);
            TitleTextLayout& single = atlas->layout(1, assets);
            std::set<std::string> queued;
            for (const LaidOutCluster& cluster : laid.clusters){
                if (cluster.glyphs.empty()) continue;
                bool ink = false;
                for (const LaidOutGlyph& g : cluster.glyphs) ink = ink || !g.outline.empty();
                if (!ink) continue;
                const std::string text = utf8Of(cluster.codepoints);
                const std::string base = hex(styleHash) + "|" + text;
                const TitleVec2 at = toOutput(cluster.glyphs.front().position);
                // the single glyph's position: the same layer alone with this cluster as its text
                double ax = 0.0, ay = 0.0;
                if (!atlas->anchor(base, ax, ay)){
                    std::vector<FieldOverride> one = layerOverrides;
                    one.erase(std::remove_if(one.begin(), one.end(), [](const FieldOverride& o){ return o.path == "text"; }), one.end());
                    one.push_back(FieldOverride{textLayer, "text", Json(text)});
                    const LaidOutText& alone = single.layout(*cellLayer, one);
                    if (alone.clusters.empty() || alone.clusters.front().glyphs.empty()) continue;
                    const TitleVec2 a = toOutput(alone.clusters.front().glyphs.front().position);
                    ax = a.x;
                    ay = a.y;
                    atlas->storeAnchor(base, ax, ay);
                }
                // the cell renders at the string's sub-pixel phase, so its quad lands on whole
                // pixels and is never resampled
                const int qx = phaseOf(at.x - ax), qy = phaseOf(at.y - ay);
                const std::string key = base + "|" + std::to_string(qx) + "," + std::to_string(qy);
                placed.push_back({key, at.x, at.y});
                stringKeys.insert(key);
                if (atlas->find(key) || queued.count(key)) continue;
                needed.push_back({key, text, ax + qx / (double)PHASES, ay + qy / (double)PHASES, qx / (double)PHASES, qy / (double)PHASES});
                queued.insert(key);
            }
            work = needed.size() + (needBackPlate ? 1 : 0) + (needFrontPlate ? 1 : 0);
        }

        void collectKeys(){
            for (const ShaderKey& k : sub->shaderKeys())
                if (std::find(subKeys.begin(), subKeys.end(), k) == subKeys.end()) subKeys.push_back(k);
        }

        bool begin(RenderRequest request, SubKind kind){
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
            subKind = kind;
            collectKeys();
            return true;
        }

        // the next plate or cell render; false when none is left
        bool startNext(){
            if (needBackPlate){
                needBackPlate = false;
                RenderRequest r;
                r.doc = spec.doc;
                r.region = frameRect;
                r.content = OutputContent::Layers;
                r.contentLayers = belowLayers;
                r.overrides = styleOverrides;
                return begin(r, SubKind::BackPlate);
            }
            if (needFrontPlate){
                needFrontPlate = false;
                RenderRequest r;
                r.doc = spec.doc;
                r.region = frameRect;
                r.content = OutputContent::Layers;
                r.contentLayers = aboveLayers;
                r.overrides = styleOverrides;
                // the canvas background is the back plate's: the front plate is only its layers
                r.overrides.push_back(FieldOverride{"", "canvas.background.transparent", Json(true)});
                return begin(r, SubKind::FrontPlate);
            }
            if (nextCell < needed.size()){
                // R26: the glyph alone, with the look, in a cell padded by its reach (one per step)
                subCell = nextCell++;
                RenderRequest r;
                r.doc = cellDoc;
                r.overrides = styleOverrides;
                r.overrides.push_back(FieldOverride{spec.glyphPlan.textLayer, "text", Json(needed[subCell].text)});
                if ((needed[subCell].phaseX != 0.0 || needed[subCell].phaseY != 0.0) && layerSize > 0.0){
                    // the glyph moved by the phase inside its layer (its character offset, percent of
                    // the size): the layer stays where it is, so the renderer draws the glyph at that
                    // phase as the string's render does, without resampling the layer
                    const double s = canvasFrame.pxScale;
                    const double dx = needed[subCell].phaseX / s, dy = -needed[subCell].phaseY / s;
                    const double det = toDocument.a * toDocument.d - toDocument.c * toDocument.b;
                    if (std::abs(det) > 1e-12){
                        const double lx = (toDocument.d * dx - toDocument.c * dy) / det, ly = (-toDocument.b * dx + toDocument.a * dy) / det;
                        r.overrides.push_back(FieldOverride{spec.glyphPlan.textLayer, "chars.0.offset",
                                                            Json::array({lx / layerSize * 100.0, ly / layerSize * 100.0})});
                    }
                }
                r.glyphCell = GlyphCellRequest{spec.glyphPlan.textLayer, 0, GlyphCellRequest::Back};
                return begin(r, SubKind::Cell);
            }
            return false;
        }

        void finishSub(){
            workDone++;
            passesRun += sub->passesDone();
            if (subKind == SubKind::BackPlate || subKind == SubKind::FrontPlate){
                atlas->storePlate(subKind == SubKind::BackPlate ? backPlateHash : frontPlateHash, std::move(sub->resultImage()));
            }else if (subKind == SubKind::Cell){
                const Needed& n = needed[subCell];
                const TitleSplits& splits = sub->splits();
                bool full = false;
                std::string error;
                if (!atlas->insert(n.key, splits.glyphBack, splits.glyphFront, n.anchorX, n.anchorY, stringKeys, full, error)){
                    if (full){
                        const std::string name = spec.label.rfind("TitleArt/", 0) == 0 ? spec.label.substr(9) : spec.label;
                        fail("Title Art '" + name + "': the glyph atlas is full (" + std::to_string(atlas->maxPages()) + " pages of " +
                             std::to_string(atlas->pageSize()) + "). Raise Max Pages, lower Resolution Scale, or reduce the Characters.");
                    }else{
                        fail(error);
                    }
                }
            }
            sub.reset();
            subKind = SubKind::None;
        }

        bool ensure(TitleRenderProgram program){
            std::string error;
            if (atlas->gpu().ensureProgram(program, &error)) return true;
            if (!error.empty()) fail(error);
            blocked = error.empty();
            return false;
        }

        bool copyPlate(TitleTarget* into, const TitleImage& plate, uint32_t& passes, std::string& error){
            TitleTarget* source = plate.target.get();
            if (!source || !source->colorTexture()) return true;
            TitleGpuPass pass;
            pass.program = TitleRenderProgram::Copy;
            pass.target = into;
            pass.scissor = {0, 0, composeRect.width, composeRect.height};
            pass.blend = BlendMode::PREMULTIPLIED;
            pass.tex[0] = source->colorTexture();
            const float w = (float)std::max(1, plate.pixels.width), h = (float)std::max(1, plate.pixels.height);
            setMap(pass, 2, 1.0f / w, 1.0f / h, (float)(composeRect.x - plate.pixels.x) / w, (float)(composeRect.y - plate.pixels.y) / h);
            pass.params.set(3, 1.0f, 0.0f, 1.0f, 0.0f);
            passes++;
            return atlas->gpu().run(pass, error);
        }

        // R28: back plate, back cells, front cells, front plate, then the output pass
        bool compose(StepReport& report){
            for (TitleRenderProgram p : {TitleRenderProgram::Copy, TitleRenderProgram::GlyphCompose, TitleRenderProgram::Output})
                if (!ensure(p)) return false;
            blocked = false;
            std::string error;
            TargetKey accKey;
            accKey.width = composeRect.width;
            accKey.height = composeRect.height;
            accKey.format = caps.format(TitleBuffer::Accumulator).format;
            accKey.filter = TextureFilter::LINEAR;
            accumulator = atlas->pool().acquire(accKey, &error);
            TargetKey outKey = accKey;
            outKey.format = ColorFormat::RGBA;
            if (accumulator) display = atlas->pool().acquire(outKey, &error);
            if (!accumulator || !display){
                fail(error.empty() ? std::string("out_of_target_memory: the Glyph Atlas compose target") : error);
                return false;
            }
            TitleTarget* acc = accumulator.get();
            uint32_t passes = 0;
            const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (!atlas->gpu().clear(acc, transparent, error)) return failWith(error);
            if (const TitleImage* back = atlas->plate(backPlateHash))
                if (!copyPlate(acc, *back, passes, error)) return failWith(error);

            // the quads: every back cell, then every front cell (rims behind every face)
            std::vector<Quad> quads;
            std::vector<Quad> fronts;
            const bool stringOrder = (spec.testFlags & TITLE_ART_TEST_GLYPH_STRING_ORDER) != 0;
            std::set<std::string> shown;
            for (const Placed& p : placed){
                const TitleGlyphAtlas::Cell* cell = atlas->find(p.key);
                if (!cell || cell->blank) continue;
                shown.insert(p.key);
                const double dx = std::round(p.x - cell->anchorX), dy = std::round(p.y - cell->anchorY);
                if (!cell->back.empty())
                    quads.push_back({cell->page, cell->back, cell->backPixels.x + dx - composeRect.x, cell->backPixels.y + dy - composeRect.y});
                Quad front{cell->page, cell->front, cell->frontPixels.x + dx - composeRect.x, cell->frontPixels.y + dy - composeRect.y};
                if (!cell->front.empty()) (stringOrder ? quads : fronts).push_back(front);
            }
            quads.insert(quads.end(), fronts.begin(), fronts.end());
            const float side = (float)atlas->pageSize();
            for (size_t i = 0; i < quads.size();){
                // a run of quads of one page, at most QUADS_PER_PASS
                const int page = quads[i].page;
                size_t end = i;
                while (end < quads.size() && quads[end].page == page && end - i < (size_t)QUADS_PER_PASS) end++;
                TitleGpuPass pass;
                pass.program = TitleRenderProgram::GlyphCompose;
                pass.target = acc;
                pass.scissor = {0, 0, composeRect.width, composeRect.height};
                pass.blend = BlendMode::PREMULTIPLIED;
                pass.tex[0] = atlas->pageTarget(page) ? atlas->pageTarget(page)->colorTexture() : nullptr;
                pass.params.set(2, (float)(end - i));
                for (size_t q = i; q < end; q++){
                    const Quad& quad = quads[q];
                    const int slot = 3 + 2 * (int)(q - i);
                    pass.params.set(slot, (float)quad.x0, (float)quad.y0, (float)(quad.x0 + quad.cell.width), (float)(quad.y0 + quad.cell.height));
                    pass.params.set(slot + 1, quad.cell.x / side, quad.cell.y / side, (quad.cell.x + quad.cell.width) / side,
                                    (quad.cell.y + quad.cell.height) / side);
                }
                passes++;
                if (!pass.tex[0] || !atlas->gpu().run(pass, error)) return failWith(error.empty() ? std::string("a glyph atlas page is gone") : error);
                i = end;
            }
            if (const TitleImage* front = atlas->plate(frontPlateHash))
                if (!copyPlate(acc, *front, passes, error)) return failWith(error);

            // S80's output pass: straight-alpha sRGB with dither, the display encoding
            TitleGpuPass out;
            out.program = TitleRenderProgram::Output;
            out.target = display.get();
            out.scissor = {0, 0, composeRect.width, composeRect.height};
            out.tex[0] = acc->colorTexture();
            out.params.set(2, 0.0f, (float)composeRect.x, (float)composeRect.y);
            setMap(out, 3, 1.0f / (float)composeRect.width, 1.0f / (float)composeRect.height, 0.0f, 0.0f);
            passes++;
            if (!atlas->gpu().run(out, error)) return failWith(error);
            accumulator.reset();
            atlas->setShown(std::move(shown));
            passesRun += passes;
            atlas->mutableStats().composes++;
            atlas->mutableStats().composePasses = passes + 1;    // and the clear
            report.passes += passes;
            report.steps = 1;
            report.estimatedUs = COMPOSE_US;
            return true;
        }

        bool failWith(const std::string& error){
            fail(error.empty() ? std::string("the Glyph Atlas compose failed") : error);
            return false;
        }
    };

}

std::unique_ptr<TitleJob> jasperengine::titleart::makeGlyphAtlasJob(TitleArtRenderer& renderer, const AssetResolver* assets, const TitleJobSpec& spec){
    return std::make_unique<GlyphAtlasJob>(renderer, assets, spec);
}
