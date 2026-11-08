// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The composition of a frame (docs/specs/S44-aseprite-import.md R8), ported from the render
// plan of the Aseprite Render Library and the Aseprite Document Library:
//   Aseprite Render Library (src/render/render.cpp), Copyright (c) 2019-2025 Igara Studio S.A.,
//   Copyright (c) 2001-2018 David Capello
//   Aseprite Document Library (src/doc/render_plan.cpp, src/doc/blend_internals.h),
//   Copyright (c) 2023-present Igara Studio S.A.
//   Both released under the terms of the MIT license. NOTICE.md lists them.
// Only what an export draws: the sprite's own colour mode as the destination (RGBA, gray+alpha
// or indices; indexed frames are composed as indices and looked up at the end, as an indexed
// --sheet PNG is), the background pass then the transparent one, cel x layer opacity through
// MUL_UN8, z-index ordering (NOTE.5), groups composed in a cleared image with their opacity and
// blend when the header says groups have them, tilemap cels from their tileset with flips. No
// onion skin, no UI opacity, no scaling.

#include "AsepriteBlend.h"
#include "AsepriteDocument.h"

#include <algorithm>
#include <cstring>

namespace jasperengine::editor::pixelart::aseprite {

    namespace {

        // the destination: one value per pixel in the sprite's own format
        struct Canvas {
            int w = 0, h = 0;
            std::vector<uint32_t> px;   // rgba / graya / index
        };

        struct Item {
            int order = 0;
            int layer = -1;
            const Cel* record = nullptr;   // the cel record of the frame (a link keeps its own z-index)
            int zIndex() const { return record ? record->zIndex : 0; }
        };

        class Composer {
        public:
            Composer(const Document& doc, int frame, const RenderOptions& options): doc(doc), frame(frame), options(options) {
                composeGroups = options.groups == GroupMode::Compose ||
                                (options.groups == GroupMode::FromFile && (doc.headerFlags & kHeaderGroupBlend) != 0);
                palette = doc.paletteRgba(frame);
                maskColor = doc.mode == ColorMode::Indexed ? doc.transparentIndex : 0;
                selectLayers();
            }

            PixelImage run();

        private:
            const Document& doc;
            int frame;
            RenderOptions options;
            bool composeGroups = false;
            std::vector<uint8_t> palette;
            uint32_t maskColor = 0;
            std::vector<bool> renderable;   // per layer

            void selectLayers();
            bool isRenderable(int layer) const { return layer >= 0 && layer < static_cast<int>(renderable.size()) && renderable[static_cast<size_t>(layer)]; }

            void addLayer(std::vector<Item>& items, int& order, int layer);
            std::vector<Item> plan(int group);
            void renderPlan(const std::vector<Item>& items, Canvas& dst, bool background, bool transparent);
            void renderCel(Canvas& dst, const Layer& layer, const Cel& record, const Cel& source, int opacity);
            void blendPixel(uint32_t& dst, uint32_t src, int opacity, blend::Func func) const;
            blend::Func blender(BlendMode mode) const;
            uint32_t sourcePixel(const PixelImage& image, int x, int y) const;
            void drawImage(Canvas& dst, const PixelImage& image, int ox, int oy, int opacity, blend::Func func, uint32_t flips,
                           uint32_t xFlipMask, uint32_t yFlipMask, uint32_t dFlipMask);
        };

        void Composer::selectLayers() {
            const int n = static_cast<int>(doc.layers.size());
            renderable.assign(static_cast<size_t>(n), false);
            std::vector<int> selection = options.layers;
            if (options.soloLayer >= 0) selection = {options.soloLayer};
            std::vector<bool> selected(static_cast<size_t>(n), false);
            for (int l : selection) {
                if (l >= 0 && l < n) selected[static_cast<size_t>(l)] = true;
            }
            for (int l = 0; l < n; l++) {
                const Layer& layer = doc.layers[static_cast<size_t>(l)];
                if (layer.isGroup()) continue;
                if (layer.reference() && !options.includeReference) continue;
                if (static_cast<uint16_t>(layer.type) > static_cast<uint16_t>(LayerType::Tilemap)) continue;
                bool on;
                if (selection.empty()) {
                    on = options.includeHidden || doc.visibleHierarchy(l);
                } else if (selected[static_cast<size_t>(l)]) {
                    // a layer asked for by name shows even when hidden (Aseprite's --layer)
                    on = true;
                } else {
                    // under a selected group: its own visibility (and its groups' below that one)
                    on = false;
                    bool visibleSoFar = options.includeHidden || layer.visible();
                    for (int p = layer.parent, guard = 0; p >= 0 && guard <= n; p = doc.layers[static_cast<size_t>(p)].parent, guard++) {
                        if (selected[static_cast<size_t>(p)]) {
                            on = visibleSoFar;
                            break;
                        }
                        visibleSoFar = visibleSoFar && (options.includeHidden || doc.layers[static_cast<size_t>(p)].visible());
                    }
                }
                renderable[static_cast<size_t>(l)] = on;
            }
            // a group is drawn when something under it is
            for (int l = n; l-- > 0;) {
                if (!renderable[static_cast<size_t>(l)]) continue;
                for (int p = doc.layers[static_cast<size_t>(l)].parent, guard = 0; p >= 0 && guard <= n; p = doc.layers[static_cast<size_t>(p)].parent, guard++) {
                    renderable[static_cast<size_t>(p)] = true;
                }
            }
        }

