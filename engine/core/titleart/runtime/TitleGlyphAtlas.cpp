// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleGlyphAtlas.h"
#include "TitleAnimCompose.h"

#include "Log.h"
#include "io/AtomicFile.h"
#include "io/Data.h"
#include "render/FrameReadback.h"
#include "render/SystemRender.h"
#include "shader/RuntimeShaderCompiler.h"
#include "texture/TexturePrepare.h"
#include "texture/TextureData.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "titleart/render/TitleStepCosts.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleFrame.h"
#include "titleart/text/TitleTextLayout.h"
#include "util/TitleArtBakeInfo.h"
#include "titleart/runtime/TitleArtScheduler.h"

#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

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
    // the compose's kind in the scheduler's cost model (TitleStepCosts)
    constexpr const char* COMPOSE_KIND = "gpu:glyph compose";

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
            geometry::Affine local = geometry::Affine::scale((t.flipX ? -1.0 : 1.0) * t.scale * t.stretch.x, (t.flipY ? -1.0 : 1.0) * t.scale * t.stretch.y);
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
        // S85 R15: where the glyph's pose moved it (null pose: at rest), the cell's own pixels
        // to the target's as p' = (m[0] x + m[1] y + m[2], m[3] x + m[4] y + m[5])
        const GlyphPose* pose = nullptr;
        double move[6] = {1, 0, 0, 0, 1, 0};
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
    if (pendingInsert.page == index) pendingInsert = PendingInsert();
    else if (pendingInsert.page > index) pendingInsert.page--;
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
    if (width + CELL_GUTTER > pageSide || height + CELL_GUTTER > pageSide){
        // more pages cannot hold it, so no page is made for it and R29's sentence would mislead
        error = "a glyph cell of " + std::to_string(width) + " x " + std::to_string(height) + " px does not fit an atlas page of " +
                std::to_string(pageSide) + ". Raise Atlas Page Size, lower Resolution Scale, or shorten what reaches past the glyphs (glows, shadows).";
        full = true;
        return false;
    }
    for (size_t p = 0; p < pages.size(); p++){
        if (place(pages[p], width, height, rect)){
            page = (int)p;
            return true;
        }
    }
    if ((int)pages.size() < pageLimit){
        if (!newPage(error)) return false;
        page = (int)pages.size() - 1;
        return place(pages.back(), width, height, rect);
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
    if (pendingInsert.page == victim) pendingInsert = PendingInsert();
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

bool TitleGlyphAtlas::copyCell(int page, const TitleImage& from, const TitleRectI& region, const TitleRectI& into, std::string& error){
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
    // `region` (output pixels inside from.pixels) lands at `into` (page pixels)
    const float w = (float)std::max(1, from.width), h = (float)std::max(1, from.height);
    const float sx = (float)(region.x - from.pixels.x), sy = (float)(region.y - from.pixels.y);
    setMap(pass, 2, 1.0f / w, 1.0f / h, (sx - (float)into.x) / w, (sy - (float)into.y) / h);
    pass.params.set(3, 1.0f, 0.0f, 1.0f, 0.0f);
    return gpuLayer.run(pass, error);
}

bool TitleGlyphAtlas::copyUpload(TitleTarget* target, TextureRender& from, int width, int height, std::string& error){
    TitleGpuPass pass;
    pass.program = TitleRenderProgram::Copy;
    pass.target = target;
    pass.scissor = {0, 0, width, height};
    pass.blend = BlendMode::NONE;
    pass.tex[0] = &from;
    setMap(pass, 2, 1.0f / (float)width, 1.0f / (float)height, 0.0f, 0.0f);
    pass.params.set(3, 1.0f, 0.0f, 1.0f, 0.0f);
    return gpuLayer.run(pass, error);
}

bool TitleGlyphAtlas::insert(const std::string& key, const TitleImage& back, const TitleImage& front, double anchorX, double anchorY,
                             const std::set<std::string>& inUse, bool& full, bool& deferred, std::string& error){
    full = false;
    deferred = false;
    Cell cell;
    cell.anchorX = anchorX;
    cell.anchorY = anchorY;
    // the pixels the glyph's content and reach can cover, not the ROI grid's padding
    auto cropOf = [](const TitleImage& image){
        return image.reach.empty() ? image.pixels : intersectRect(image.reach, image.pixels);
    };
    cell.backPixels = cropOf(back);
    cell.frontPixels = cropOf(front);
    const bool hasBack = back.target && !cell.backPixels.empty();
    const bool hasFront = front.target && !cell.frontPixels.empty();
    if (!hasBack && !hasFront){
        pendingInsert = PendingInsert();
        insertBlank(key);
        return true;
    }
    // both parts on one page (one texture per compose run of the glyph)
    const int width = (hasBack ? cell.backPixels.width + CELL_GUTTER : 0) + (hasFront ? cell.frontPixels.width : 0);
    const int height = std::max(hasBack ? cell.backPixels.height : 0, hasFront ? cell.frontPixels.height : 0);
    int page = -1;
    TitleRectI rect;
    if (pendingInsert.page >= 0 && pendingInsert.key == key){
        page = pendingInsert.page;
        rect = pendingInsert.rect;
    }else{
        pendingInsert = PendingInsert();
        if (!allocate(width, height, inUse, page, rect, full, error)) return false;
    }
    cell.page = page;
    const uint64_t deferrals = gpuLayer.deferrals();
    if (hasBack){
        cell.back = {rect.x, rect.y, cell.backPixels.width, cell.backPixels.height};
        if (!copyCell(page, back, cell.backPixels, cell.back, error)) return false;
    }
    if (hasFront){
        cell.front = {rect.x + (hasBack ? cell.backPixels.width + CELL_GUTTER : 0), rect.y, cell.frontPixels.width, cell.frontPixels.height};
        if (!copyCell(page, front, cell.frontPixels, cell.front, error)) return false;
    }
    if (gpuLayer.deferrals() != deferrals){
        // the cell's pixels are not in the page yet: registering it would be a blank cell forever
        pendingInsert = {key, page, rect};
        deferred = true;
        return false;
    }
    pendingInsert = PendingInsert();
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
    if (cellDocs.size() >= 64) cellDocs.clear();
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
    uploads.clear();
    triedPrebakes.clear();
    plates.clear();
    pages.clear();
    shownKeys.clear();
    targetPool.clear();
    gpuLayer.release();
}

// ---- prebaked atlases (R32) ----

namespace {

    Json rectJson(const TitleRectI& r){
        return Json::array({r.x, r.y, r.width, r.height});
    }

    TitleRectI rectOf(const Json& value){
        if (!value.is_array() || value.size() != 4) return {};
        return {value[0].get<int>(), value[1].get<int>(), value[2].get<int>(), value[3].get<int>()};
    }

    // a target's RGBA8 pixels (a float target's clamped to 8 bits; the values are linear premultiplied)
    bool readPixels(TitleTarget* target, int width, int height, ColorFormat format, std::vector<uint8_t>& out, std::string& error){
        TextureRender* texture = target ? target->colorTexture() : nullptr;
        if (!texture){
            error = "a glyph atlas target is gone";
            return false;
        }
        const bool half = format == ColorFormat::RGBA16F;
        std::vector<uint8_t> raw;
        if (!FrameReadback::readTexture(*texture, width, height, half ? 8 : 4, raw, &error)) return false;
        if (!half){
            out = std::move(raw);
            return true;
        }
        out.resize((size_t)width * height * 4);
        for (size_t i = 0; i < out.size(); i++){
            uint16_t h;
            std::memcpy(&h, &raw[i * 2], 2);
            out[i] = (uint8_t)std::lround(std::min(1.0f, std::max(0.0f, halfToFloat(h))) * 255.0f);
        }
        return true;
    }

}

uint64_t jasperengine::titleart::titleGlyphStyleHash(uint64_t documentHash, const std::vector<FieldOverride>& overrides, const std::string& textLayer,
                                                      float pxScale){
    uint64_t hash = hashCombine(documentHash, (uint64_t)std::llround(pxScale * 64.0f));
    for (const FieldOverride& o : overrides){
        if (o.layerId == textLayer && o.path == "text") continue;
        hash = hashCombine(hash, fnv1a64(o.layerId + "\x1f" + o.path + "\x1f" + o.value.dump()));
    }
    return hash;
}

bool TitleGlyphAtlas::snapshot(TitleGlyphAtlasSnapshot& out, std::string& error){
    out = TitleGlyphAtlasSnapshot();
    Json manifest{{"format", "jasperengine.titleart.glyphs"}, {"version", 1}, {"page_size", pageSide}};
    Json skylines = Json::array();
    for (Page& page : pages){
        std::vector<uint8_t> pixels;
        if (!readPixels(page.target.get(), pageSide, pageSide, ColorFormat::RGBA, pixels, error)) return false;
        out.pages.push_back(std::move(pixels));
        skylines.push_back(page.skyline);
    }
    manifest["skylines"] = skylines;
    Json list = Json::array();
    for (const auto& [key, cell] : cells){
        list.push_back(Json{{"key", key}, {"page", cell.page}, {"back", rectJson(cell.back)}, {"front", rectJson(cell.front)},
                            {"back_pixels", rectJson(cell.backPixels)}, {"front_pixels", rectJson(cell.frontPixels)},
                            {"anchor", Json::array({cell.anchorX, cell.anchorY})}, {"blank", cell.blank}});
    }
    manifest["cells"] = list;
    Json anchorList = Json::array();
    for (const auto& [key, at] : anchors) anchorList.push_back(Json{{"key", key}, {"at", Json::array({at.first, at.second})}});
    manifest["anchors"] = anchorList;
    Json plateList = Json::array();
    for (auto& [hash, image] : plates){
        std::vector<uint8_t> pixels;
        if (!readPixels(image.target.get(), image.width, image.height, image.format, pixels, error)) return false;
        char hex[17];
        std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
        plateList.push_back(Json{{"hash", hex}, {"pixels", rectJson(image.pixels)}, {"width", image.width}, {"height", image.height}});
        out.plates.push_back(std::move(pixels));
    }
    manifest["plates"] = plateList;
    out.manifest = std::move(manifest);
    return true;
}

bool TitleGlyphAtlas::restore(const TitleGlyphAtlasSnapshot& in, std::string& error){
    std::string programError;
    if (!gpuLayer.ensureProgram(TitleRenderProgram::Copy, &programError)){
        error = programError;
        return false;
    }
    const uint64_t deferrals = gpuLayer.deferrals();
    const uint64_t restoredBefore = counters.restoredCells;
    // a copy that went to a later frame (TitleFirstDraws) would be a blank page or plate: the
    // caller keeps the snapshot and the whole restore runs again
    auto deferred = [&]{
        if (gpuLayer.deferrals() == deferrals) return false;
        pages.clear();
        cells.clear();
        uploads.clear();
        counters.restoredCells = restoredBefore;
        return true;
    };
    const Json& m = in.manifest;
    const int side = m.value("page_size", 0);
    const Json skylines = m.value("skylines", Json::array());
    if (side <= 0 || in.pages.size() != skylines.size()){
        error = "the prebaked glyph atlas does not match its pages";
        return false;
    }
    setLimits(side, std::max(pageLimit, (int)in.pages.size()));
    pages.clear();
    cells.clear();
    for (size_t i = 0; i < in.pages.size(); i++){
        if (in.pages[i].size() != (size_t)side * side * 4){
            error = "a prebaked glyph atlas page has the wrong size";
            return false;
        }
        if (!newPage(error)) return false;
        auto upload = std::make_unique<TitleUpload>();
        if (!upload->create("TitleArt/GlyphPage", side, side, ColorFormat::RGBA, in.pages[i].data(), in.pages[i].size(), TextureFilter::LINEAR)){
            error = "a prebaked glyph atlas page could not be uploaded";
            return false;
        }
        if (!copyUpload(pages.back().target.get(), upload->texture(), side, side, error)) return false;
        if (deferred()) return false;
        uploads.push_back(std::move(upload));
        pages.back().skyline = skylines[i].get<std::vector<int>>();
        if ((int)pages.back().skyline.size() != side) pages.back().skyline.assign((size_t)side, side);
    }
    for (const Json& c : m.value("cells", Json::array())){
        Cell cell;
        cell.page = c.value("page", -1);
        cell.back = rectOf(c.value("back", Json()));
        cell.front = rectOf(c.value("front", Json()));
        cell.backPixels = rectOf(c.value("back_pixels", Json()));
        cell.frontPixels = rectOf(c.value("front_pixels", Json()));
        const Json at = c.value("anchor", Json::array({0, 0}));
        cell.anchorX = at[0].get<double>();
        cell.anchorY = at[1].get<double>();
        cell.blank = c.value("blank", false);
        cell.lastUse = ++clock;
        if (!cell.blank && (cell.page < 0 || cell.page >= (int)pages.size())) continue;
        cells[c.value("key", std::string())] = cell;
        counters.restoredCells++;
    }
    for (const Json& a : m.value("anchors", Json::array())){
        const Json at = a.value("at", Json::array({0, 0}));
        anchors[a.value("key", std::string())] = {at[0].get<double>(), at[1].get<double>()};
    }
    const Json plateList = m.value("plates", Json::array());
    for (size_t i = 0; i < plateList.size() && i < in.plates.size(); i++){
        const Json& p = plateList[i];
        TitleImage image;
        image.width = p.value("width", 0);
        image.height = p.value("height", 0);
        image.pixels = rectOf(p.value("pixels", Json()));
        if (image.width <= 0 || image.height <= 0 || in.plates[i].size() != (size_t)image.width * image.height * 4) continue;
        TargetKey key;
        key.width = image.width;
        key.height = image.height;
        key.format = ColorFormat::RGBA;
        key.filter = TextureFilter::LINEAR;
        image.target = targetPool.acquire(key, &error);
        if (!image.target) return false;
        auto upload = std::make_unique<TitleUpload>();
        if (!upload->create("TitleArt/GlyphPlate", image.width, image.height, ColorFormat::RGBA, in.plates[i].data(), in.plates[i].size(),
                            TextureFilter::LINEAR)){
            error = "a prebaked plate could not be uploaded";
            return false;
        }
        if (!copyUpload(image.target.get(), upload->texture(), image.width, image.height, error)) return false;
        if (deferred()) return false;
        uploads.push_back(std::move(upload));
        image.format = ColorFormat::RGBA;
        image.premultiplied = true;
        image.frame = image.pixels;
        plates[std::stoull(p.value("hash", std::string("0")), nullptr, 16)] = std::move(image);
    }
    return true;
}

bool TitleGlyphAtlas::readPrebaked(const std::string& manifestPath, TitleGlyphAtlasSnapshot& out, std::string& error){
    Data data;
    if (data.open(manifestPath.c_str()) != FILEDATA_OK) return false;
    const std::string text(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
    out = TitleGlyphAtlasSnapshot();
    out.manifest = Json::parse(text, nullptr, false);
    if (!out.manifest.is_object() || out.manifest.value("format", std::string()) != "jasperengine.titleart.glyphs"){
        error = manifestPath + " is not a prebaked glyph atlas";
        return false;
    }
    auto readPng = [&](const std::string& path, int width, int height, std::vector<uint8_t>& pixels){
        TextureData image;
        if (!image.loadTextureFromFile(path.c_str()) || image.getWidth() != width || image.getHeight() != height ||
            image.getSize() != (unsigned int)(width * height * 4)){
            error = "cannot read " + path;
            return false;
        }
        const uint8_t* bytes = static_cast<const uint8_t*>(image.getData());
        pixels.assign(bytes, bytes + (size_t)width * height * 4);
        return true;
    };
    const int side = out.manifest.value("page_size", 0);
    const size_t pageCount = out.manifest.value("skylines", Json::array()).size();
    for (size_t i = 0; i < pageCount; i++){
        std::vector<uint8_t> pixels;
        if (!readPng(titleArtGlyphPrebakeFile(manifestPath, "p", (int)i), side, side, pixels)) return false;
        out.pages.push_back(std::move(pixels));
    }
    const Json plateList = out.manifest.value("plates", Json::array());
    for (size_t i = 0; i < plateList.size(); i++){
        std::vector<uint8_t> pixels;
        if (!readPng(titleArtGlyphPrebakeFile(manifestPath, "plate", (int)i), plateList[i].value("width", 0), plateList[i].value("height", 0), pixels))
            return false;
        out.plates.push_back(std::move(pixels));
    }
    return true;
}

namespace {

    void appendPngBytes(void* context, void* data, int size){
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), (size_t)size);
    }

    bool writeFileBytes(const std::string& path, const std::string& bytes, std::string& error){
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        options.createDirectories = true;
        const io::WriteResult written = io::writeFileAtomic(std::filesystem::path(path), bytes, options);
        if (!written.ok()){
            error = written.message;
            return false;
        }
        return true;
    }

    bool writePngFile(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height, size_t& bytes, std::string& error){
        if (width <= 0 || height <= 0 || rgba.size() != (size_t)width * height * 4){
            error = "a glyph atlas image has the wrong size";
            return false;
        }
        std::string png;
        if (!stbi_write_png_to_func(&appendPngBytes, &png, width, height, 4, rgba.data(), width * 4)){
            error = "cannot encode " + path;
            return false;
        }
        bytes += png.size();
        return writeFileBytes(path, png, error);
    }

}

bool TitleGlyphAtlas::writePrebaked(const std::string& manifestPath, const TitleGlyphAtlasSnapshot& snapshot, size_t& bytes, std::string& error){
    const int side = snapshot.manifest.value("page_size", 0);
    for (size_t i = 0; i < snapshot.pages.size(); i++)
        if (!writePngFile(titleArtGlyphPrebakeFile(manifestPath, "p", (int)i), snapshot.pages[i], side, side, bytes, error)) return false;
    const Json plates = snapshot.manifest.value("plates", Json::array());
    for (size_t i = 0; i < snapshot.plates.size() && i < plates.size(); i++){
        if (!writePngFile(titleArtGlyphPrebakeFile(manifestPath, "plate", (int)i), snapshot.plates[i], plates[i].value("width", 0),
                          plates[i].value("height", 0), bytes, error))
            return false;
    }
    const std::string text = snapshot.manifest.dump(1);
    bytes += text.size();
    return writeFileBytes(manifestPath, text, error);
}

std::vector<std::string> jasperengine::titleart::titleGlyphPrebakeStrings(const std::string& initial){
    std::vector<std::string> out{initial};
    for (size_t i = 0; i < initial.size(); i++){
        if (initial[i] < '0' || initial[i] > '9') continue;
        for (char d = '0'; d <= '9'; d++){
            if (d == initial[i]) continue;
            std::string variant = initial;
            variant[i] = d;
            out.push_back(variant);
        }
    }
    return out;
}