        // RenderPlan::addLayer: every layer takes an order number (hidden ones too, so z-indexes
        // count them); groups are flattened into their children unless they are composed
        void Composer::addLayer(std::vector<Item>& items, int& order, int layer) {
            ++order;
            const Layer& l = doc.layers[static_cast<size_t>(layer)];
            if (l.isGroup() && !composeGroups) {
                for (int child : doc.children(layer)) addLayer(items, order, child);
                return;
            }
            items.push_back({order, layer, l.isGroup() ? nullptr : doc.celRecord(layer, frame)});
        }

        std::vector<Item> Composer::plan(int group) {
            std::vector<Item> items;
            int order = group < 0 ? 1 : 0;   // the root takes the first number
            for (int child : doc.children(group)) addLayer(items, order, child);
            bool hasZ = false;
            if (options.zIndex) {
                for (const Item& item : items) hasZ = hasZ || item.zIndex() != 0;
            }
            if (hasZ) {
                for (Item& item : items) item.order += item.zIndex();
                std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
                    return a.order < b.order || (a.order == b.order && a.zIndex() < b.zIndex());
                });
            }
            // hidden layers were only needed for the z order
            std::vector<Item> visible;
            for (const Item& item : items) {
                if (isRenderable(item.layer)) visible.push_back(item);
            }
            return visible;
        }

        blend::Func Composer::blender(BlendMode mode) const {
            int m = static_cast<int>(mode);
            if (m < 0 || m > static_cast<int>(BlendMode::Divide)) m = 0;
            const blend::Mode bm = static_cast<blend::Mode>(m);
            return doc.mode == ColorMode::Grayscale ? blend::grayaBlender(bm, options.newBlend)
                                                    : blend::rgbaBlender(bm, options.newBlend, options.legacyHsl);
        }

        uint32_t Composer::sourcePixel(const PixelImage& image, int x, int y) const {
            const size_t i = static_cast<size_t>(y) * image.width() + x;
            switch (doc.mode) {
                case ColorMode::Indexed: return image.hasIndices() ? image.indices()[i] : 0;
                case ColorMode::Grayscale: {
                    const uint8_t* p = image.pixels().data() + i * 4;
                    return blend::graya(p[0], p[3]);
                }
                case ColorMode::Rgba: return blend::pack(image.pixels().data() + i * 4);
            }
            return 0;
        }

        // BlenderHelper: the source's mask colour is skipped; indexed onto indexed copies the index
        void Composer::blendPixel(uint32_t& dst, uint32_t src, int opacity, blend::Func func) const {
            if (src == maskColor) return;
            if (doc.mode == ColorMode::Indexed) {
                if (src < palette.size() / 4) dst = src;
                return;
            }
            dst = func(dst, src, opacity);
        }

        void Composer::drawImage(Canvas& dst, const PixelImage& image, int ox, int oy, int opacity, blend::Func func, uint32_t flips,
                                 uint32_t xFlipMask, uint32_t yFlipMask, uint32_t dFlipMask) {
            const int w = image.width();
            const int h = image.height();
            const bool xf = xFlipMask && (flips & xFlipMask) == xFlipMask;
            const bool yf = yFlipMask && (flips & yFlipMask) == yFlipMask;
            const bool df = dFlipMask && (flips & dFlipMask) == dFlipMask;
            const int x0 = std::max(0, ox), y0 = std::max(0, oy);
            const int x1 = std::min(dst.w, ox + w), y1 = std::min(dst.h, oy + h);
            const int square = std::min(w, h);
            if (doc.mode == ColorMode::Rgba && !xf && !yf && !df) {
                // the common case, row by row without the per-pixel mode switch; an opaque source
                // pixel at full opacity in Normal mode is the result itself
                const bool normal = func == blender(BlendMode::Normal);
                const uint8_t* rows = image.pixels().data();
                for (int y = y0; y < y1; y++) {
                    const uint8_t* p = rows + (static_cast<size_t>(y - oy) * w + (x0 - ox)) * 4;
                    uint32_t* d = dst.px.data() + static_cast<size_t>(y) * dst.w + x0;
                    for (int x = x0; x < x1; x++, p += 4, d++) {
                        const uint32_t src = blend::pack(p);
                        if (src == maskColor) continue;
                        if (normal && opacity == 255 && p[3] == 255) {
                            *d = src;
                        } else {
                            *d = func(*d, src, opacity);
                        }
                    }
                }
                return;
            }
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    int sx = x - ox, sy = y - oy;
                    if (xf) sx = w - 1 - sx;
                    if (yf) sy = h - 1 - sy;
                    uint32_t& d = dst.px[static_cast<size_t>(y) * dst.w + x];
                    if (df) {
                        std::swap(sx, sy);
                        // composite_image_general_with_tile_flags clears what a transposed
                        // non-square tile does not cover
                        if (sx >= square || sy >= square) {
                            d = 0;
                            continue;
                        }
                    }
                    blendPixel(d, sourcePixel(image, sx, sy), opacity, func);
                }
            }
        }

        void Composer::renderCel(Canvas& dst, const Layer& layer, const Cel& record, const Cel& source, int opacity) {
            const blend::Func func = blender(layer.blend);
            if (source.tilemap) {
                const Tilemap& tm = *source.tilemap;
                const Tileset* ts = nullptr;
                for (const Tileset& t : doc.tilesets) {
                    if (t.id == layer.tilesetIndex) ts = &t;
                }
                if (!ts || ts->tiles.empty()) return;
                // a tilemap built in memory may not hold columns x rows tiles (a read one always does)
                if (tm.columns < 0 || tm.rows < 0 || tm.tiles.size() < static_cast<size_t>(tm.columns) * static_cast<size_t>(tm.rows)) return;
                const uint32_t flagMask = tm.xFlipMask | tm.yFlipMask | tm.diagonalMask;
                uint32_t idShift = 0;
                while (tm.idMask && idShift < 32 && !((tm.idMask >> idShift) & 1)) idShift++;
                for (int v = 0; v < tm.rows; v++) {
                    for (int u = 0; u < tm.columns; u++) {
                        const uint32_t raw = tm.at(u, v);
                        uint32_t index;
                        if (ts->zeroIsEmpty()) {
                            index = (raw & tm.idMask) >> idShift;
                            if (index == 0 && (raw & flagMask) == 0) continue;   // notile
                        } else {
                            if (raw == 0xffffffffu) continue;                     // the old empty tile
                            index = (raw & tm.idMask) >> idShift;
                        }
                        if (index >= static_cast<uint32_t>(ts->tileCount)) continue;
                        const PixelImage tile = ts->tile(static_cast<int>(index));
                        drawImage(dst, tile, record.x + u * ts->tileWidth, record.y + v * ts->tileHeight, opacity, func, raw & flagMask,
                                  tm.xFlipMask, tm.yFlipMask, tm.diagonalMask);
                    }
                }
                return;
            }
            if (source.image && !source.image->empty()) {
                drawImage(dst, *source.image, record.x, record.y, opacity, func, 0, 0, 0, 0);
            }
        }

        void Composer::renderPlan(const std::vector<Item>& items, Canvas& dst, bool background, bool transparent) {
            for (const Item& item : items) {
                const Layer& layer = doc.layers[static_cast<size_t>(item.layer)];
                if (layer.isGroup()) {
                    // a composed group: its children drawn in a cleared image, then that image
                    // composited with the group's opacity and blend mode
                    Canvas group;
                    group.w = dst.w;
                    group.h = dst.h;
                    group.px.assign(dst.px.size(), doc.mode == ColorMode::Indexed ? maskColor : 0);
                    renderPlan(plan(item.layer), group, background, transparent);
                    const blend::Func func = blender(layer.blend);
                    for (size_t i = 0; i < dst.px.size(); i++) blendPixel(dst.px[i], group.px[i], layer.opacity, func);
                    continue;
                }
                if ((!background && layer.background()) || (!transparent && !layer.background())) continue;
                if (!item.record) continue;
                // a link draws its target's pixels at its own position, opacity and z-index
                const Cel* source = item.record;
                if (source->linkedFrame >= 0) {
                    source = doc.cel(item.layer, frame);
                    if (!source) continue;
                }
                const int opacity = blend::mulUn8(item.record->opacity, layer.opacity);
                renderCel(dst, layer, *item.record, *source, opacity);
            }
        }

        PixelImage Composer::run() {
            Canvas canvas;
            canvas.w = doc.width;
            canvas.h = doc.height;
            // the new blending method clears the destination with the background colour: the
            // transparent index for indexed sprites, transparent black otherwise
            canvas.px.assign(static_cast<size_t>(doc.width) * doc.height, doc.mode == ColorMode::Indexed ? maskColor : 0);
            const std::vector<Item> items = plan(-1);
            renderPlan(items, canvas, true, false);
            renderPlan(items, canvas, false, true);

            const size_t n = canvas.px.size();
            if (doc.mode == ColorMode::Indexed) {
                std::vector<uint8_t> indices(n);
                for (size_t i = 0; i < n; i++) indices[i] = static_cast<uint8_t>(canvas.px[i]);
                // an indexed sheet's transparent index is clear (its PNG gives it alpha 0), unless a
                // background layer is drawn: its pixels are opaque whatever their index, as Aseprite
                // exports them
                bool backgroundDrawn = false;
                for (size_t l = 0; l < doc.layers.size() && l < renderable.size(); l++) {
                    backgroundDrawn = backgroundDrawn || (doc.layers[l].background() && renderable[l]);
                }
                PixelImage image = PixelImage::fromIndices(doc.width, doc.height, std::move(indices), palette,
                                                           backgroundDrawn ? -1 : doc.transparentIndex);
                if (!options.keepIndices) image.dropIndices();
                return image;
            }
            PixelImage image(doc.width, doc.height);
            std::span<uint8_t> out = image.editPixels();
            for (size_t i = 0; i < n; i++) {
                const uint32_t c = canvas.px[i];
                uint8_t* p = out.data() + i * 4;
                if (doc.mode == ColorMode::Grayscale) {
                    p[0] = p[1] = p[2] = static_cast<uint8_t>(blend::grayV(c));
                    p[3] = static_cast<uint8_t>(blend::grayA(c));
                } else {
                    blend::unpack(c, p);
                }
            }
            return image;
        }

    }

    PixelImage renderFrame(const Document& document, int frame, const RenderOptions& options) {
        if (frame < 0 || frame >= document.frameCount() || document.width < 1 || document.height < 1) return PixelImage();
        Composer composer(document, frame, options);
        return composer.run();
    }

    PixelImage renderTilemap(const Document& document, const Tilemap& tilemap, uint32_t tilesetIndex) {
        const Tileset* ts = nullptr;
        for (const Tileset& t : document.tilesets) {
            if (t.id == tilesetIndex) ts = &t;
        }
        if (!ts || tilemap.columns < 1 || tilemap.rows < 1) return PixelImage();
        // a one-layer, one-frame document holding just this tilemap, drawn by the composer
        Document doc;
        // in 64 bits: 65535 columns of 65535-pixel tiles overflow an int
        doc.width = static_cast<int>(std::min<int64_t>(kMaxCanvasSide, static_cast<int64_t>(tilemap.columns) * ts->tileWidth));
        doc.height = static_cast<int>(std::min<int64_t>(kMaxCanvasSide, static_cast<int64_t>(tilemap.rows) * ts->tileHeight));
        if (doc.width < 1 || doc.height < 1) return PixelImage();
        doc.mode = document.mode;
        doc.transparentIndex = document.transparentIndex;
        doc.palette = document.palette;
        doc.tilesets = {*ts};
        Layer layer;
        layer.type = LayerType::Tilemap;
        layer.tilesetIndex = tilesetIndex;
        doc.layers = {layer};
        doc.frameDurations = {100};
        Cel cel;
        cel.tilemap = std::make_shared<Tilemap>(tilemap);
        doc.cels = {cel};
        return renderFrame(doc, 0);
    }

}