bool jasperengine::titleart::prebakeTitleGlyphAtlas(TitleArtLive& live, const TitleJobFactory& factory, const TitleArtLiveState& state,
                                                    const std::vector<FieldOverride>& overrides, const std::string& textLayer,
                                                    const std::vector<std::string>& strings, size_t maxCells, std::string& error){
    TitleLiveLimits limits;
    double clock = 0.0;
    for (const std::string& text : strings){
        if (live.glyphAtlas() && live.glyphAtlas()->cellCount() >= maxCells) break;
        std::vector<FieldOverride> values;
        for (const FieldOverride& o : overrides)
            if (!(o.layerId == textLayer && o.path == "text")) values.push_back(o);
        values.push_back(FieldOverride{textLayer, "text", Json(text)});
        live.setState(state, fnv1a64(text) + 1, values);
        const auto start = std::chrono::steady_clock::now();
        while (live.hasWork() && !live.status().failed && std::chrono::steady_clock::now() - start < std::chrono::seconds(60)){
            // the GPU objects and programs it waits for are made between its steps
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            TitleArtScheduler::schedule({&live}, factory, 1000000000u, 1.0);
            size_t cache = 0;
            clock += 1.0 / 60.0;
            live.endFrame(clock, 1.0 / 60.0, limits, cache);
            // R14's cache would keep every string's image: none is needed here
            while (live.evictOldest()){
            }
        }
        if (live.status().failed || live.hasWork()){
            error = "\"" + text + "\": " + (live.status().failed ? live.status().error : std::string("did not finish within 60 s"));
            return false;
        }
    }
    return true;
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
            atlas->releaseUploads();
            setUpStyle();
            // R32: a style's prebaked atlas, once (its restore is the first step)
            if (!spec.titlePath.empty() && !atlas->prebakeTried(styleHash)){
                atlas->markPrebakeTried(styleHash);
                std::string error;
                if (TitleGlyphAtlas::readPrebaked(titleArtGlyphPrebakePath(spec.titlePath, styleHash), prebaked, error)) hasPrebaked = true;
            }
            if (!hasPrebaked) prepare();
        }

        bool step(const StepBudget&, StepReport* report) override{
            if (!failure.empty() || finished) return finished;
            StepReport local;
            bool composed = false;
            const auto start = std::chrono::steady_clock::now();
            // a page copy or compose whose first draw went to a later frame (TitleFirstDraws) did
            // not finish: its chain draws again once a new first draw can go
            if (firstDrawWait){
                if (!TitleFirstDraws::available()){
                    blocked = true;
                    if (report) *report = local;
                    return false;
                }
                firstDrawWait = false;
                blocked = false;
            }
            if (!prepared){
                // the prebaked pages and plates, then the state's layout
                std::string error;
                if (atlas->restore(prebaked, error)){
                    prebaked = TitleGlyphAtlasSnapshot();
                    blocked = false;
                    prepare();
                }else if (!error.empty()){
                    // a damaged prebake: the cells render as they would without one
                    prebaked = TitleGlyphAtlasSnapshot();
                    prepare();
                }else{
                    blocked = true;
                    firstDrawWait = atlas->gpu().lastRunDeferred();
                }
            }else if (sub){
                if (!sub->done()) sub->step(StepBudget{1, 0}, &local);
                if (sub->failed()){
                    fail(sub->error());
                }else if (sub->done()){
                    finishSub();
                }
                blocked = firstDrawWait || (sub && sub->waiting() && local.steps == 0);
            }else if (!startNext()){
                // every plate and cell is in: compose (one step)
                if (compose(local)) finished = true;
                composed = finished;
            }
            local.measuredUs = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
            if (composed && costs){
                // what the compose costs this device (the scheduler's prediction of the next one)
                local.estimatedUs = costs->predictUs(COMPOSE_KIND, COMPOSE_US);
                costs->observe(COMPOSE_KIND, COMPOSE_US, local.measuredUs);
            }
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
            return costs ? costs->predictUs(COMPOSE_KIND, COMPOSE_US) : COMPOSE_US;
        }
        void useCosts(TitleStepCosts* model) override{
            costs = model;
            if (sub) sub->useCosts(model);
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
            for (TitleRenderProgram p : {TitleRenderProgram::Copy, TitleRenderProgram::GlyphCompose, TitleRenderProgram::GlyphComposeAnim, TitleRenderProgram::Output, TitleRenderProgram::Blend}){
                const ShaderKey k = titleRenderProgramKey((uint32_t)p);
                if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
            }
            return keys;
        }

    private:
        struct Needed {
            std::string key;
            std::string text;           // the cluster
            std::shared_ptr<const TitleArtDoc> doc;     // the cell document with its character's and line's adjustments
            Json offset;                // the character's own offset (percent), or null
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
        bool prepared = false;
        bool hasPrebaked = false;
        TitleGlyphAtlasSnapshot prebaked;
        uint32_t splitLimit = 0;
        TitleStepCosts* costs = nullptr;
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
        double opacity = 1.0;               // the text layer's and its groups'
        int blendMode = 0;                  // TitleBlend of the text layer
        bool textVisible = true;
        std::vector<std::string> belowLayers, aboveLayers;
        uint64_t backPlateHash = 0, frontPlateHash = 0;
        bool needBackPlate = false, needFrontPlate = false;
        std::vector<Needed> needed;     // cells to render, in order
        std::set<std::string> stringKeys;
        struct Placed {
            std::string key;
            double x = 0, y = 0;        // the string glyph's position, output pixels
            // S85 R15: the glyph's pose (null: at rest) and its move in output pixels
            const GlyphPose* pose = nullptr;
            double move[6] = {1, 0, 0, 0, 1, 0};
        };
        std::vector<Placed> placed;     // the string's glyphs in order

        bool firstDrawWait = false;     // a chain of passes waits for a first draw

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

        // R15: the glyph's sampled values as a move in output pixels: about its pivot (the
        // ink box's centre, baseline, bottom or top), to the pivot shifted by the offset (% of
        // the layer's size, layer axes), as the editor's exact pose does
        void poseGlyph(Placed& glyph, const LaidOutCluster& cluster, const Json& textLayer) const{
            if (!spec.glyphPoses) return;
            auto found = spec.glyphPoses->find(cluster.index);
            if (found == spec.glyphPoses->end() || found->second.identity()) return;
            glyph.pose = &found->second;
            titleGlyphMove(found->second, cluster.inkBox, cluster.pen, textLayer.value("size", 200.0), [this](const TitleVec2& p){ return toOutput(p); }, glyph.move);
        }

        // the style: overrides, its hash (R32's file key), the cell document
        void setUpStyle(){
            const std::string& textLayer = spec.glyphPlan.textLayer;
            for (const FieldOverride& o : spec.overrides){
                const bool text = o.layerId == textLayer && o.path == "text";
                if (!text) styleOverrides.push_back(o);
                if (o.layerId == textLayer) layerOverrides.push_back(o);
            }
            styleHash = titleGlyphStyleHash(spec.docHash, spec.overrides, textLayer, spec.pxScale);
            atlas->mutableStats().styleHash = styleHash;
            cellDoc = atlas->cellDocument(styleHash);
            if (!cellDoc){
                cellDoc = makeCellDocument(*spec.doc, spec.glyphPlan);
                atlas->storeCellDocument(styleHash, cellDoc);
            }
        }

        // a layer member's value with the instance's override of it
        Json valueOf(const Json& layer, const std::string& id, const char* member, Json fallback) const{
            for (const FieldOverride& o : styleOverrides)
                if (o.layerId == id && o.path == member) return o.value;
            auto it = layer.find(member);
            return it != layer.end() ? *it : fallback;
        }

        // the adjustment entry of a character or a line (null when it has none)
        static Json entryOf(const Json& layer, const char* map, uint32_t index){
            auto list = layer.find(map);
            if (list == layer.end() || !list->is_object()) return Json();
            auto entry = list->find(std::to_string(index));
            return entry != list->end() && entry->is_object() ? *entry : Json();
        }

        // the cell document with a character's and its line's adjustments as the single glyph's
        // (chars."0", lines."0"; the character's offset moves the glyph through the cell's
        // offset instead), one per distinct adjustment
        std::shared_ptr<const TitleArtDoc> adjustedDocument(const Json& chars, const Json& line, const std::string& adjustKey){
            if (adjustKey.empty()) return cellDoc;
            const uint64_t key = hashCombine(styleHash, fnv1a64(adjustKey));
            if (auto known = atlas->cellDocument(key)) return known;
            auto doc = std::make_shared<TitleArtDoc>(*cellDoc);
            if (Json* layer = findLayer(doc->root, spec.glyphPlan.textLayer)){
                if (chars.is_object()) (*layer)["chars"] = Json{{"0", chars}};
                if (line.is_object()) (*layer)["lines"] = Json{{"0", line}};
            }
            normalizeTitleArt(*doc);
            atlas->storeCellDocument(key, doc);
            return doc;
        }

        // the state's layout, plates and cells
        void prepare(){
            prepared = true;
            const TitleArtDoc& doc = *spec.doc;
            const std::string& textLayer = spec.glyphPlan.textLayer;

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
            // R25, R28: the opacity of the text layer and its groups, its blend, its visibility
            for (const Json* step : chain){
                const std::string id = step->value("id", std::string());
                opacity *= valueOf(*step, id, "opacity", Json(1.0)).get<double>();
                textVisible = textVisible && valueOf(*step, id, "visible", Json(true)).get<bool>();
            }
            if (!chain.empty()){
                const std::string blend = valueOf(*chain.back(), textLayer, "blend", Json("normal")).get<std::string>();
                for (int b = 0; b < TITLE_BLEND_COUNT; b++)
                    if (blend == titleBlendName((TitleBlend)b)) blendMode = b;
            }
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
            // the string snaps to the pixel grid (its first pen to a whole pixel): S80 centres a
            // text by its ink, so every value moves the string by a fraction of a pixel; snapped,
            // a glyph keeps its phase on a fixed advance grid (a counter's tabular figures) and its
            // cell serves every value. The string sits under half a pixel from Realtime's place.
            double snapX = 0.0, snapY = 0.0;
            if (!laid.clusters.empty()){
                const TitleVec2 pen = toOutput(laid.clusters.front().pen);
                snapX = std::round(pen.x) - pen.x;
                snapY = std::round(pen.y) - pen.y;
            }
            atlas->mutableStats().snapX = snapX;
            atlas->mutableStats().snapY = snapY;
            std::set<std::string> queued;
            for (const LaidOutCluster& cluster : laid.clusters){
                if (cluster.glyphs.empty()) continue;
                bool ink = false;
                for (const LaidOutGlyph& g : cluster.glyphs) ink = ink || !g.outline.empty();
                if (!ink) continue;
                const std::string text = utf8Of(cluster.codepoints);
                // its character's and line's adjustments render with the glyph (scale, rotation,
                // face material; offsets move it like the string does)
                Json chars = entryOf(*layer, "chars", cluster.index);
                const Json line = entryOf(*layer, "lines", cluster.line);
                Json offset;
                if (chars.is_object() && chars.contains("offset")){
                    offset = chars["offset"];
                    chars.erase("offset");
                    if (chars.empty()) chars = Json();
                }
                std::string adjustKey;
                if (chars.is_object()) adjustKey += "chars" + chars.dump();
                if (line.is_object()) adjustKey += "lines" + line.dump();
                const std::shared_ptr<const TitleArtDoc> adjusted = adjustedDocument(chars, line, adjustKey);
                const Json* adjustedLayer = findLayer(adjusted->root, textLayer);
                if (!offset.is_null()) adjustKey += "offset" + offset.dump();
                const std::string base = hex(styleHash) + "|" + text + (adjustKey.empty() ? std::string() : "|" + hex(fnv1a64(adjustKey)));
                const TitleVec2 exact = toOutput(cluster.glyphs.front().position);
                const TitleVec2 at{exact.x + snapX, exact.y + snapY};
                // the single glyph's position: the same layer alone with this cluster as its text
                double ax = 0.0, ay = 0.0;
                if (!atlas->anchor(base, ax, ay)){
                    std::vector<FieldOverride> one = layerOverrides;
                    one.erase(std::remove_if(one.begin(), one.end(), [](const FieldOverride& o){ return o.path == "text"; }), one.end());
                    one.push_back(FieldOverride{textLayer, "text", Json(text)});
                    if (!offset.is_null()) one.push_back(FieldOverride{textLayer, "chars.0.offset", offset});
                    const LaidOutText& alone = single.layout(adjustedLayer ? *adjustedLayer : *cellLayer, one);
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
                Placed glyph{key, at.x, at.y};
                poseGlyph(glyph, cluster, *layer);
                placed.push_back(glyph);
                stringKeys.insert(key);
                if (atlas->find(key) || queued.count(key)) continue;
                needed.push_back({key, text, adjusted, offset, ax + qx / (double)PHASES, ay + qy / (double)PHASES, qx / (double)PHASES,
                                  qy / (double)PHASES});
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
            request.fxTier = spec.fxTier;
            request.testFlags = renderFlags;
            sub = renderer.begin(request);
            if (!sub){
                fail("the renderer refused the document");
                return false;
            }
            if (costs) sub->useCosts(costs);
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
                const Needed& n = needed[subCell];
                RenderRequest r;
                r.doc = n.doc;
                r.overrides = styleOverrides;
                r.overrides.push_back(FieldOverride{spec.glyphPlan.textLayer, "text", Json(n.text)});
                // the glyph moved by its own offset and the phase inside its layer (its character
                // offset, percent of the size): the layer stays where it is, so the renderer draws
                // the glyph at that phase as the string's render does, without resampling the layer
                double ox = n.offset.is_array() && n.offset.size() == 2 ? n.offset[0].get<double>() : 0.0;
                double oy = n.offset.is_array() && n.offset.size() == 2 ? n.offset[1].get<double>() : 0.0;
                if ((n.phaseX != 0.0 || n.phaseY != 0.0) && layerSize > 0.0){
                    const double s = canvasFrame.pxScale;
                    const double dx = n.phaseX / s, dy = -n.phaseY / s;
                    const double det = toDocument.a * toDocument.d - toDocument.c * toDocument.b;
                    if (std::abs(det) > 1e-12){
                        ox += (toDocument.d * dx - toDocument.c * dy) / det / layerSize * 100.0;
                        oy += (-toDocument.b * dx + toDocument.a * dy) / det / layerSize * 100.0;
                    }
                }
                if (ox != 0.0 || oy != 0.0) r.overrides.push_back(FieldOverride{spec.glyphPlan.textLayer, "chars.0.offset", Json::array({ox, oy})});
                r.glyphCell = GlyphCellRequest{spec.glyphPlan.textLayer, 0, GlyphCellRequest::Back};
                return begin(r, SubKind::Cell);
            }
            return false;
        }

        void finishSub(){
            if (subKind == SubKind::BackPlate || subKind == SubKind::FrontPlate){
                atlas->storePlate(subKind == SubKind::BackPlate ? backPlateHash : frontPlateHash, std::move(sub->resultImage()));
            }else if (subKind == SubKind::Cell){
                const Needed& n = needed[subCell];
                const TitleSplits& splits = sub->splits();
                bool full = false, deferred = false;
                std::string error;
                if (!atlas->insert(n.key, splits.glyphBack, splits.glyphFront, n.anchorX, n.anchorY, stringKeys, full, deferred, error)){
                    if (deferred){
                        // the cell is rendered but not in its page: the sub stays and the copies retry
                        firstDrawWait = true;
                        return;
                    }
                    if (full){
                        const std::string name = spec.label.rfind("TitleArt/", 0) == 0 ? spec.label.substr(9) : spec.label;
                        // `error` when the cell is wider than a page: R29's sentence is about the page count
                        fail("Title Art '" + name + "': " + (!error.empty() ? error : "the glyph atlas is full (" + std::to_string(atlas->maxPages()) + " pages of " +
                             std::to_string(atlas->pageSize()) + "). Raise Max Pages, lower Resolution Scale, or reduce the Characters."));
                    }else{
                        fail(error);
                    }
                }
            }
            workDone++;
            passesRun += sub->passesDone();
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

        bool copyInto(TitleTarget* into, TitleTarget* source, const TitleRectI& sourcePixels, float alpha, BlendMode blend, uint32_t& passes,
                      std::string& error){
            if (!source || !source->colorTexture()) return true;
            TitleGpuPass pass;
            pass.program = TitleRenderProgram::Copy;
            pass.target = into;
            pass.scissor = {0, 0, composeRect.width, composeRect.height};
            pass.blend = blend;
            pass.tex[0] = source->colorTexture();
            const float w = (float)std::max(1, sourcePixels.width), h = (float)std::max(1, sourcePixels.height);
            setMap(pass, 2, 1.0f / w, 1.0f / h, (float)(composeRect.x - sourcePixels.x) / w, (float)(composeRect.y - sourcePixels.y) / h);
            pass.params.set(3, alpha, 0.0f, 1.0f, 0.0f);
            passes++;
            return atlas->gpu().run(pass, error);
        }

        TargetRef target(ColorFormat format, std::string& error){
            TargetKey key;
            key.width = composeRect.width;
            key.height = composeRect.height;
            key.format = format;
            key.filter = TextureFilter::LINEAR;
            return atlas->pool().acquire(key, &error);
        }

        // the cell quads into `into`: every back cell, then every front cell (rims behind every face)
        bool drawCells(TitleTarget* into, std::set<std::string>& shown, uint32_t& passes, std::string& error){
            std::vector<Quad> quads;
            std::vector<Quad> fronts;
            const bool stringOrder = (spec.testFlags & TITLE_ART_TEST_GLYPH_STRING_ORDER) != 0;
            for (const Placed& p : placed){
                const TitleGlyphAtlas::Cell* cell = atlas->find(p.key);
                if (!cell || cell->blank) continue;
                shown.insert(p.key);
                const double dx = std::round(p.x - cell->anchorX), dy = std::round(p.y - cell->anchorY);
                // the pose moves output pixels; these quads sit in the compose target's
                auto posed = [&](Quad quad){
                    if (p.pose){
                        quad.pose = p.pose;
                        std::copy(p.move, p.move + 6, quad.move);
                        quad.move[2] -= composeRect.x - (quad.move[0] * composeRect.x + quad.move[1] * composeRect.y);
                        quad.move[5] -= composeRect.y - (quad.move[3] * composeRect.x + quad.move[4] * composeRect.y);
                    }
                    return quad;
                };
                if (!cell->back.empty())
                    quads.push_back(posed({cell->page, cell->back, cell->backPixels.x + dx - composeRect.x, cell->backPixels.y + dy - composeRect.y}));
                Quad front = posed({cell->page, cell->front, cell->frontPixels.x + dx - composeRect.x, cell->frontPixels.y + dy - composeRect.y});
                if (!cell->front.empty()) (stringOrder ? quads : fronts).push_back(front);
            }
            quads.insert(quads.end(), fronts.begin(), fronts.end());
            const bool anyPose = std::any_of(quads.begin(), quads.end(), [](const Quad& q){ return q.pose != nullptr; });
            if (anyPose) return drawPosedCells(into, quads, passes, error);
            const float side = (float)atlas->pageSize();
            for (size_t i = 0; i < quads.size();){
                // a run of quads of one page, at most QUADS_PER_PASS
                const int page = quads[i].page;
                size_t end = i;
                while (end < quads.size() && quads[end].page == page && end - i < (size_t)QUADS_PER_PASS) end++;
                TitleGpuPass pass;
                pass.program = TitleRenderProgram::GlyphCompose;
                pass.target = into;
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
                if (!pass.tex[0]){
                    error = "a glyph atlas page is gone";
                    return false;
                }
                if (!atlas->gpu().run(pass, error)) return false;
                i = end;
            }
            return true;
        }

        // S85 R15: the quads with their poses
        bool drawPosedCells(TitleTarget* into, const std::vector<Quad>& quads, uint32_t& passes, std::string& error){
            const float side = (float)atlas->pageSize();
            std::vector<TitlePosedQuad> posed;
            for (const Quad& quad : quads){
                TitlePosedQuad p;
                TitleTarget* page = atlas->pageTarget(quad.page);
                p.texture = page ? page->colorTexture() : nullptr;
                p.uv[0] = quad.cell.x / side;
                p.uv[1] = quad.cell.y / side;
                p.uv[2] = (quad.cell.x + quad.cell.width) / side;
                p.uv[3] = (quad.cell.y + quad.cell.height) / side;
                p.x0 = quad.x0;
                p.y0 = quad.y0;
                p.width = quad.cell.width;
                p.height = quad.cell.height;
                std::copy(quad.move, quad.move + 6, p.move);
                if (quad.pose){
                    p.opacity = (float)quad.pose->opacity;
                    for (int k = 0; k < 3; k++){
                        p.tint[k] = srgbToLinear((float)quad.pose->tint[k]);
                        p.flash[k] = srgbToLinear((float)quad.pose->flash[k]) * (float)quad.pose->flashAmount;
                    }
                }
                posed.push_back(p);
            }
            return drawTitlePosedQuads(atlas->gpu(), into, posed, composeRect.width, composeRect.height, passes, error);
        }

        // R28: back plate, back cells, front cells, front plate, then the output pass; a text layer
        // with an opacity or a blend composes its glyphs apart, then over the back plate
        bool compose(StepReport& report){
            const bool apart = blendMode != (int)TitleBlend::Normal || opacity < 1.0;
            for (TitleRenderProgram p : {TitleRenderProgram::Copy, TitleRenderProgram::GlyphCompose, TitleRenderProgram::GlyphComposeAnim, TitleRenderProgram::Output})
                if (!ensure(p)) return false;
            if (blendMode != (int)TitleBlend::Normal && !ensure(TitleRenderProgram::Blend)) return false;
            blocked = false;
            std::string error;
            const ColorFormat accFormat = caps.format(TitleBuffer::Accumulator).format;
            accumulator = target(accFormat, error);
            if (accumulator) display = target(ColorFormat::RGBA, error);
            if (!accumulator || !display){
                fail(error.empty() ? std::string("out_of_target_memory: the Glyph Atlas compose target") : error);
                return false;
            }
            TitleTarget* acc = accumulator.get();
            uint32_t passes = 0;
            const uint64_t deferrals = atlas->gpu().deferrals();
            const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (!atlas->gpu().clear(acc, transparent, error)) return failWith(error);
            if (const TitleImage* back = atlas->plate(backPlateHash))
                if (!copyInto(acc, back->target.get(), back->pixels, 1.0f, BlendMode::PREMULTIPLIED, passes, error)) return failWith(error);

            std::set<std::string> shown;
            if (textVisible && !apart){
                if (!drawCells(acc, shown, passes, error)) return failWith(error);
            }else if (textVisible){
                TargetRef glyphs = target(accFormat, error);
                if (!glyphs) return failWith(error);
                if (!atlas->gpu().clear(glyphs.get(), transparent, error)) return failWith(error);
                if (!drawCells(glyphs.get(), shown, passes, error)) return failWith(error);
                const TitleRectI whole = composeRect;
                if (blendMode == (int)TitleBlend::Normal){
                    if (!copyInto(acc, glyphs.get(), whole, (float)opacity, BlendMode::PREMULTIPLIED, passes, error)) return failWith(error);
                }else{
                    // S80's blend pass over a copy of the backdrop (blending off)
                    TargetRef backdrop = target(accFormat, error);
                    if (!backdrop) return failWith(error);
                    if (!copyInto(backdrop.get(), acc, whole, 1.0f, BlendMode::NONE, passes, error)) return failWith(error);
                    TitleGpuPass blend;
                    blend.program = TitleRenderProgram::Blend;
                    blend.target = acc;
                    blend.scissor = {0, 0, composeRect.width, composeRect.height};
                    blend.blend = BlendMode::NONE;
                    blend.tex[0] = backdrop.get()->colorTexture();
                    blend.tex[1] = glyphs.get()->colorTexture();
                    const float w = 1.0f / (float)composeRect.width, h = 1.0f / (float)composeRect.height;
                    setMap(blend, 2, w, h, 0.0f, 0.0f);
                    setMap(blend, 3, w, h, 0.0f, 0.0f);
                    blend.params.set(5, (float)blendMode, (float)opacity, 0.0f);
                    passes++;
                    if (!atlas->gpu().run(blend, error)) return failWith(error);
                }
            }
            if (const TitleImage* front = atlas->plate(frontPlateHash))
                if (!copyInto(acc, front->target.get(), front->pixels, 1.0f, BlendMode::PREMULTIPLIED, passes, error)) return failWith(error);

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
            if (atlas->gpu().deferrals() != deferrals){
                // a pass of the chain did not draw: the image would have a hole. The compose runs
                // again from the top (the targets are cleared, new keys wait their frames)
                firstDrawWait = true;
                blocked = true;
                return false;
            }
            accumulator.reset();
            atlas->setShown(std::move(shown));
            passesRun += passes;
            atlas->mutableStats().composes++;
            atlas->mutableStats().composePasses = passes + 1;    // and the clear
            if (atlas->stats().composes == 1){
                // R32: what a shipped counter's first value cost (0 cell renders with its prebake)
                Log::print("Title Art '%s': the glyph atlas showed its first string after %llu cell renders (%llu cells from the build's prebaked atlas)",
                           spec.label.c_str(), (unsigned long long)atlas->stats().cellRenders, (unsigned long long)atlas->stats().restoredCells);
            }
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
